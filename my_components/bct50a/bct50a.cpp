#include "bct50a.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <iomanip>
#include "esphome/core/log.h"
#include "esp_http_server.h"
#include "bct50a_html.h"
#include "esp_heap_caps.h"
#include <cctype>
#include <cstdlib>
#include <limits>
#include <vector>
#include "esphome/components/time/real_time_clock.h"

#include "esp_littlefs.h"
#include "esp_partition.h"
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <dirent.h>
#include <cerrno>

#include "esp_wifi.h"
#include "esphome/components/wifi/wifi_component.h"

namespace {
static constexpr uint32_t HISTORY_FILE_MAGIC = 0x42544352; // "BTCR"
static constexpr uint16_t HISTORY_FILE_VERSION = 1;
static constexpr char HISTORY_DIR[] = "/bct_fs/history";
struct __attribute__((packed)) HistoryDiskHeader {
  uint32_t magic;
  uint16_t version;
  uint16_t header_size;
  uint32_t id;
  uint32_t cycle_step;
  char mode[16];
  char status[16];
  char start_time[32];
  char end_time[32];
  float set_current_a;
  float set_voltage_v;
  float termination_current_a;
  float final_capacity_ah;
  float final_energy_wh;
  uint32_t runtime_seconds;
  uint32_t sample_count;
};
struct __attribute__((packed)) HistoryDiskSample {
  uint32_t elapsed_ms;
  float voltage_v;
  float current_a;
  float capacity_ah;
  float energy_wh;
};
static_assert(sizeof(HistoryDiskSample) == 20, "Unexpected history sample disk layout");

}



namespace esphome {
namespace bct50a {

static const char *const TAG = "bct50a";
static BCT50AComponent *http_component = nullptr;

namespace {

struct CycleJsonValue {
  enum Type { OBJECT, ARRAY, STRING, NUMBER, OTHER } type{OTHER};
  std::string string_value;
  double number_value{0};
  std::vector<std::pair<std::string, CycleJsonValue>> object;
  std::vector<CycleJsonValue> array;
  const CycleJsonValue *get(const char *key) const {
    for (const auto &entry : object) {
      if (entry.first == key)
        return &entry.second;
    }
    return nullptr;
  }
};


class CycleJsonParser {
 public:
  explicit CycleJsonParser(const std::string &input) : input_(input) {}
  bool parse(CycleJsonValue &value) {
    skip_ws_();
    if (!parse_value_(value, 0))
      return false;
    skip_ws_();
    return pos_ == input_.size();
  }
 private:
  const std::string &input_;
  size_t pos_{0};
  void skip_ws_() {
    while (pos_ < input_.size() &&
           std::isspace(static_cast<unsigned char>(input_[pos_]))) {
      ++pos_;
    }
  }
  bool parse_string_(std::string &out) {
    if (pos_ >= input_.size() || input_[pos_++] != '"')
      return false;
    out.clear();
    while (pos_ < input_.size()) {
      char c = input_[pos_++];
      if (c == '"')
        return true;
      if (static_cast<unsigned char>(c) < 0x20)
        return false;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (pos_ >= input_.size())
        return false;
      c = input_[pos_++];
      switch (c) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (pos_ + 4 > input_.size())
            return false;
          unsigned code = 0;
          for (int i = 0; i < 4; ++i) {
            char h = input_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9')
              code |= h - '0';
            else if (h >= 'a' && h <= 'f')
              code |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F')
              code |= h - 'A' + 10;
            else
              return false;
          }
          // These request fields are ASCII. Reject escaped
          // non-ASCII values rather than mis-encoding UTF-8.
          if (code > 0x7F)
            return false;
          out += static_cast<char>(code);
          break;
        }
        default:
          return false;
      }
    }
    return false;
  }
  bool parse_value_(CycleJsonValue &out, unsigned depth) {
    if (depth > 16 || pos_ >= input_.size())
      return false;
    skip_ws_();
    if (pos_ >= input_.size())
      return false;
    char c = input_[pos_];
    if (c == '{') {
      ++pos_;
      out.type = CycleJsonValue::OBJECT;
      skip_ws_();
      if (pos_ < input_.size() && input_[pos_] == '}') {
        ++pos_;
        return true;
      }
      while (pos_ < input_.size()) {
        skip_ws_();
        std::string key;
        if (!parse_string_(key))
          return false;
        skip_ws_();
        if (pos_ >= input_.size() || input_[pos_++] != ':')
          return false;
        CycleJsonValue child;
        if (!parse_value_(child, depth + 1))
          return false;
        out.object.emplace_back(key, std::move(child));
        skip_ws_();
        if (pos_ >= input_.size())
          return false;
        if (input_[pos_] == '}') {
          ++pos_;
          return true;
        }
        if (input_[pos_++] != ',')
          return false;
      }
      return false;
    }
    if (c == '[') {
      ++pos_;
      out.type = CycleJsonValue::ARRAY;
      skip_ws_();
      if (pos_ < input_.size() && input_[pos_] == ']') {
        ++pos_;
        return true;
      }
      while (pos_ < input_.size()) {
        CycleJsonValue child;
        if (!parse_value_(child, depth + 1))
          return false;
        out.array.emplace_back(std::move(child));
        skip_ws_();
        if (pos_ >= input_.size())
          return false;
        if (input_[pos_] == ']') {
          ++pos_;
          return true;
        }
        if (input_[pos_++] != ',')
          return false;
      }
      return false;
    }
    if (c == '"') {
      out.type = CycleJsonValue::STRING;
      return parse_string_(out.string_value);
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      const size_t start = pos_;
      if (input_[pos_] == '-')
        ++pos_;
      if (pos_ >= input_.size())
        return false;
      if (input_[pos_] == '0') {
        ++pos_;
        if (pos_ < input_.size() &&
            std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
          return false;
        }
      } else {
        if (input_[pos_] < '1' || input_[pos_] > '9')
          return false;
        while (pos_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
          ++pos_;
        }
      }
      if (pos_ < input_.size() && input_[pos_] == '.') {
        ++pos_;
        const size_t fraction_start = pos_;
        while (pos_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
          ++pos_;
        }
        if (pos_ == fraction_start)
          return false;
      }
      if (pos_ < input_.size() &&
          (input_[pos_] == 'e' || input_[pos_] == 'E')) {
        ++pos_;
        if (pos_ < input_.size() &&
            (input_[pos_] == '+' || input_[pos_] == '-')) {
          ++pos_;
        }
        const size_t exponent_start = pos_;
        while (pos_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[pos_]))) {
          ++pos_;
        }
        if (pos_ == exponent_start)
          return false;
      }
      char *end = nullptr;
      out.number_value = std::strtod(input_.c_str() + start, &end);
      if (end != input_.c_str() + pos_ ||
          !std::isfinite(out.number_value)) {
        return false;
      }
      out.type = CycleJsonValue::NUMBER;
      return true;
    }
    for (const char *literal : {"true", "false", "null"}) {
      const size_t len = std::strlen(literal);
      if (input_.compare(pos_, len, literal) == 0) {
        pos_ += len;
        out.type = CycleJsonValue::OTHER;
        return true;
      }
    }
    return false;
  }
};


bool cycle_json_number(const CycleJsonValue *value, double &out) {
  if (value == nullptr || value->type != CycleJsonValue::NUMBER)
    return false;
  out = value->number_value;
  return std::isfinite(out);
}


bool cycle_json_integer(const CycleJsonValue *value, double minimum, double maximum, uint32_t &out) {
  double number;
  if (!cycle_json_number(value, number) || number < minimum || number > maximum || std::floor(number) != number) {
    return false;
  }
  out = static_cast<uint32_t>(number);
  return true;
}


}  // namespace



////// storage helper
// Returns ESP_OK if the partition was read successfully.
// Sets erased=true only if every byte in the partition is 0xFF.
static esp_err_t littlefs_partition_is_erased_( const esp_partition_t *partition, bool &erased ) {
  erased = true;
  if (partition == nullptr) { return ESP_ERR_NOT_FOUND; }
  uint8_t buffer[256];
  for (size_t offset = 0; offset < partition->size; offset += sizeof(buffer)) {
    const size_t bytes_to_read = std::min<size_t>(sizeof(buffer), partition->size - offset);
    esp_err_t err = esp_partition_read(
        partition,
        offset,
        buffer,
        bytes_to_read
    );
    if (err != ESP_OK) { return err; }
    for (size_t i = 0; i < bytes_to_read; i++) {
      if (buffer[i] != 0xFF) {
        erased = false;
        return ESP_OK;
      }
    }
  }
  return ESP_OK;
}


esp_err_t BCT50AComponent::format_storage() {
  // Never format while a test or cycle is active.
  if (test_running_ || cycle_running_) {
    ESP_LOGW(TAG, "LittleFS format refused: test or cycle is active");
    return ESP_ERR_INVALID_STATE;
  }
  ESP_LOGW(TAG, "Starting manual LittleFS format");
  // Unmount first if currently mounted.
  if (littlefs_mounted_) {
    esp_err_t err = esp_vfs_littlefs_unregister("bct_fs");
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "LittleFS unmount failed: %s", esp_err_to_name(err));
      return err;
    }
    littlefs_mounted_ = false;
    ESP_LOGI(TAG, "LittleFS unmounted");
  }
  // Format the partition.
  esp_err_t format_err = esp_littlefs_format("bct_fs");
  if (format_err == ESP_OK) {
    ESP_LOGI(TAG, "LittleFS format successful");
  } else {
    ESP_LOGE(TAG, "LittleFS format failed: %s", esp_err_to_name(format_err));
  }
  // Attempt to mount again, even if formatting failed.
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/bct_fs";
  conf.partition_label = "bct_fs";
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  esp_err_t mount_err = esp_vfs_littlefs_register(&conf);
  if (mount_err == ESP_OK) {
    littlefs_mounted_ = true;
    ESP_LOGI(TAG, "LittleFS remounted successfully");
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    esp_err_t info_err = esp_littlefs_info("bct_fs", &total_bytes, &used_bytes);
    if (info_err == ESP_OK) {
      ESP_LOGI(TAG, "LittleFS after format: total=%u, used=%u, free=%u bytes",
               static_cast<unsigned>(total_bytes),
               static_cast<unsigned>(used_bytes),
               static_cast<unsigned>(total_bytes - used_bytes));
    }
  } else {
    littlefs_mounted_ = false;
    ESP_LOGE(TAG, "LittleFS remount failed: %s", esp_err_to_name(mount_err));
  }
  // Report the formatting error first, otherwise the mount result.
  return format_err != ESP_OK ? format_err : mount_err;
}


bool BCT50AComponent::history_storage_init_() {
  if (!littlefs_mounted_) {
    ESP_LOGW(TAG, "History storage unavailable: LittleFS not mounted");
    history_recalculate_next_id_();
    return false;
  }
  if (mkdir(HISTORY_DIR, 0775) != 0 && errno != EEXIST) {
    ESP_LOGE(TAG, "Cannot create history directory: %s", strerror(errno));
    history_recalculate_next_id_();
    return false;
  }
  ESP_LOGI(TAG, "History storage directory ready");
  history_recalculate_next_id_();
  return true;
}


void BCT50AComponent::history_recalculate_next_id_() {
  uint32_t max_id = 0;
  // Include records currently retained in PSRAM.
  if (history_records_ != nullptr) {
    for (size_t i = 0; i < HISTORY_MAX_RECORDS; i++) {
      const TestRecord &record = history_records_[i];
      if (record.used && record.id > max_id) {
        max_id = record.id;
      }
    }
  }
  // Include completed records in LittleFS.
  if (littlefs_mounted_) {
    DIR *dir = opendir(HISTORY_DIR);
    if (dir != nullptr) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr) {
        const char *name = entry->d_name;
        const size_t name_len = strlen(name);
        if (name_len < 5 ||
            strcmp(name + name_len - 4, ".rec") != 0) {
          continue;
        }
        char path[320];
        int path_len = snprintf( path, sizeof(path), "%s/%s", HISTORY_DIR, name );
        if (path_len < 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
          ESP_LOGW(TAG, "Skipping overlong history filename");
          continue;
        }
        FILE *file = fopen(path, "rb");
        if (file == nullptr) {
          continue;
        }
        HistoryDiskHeader header{};
        const bool header_read = fread(&header, sizeof(header), 1, file) == 1;
        fclose(file);
        if (!header_read ||
            header.magic != HISTORY_FILE_MAGIC ||
            header.version != HISTORY_FILE_VERSION ||
            header.header_size != sizeof(HistoryDiskHeader) ||
            header.id == 0) {
          continue;
        }
        struct stat st{};
        if (stat(path, &st) != 0) {
          continue;
        }
        const uint64_t expected_size = sizeof(HistoryDiskHeader) + static_cast<uint64_t>(header.sample_count) * sizeof(HistoryDiskSample);
        if (expected_size != static_cast<uint64_t>(st.st_size)) {
          continue;
        }
        if (header.id > max_id) {
          max_id = header.id;
        }
      }
      closedir(dir);
    } else {
      ESP_LOGW(TAG, "Cannot scan history directory for record IDs");
    }
  }
  if (max_id == UINT32_MAX) {
    next_history_id_ = 0;  // Reserved as ID exhaustion.
    ESP_LOGE(TAG, "History ID space exhausted");
  } else {
    next_history_id_ = max_id + 1;
  }
  ESP_LOGI(TAG, "Next history ID: %lu", static_cast<unsigned long>(next_history_id_));
}


bool BCT50AComponent::history_storage_save_(const TestRecord &record) {
  if (!littlefs_mounted_) {
    ESP_LOGW(TAG, "Cannot save history #%lu: LittleFS unavailable", static_cast<unsigned long>(record.id));
    return false;
  }
  if (!record.used || record.id == 0 || (record.sample_count > 0 && record.samples == nullptr) || record.sample_count > UINT32_MAX) {
    ESP_LOGE(TAG, "Invalid history record #%lu", static_cast<unsigned long>(record.id));
    return false;
  }
  char final_path[128];
  char temp_path[128];
  snprintf(final_path, sizeof(final_path), "%s/%06lu.rec", HISTORY_DIR, static_cast<unsigned long>(record.id));
  snprintf(temp_path, sizeof(temp_path), "%s/%06lu.tmp", HISTORY_DIR, static_cast<unsigned long>(record.id));
  // Never overwrite an existing completed record.
  struct stat existing{};
  if (stat(final_path, &existing) == 0) {
    ESP_LOGE(TAG, "History file already exists: %s", final_path);
    return false;
  }
  remove(temp_path);
  HistoryDiskHeader header{};
  header.magic = HISTORY_FILE_MAGIC;
  header.version = HISTORY_FILE_VERSION;
  header.header_size = sizeof(HistoryDiskHeader);
  header.id = record.id;
  header.cycle_step = record.cycle_step;
  memcpy(header.mode, record.mode, sizeof(header.mode));
  memcpy(header.status, record.status, sizeof(header.status));
  memcpy(header.start_time, record.start_time, sizeof(header.start_time));
  memcpy(header.end_time, record.end_time, sizeof(header.end_time));
  header.set_current_a = record.set_current_a;
  header.set_voltage_v = record.set_voltage_v;
  header.termination_current_a = record.termination_current_a;
  header.final_capacity_ah = record.final_capacity_ah;
  header.final_energy_wh = record.final_energy_wh;
  header.runtime_seconds = record.runtime_seconds;
  header.sample_count = static_cast<uint32_t>(record.sample_count);
  FILE *file = fopen(temp_path, "wb");
  if (file == nullptr) {
    ESP_LOGE(TAG, "Cannot create history file %s: %s", temp_path, strerror(errno));
    return false;
  }
  bool ok = fwrite(&header, sizeof(header), 1, file) == 1;
  for (size_t i = 0; ok && i < record.sample_count; i++) {
    const TestSample &sample = record.samples[i];
    HistoryDiskSample disk_sample{};
    disk_sample.elapsed_ms = sample.elapsed_ms;
    disk_sample.voltage_v = sample.voltage_v;
    disk_sample.current_a = sample.current_a;
    disk_sample.capacity_ah = sample.capacity_ah;
    disk_sample.energy_wh = sample.energy_wh;
    ok = fwrite(&disk_sample, sizeof(disk_sample), 1, file) == 1;
  }
  if (ferror(file)) {
    ok = false;
  }
  if (fclose(file) != 0) {
    ok = false;
  }
  if (!ok) {
    ESP_LOGE(TAG, "Write failed for history #%lu", static_cast<unsigned long>(record.id));
    remove(temp_path);
    return false;
  }
  // Verify the complete file size before making it visible as a record.
  struct stat st{};
  const uint64_t expected_size = sizeof(HistoryDiskHeader) + static_cast<uint64_t>(record.sample_count) * sizeof(HistoryDiskSample);
  if (stat(temp_path, &st) != 0 || static_cast<uint64_t>(st.st_size) != expected_size) {
    ESP_LOGE(TAG, "History #%lu file size verification failed", static_cast<unsigned long>(record.id));
    remove(temp_path);
    return false;
  }
  if (rename(temp_path, final_path) != 0) {
    ESP_LOGE(TAG, "Cannot commit history #%lu: %s", static_cast<unsigned long>(record.id), strerror(errno));
    remove(temp_path);
    return false;
  }
  ESP_LOGI(TAG, "Saved history #%lu to LittleFS (%u samples)", static_cast<unsigned long>(record.id), static_cast<unsigned>(record.sample_count));
  return true;
}


static void littlefs_list_directory_(const char *path) {
  ESP_LOGI("bct50a", "LittleFS directory listing: %s", path);
  errno = 0;
  DIR *dir = opendir(path);
  if (dir == nullptr) {
    int err = errno;
    ESP_LOGE("bct50a", "opendir(%s) failed: errno=%d (%s)", path, err, strerror(err));
    return;
  }
  struct dirent *entry;
  size_t count = 0;
  while ((entry = readdir(dir)) != nullptr) {
    char full_path[320];
    struct stat st{};
    int len = snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
    if (len < 0 || static_cast<size_t>(len) >= sizeof(full_path)) { continue; }
    if (stat(full_path, &st) == 0) {
      ESP_LOGI("bct50a", "  %s  (%lu bytes)", entry->d_name, static_cast<unsigned long>(st.st_size));
    } else {
      ESP_LOGI("bct50a", "  %s  (size unavailable)", entry->d_name);
    }
    count++;
  }
  closedir(dir);
  ESP_LOGI("bct50a", "Directory entries: %u", static_cast<unsigned>(count));
}


//////////////////////////////////////////////////////////////////////////////////////////////////////


uint16_t BCT50AComponent::be16_(const uint8_t *p) {
  return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}


uint16_t BCT50AComponent::oem_decode_(uint16_t raw) {
  const uint16_t high = static_cast<uint16_t>((raw >> 8) & 0xFF);
  const int32_t decoded = static_cast<int32_t>(raw) - static_cast<int32_t>(high) * 16;
  return static_cast<uint16_t>(std::max<int32_t>(0, decoded));
}


uint16_t BCT50AComponent::oem_encode_(uint16_t value) {
  const uint16_t high = value / 240U;
  const uint16_t low = value % 240U;
  return static_cast<uint16_t>((high << 8) | low);
}


void BCT50AComponent::setup() {
  add_log_("BCT50A controller starting; waiting for CH340 and hello frame");
  led_startup_until_ms_ = millis() + 1800;
  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (psram_total == 0 || psram_free == 0) {
    ESP_LOGE(TAG, "PSRAM heap unavailable; test history cannot be allocated");
    add_log_("ERROR: PSRAM heap unavailable");
  } else {
    ESP_LOGI(TAG, "PSRAM heap: %u bytes total, %u bytes free (%.2f MiB free)",
           static_cast<unsigned>(psram_total),
           static_cast<unsigned>(psram_free),
           static_cast<double>(psram_free) / (1024.0 * 1024.0));
    add_log_("Setup: PSRAM found!");
  }
  // LittleFS diagnostics
  ESP_LOGI(TAG, "Enumerating data partitions:");
  esp_partition_iterator_t it = esp_partition_find( ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr );
  while (it != nullptr) {
    const esp_partition_t *p = esp_partition_get(it);
    ESP_LOGI(TAG,
      "Partition: label=%s type=0x%02X subtype=0x%02X "
      "address=0x%08X size=%u",
      p->label,
      p->type,
      p->subtype,
      static_cast<unsigned>(p->address),
      static_cast<unsigned>(p->size)
    );
    it = esp_partition_next(it);
  }
  esp_partition_iterator_release(it);
  // Check whether the LittleFS partition is completely blank.
  const esp_partition_t *fs_partition = esp_partition_find_first( ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "bct_fs" );
  bool fs_erased = false;
  if (fs_partition == nullptr) { ESP_LOGE(TAG, "LittleFS partition bct_fs not found"); }
  else {
    esp_err_t check_err = littlefs_partition_is_erased_(fs_partition, fs_erased);
    if (check_err != ESP_OK) {
      ESP_LOGE(TAG, "Could not inspect LittleFS partition: %s", esp_err_to_name(check_err));
    } else if (fs_erased) {
      ESP_LOGW(TAG, "LittleFS partition is blank - formatting for first use");
      esp_err_t format_err = esp_littlefs_format("bct_fs");
      if (format_err == ESP_OK) {
        ESP_LOGI(TAG, "Initial LittleFS format successful");
      } else {
        ESP_LOGE(TAG, "Initial LittleFS format failed: %s", esp_err_to_name(format_err));
      }
    } else {
      ESP_LOGI(TAG, "LittleFS partition contains existing data - preserving it");
    }
  }
  // Mount LittleFS. Never automatically format if mounting fails.
  esp_log_level_set("esp_littlefs", ESP_LOG_VERBOSE);
  esp_vfs_littlefs_conf_t conf = {};
  conf.base_path = "/bct_fs";
  conf.partition_label = "bct_fs";
  conf.format_if_mount_failed = false;
  conf.dont_mount = false;
  esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err == ESP_OK) {
    ESP_LOGI(TAG, "LittleFS mounted successfully");
    littlefs_mounted_ = true;
    size_t total_bytes = 0;
    size_t used_bytes = 0;
    err = esp_littlefs_info("bct_fs", &total_bytes, &used_bytes);
    if (err == ESP_OK) {
      ESP_LOGI(TAG, "LittleFS: total=%u, used=%u, free=%u bytes",
               static_cast<unsigned>(total_bytes),
               static_cast<unsigned>(used_bytes),
               static_cast<unsigned>(total_bytes - used_bytes));
    } else {
      ESP_LOGE(TAG, "LittleFS info failed: %s", esp_err_to_name(err));
      littlefs_mounted_ = false;
    }
  } else {
    littlefs_mounted_ = false;
    ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
    if (fs_partition != nullptr && !fs_erased) {
      ESP_LOGW(TAG, "Existing LittleFS data was NOT formatted or modified");
    }
  }
  
  DIR *test_dir = opendir("/bct_fs");
  if (test_dir != nullptr) {
    ESP_LOGI(TAG, "TEST: opendir(/bct_fs) succeeded");
    closedir(test_dir);
  } else {
    ESP_LOGE(TAG, "TEST: opendir(/bct_fs) failed, errno=%d", errno);
  }
  DIR *test_history_dir = opendir("/bct_fs/history");
  if (test_history_dir != nullptr) {
    ESP_LOGI(TAG, "TEST: opendir(/bct_fs/history) succeeded");
    closedir(test_history_dir);
  } else {
    ESP_LOGE(TAG, "TEST: opendir(/bct_fs/history) failed, errno=%d", errno);
  }
  // Keep existing PSRAM/LittleFS diagnostics above this line.
  history_init_();
  // The captive portal owns HTTP port 80 while Wi-Fi is being provisioned.
  // Start the BCT50A dashboard only after the station joins the user's Wi-Fi.
  ESP_LOGI(TAG, "Dashboard HTTP server waiting for Wi-Fi connection");
//  start_http_server_();
  startup_trace_after_ms_ = millis() + 25000;
}


void BCT50AComponent::loop() {
  if (startup_trace_pending_ && static_cast<int32_t>(millis() - startup_trace_after_ms_) >= 0) {
    startup_trace_pending_ = false;
    ESP_LOGE("BCT_TRACE", "Delayed startup trace: component loop is running");
    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0 || psram_free == 0) {
        ESP_LOGE(TAG, "PSRAM heap unavailable; test history cannot be allocated");
        add_log_("ERROR: PSRAM heap unavailable");
    } else {
        ESP_LOGI(TAG, "PSRAM heap: %u bytes total, %u bytes free (%.2f MiB free)",
           static_cast<unsigned>(psram_total),
           static_cast<unsigned>(psram_free),
           static_cast<double>(psram_free) / (1024.0 * 1024.0));
        add_log_("Setup: PSRAM found!");
    }
    const esp_partition_t *fs_partition = esp_partition_find_first( ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_LITTLEFS, "bct_fs" );
    if (fs_partition == nullptr) {
      ESP_LOGE(TAG, "LittleFS partition bct_fs not found");
      add_log_("ERROR: LittleFS partition not found");
    } else {
      ESP_LOGI(TAG, "LittleFS partition found:");
      ESP_LOGI(TAG, "  Label: %s", fs_partition->label);
      ESP_LOGI(TAG, "  Address: 0x%08X", static_cast<unsigned>(fs_partition->address));
      ESP_LOGI(TAG, "  Size: %u bytes (%.2f MiB)", static_cast<unsigned>(fs_partition->size), static_cast<double>(fs_partition->size) / (1024.0 * 1024.0));
      size_t total_bytes = 0;
      size_t used_bytes = 0;
      esp_err_t err = esp_littlefs_info("bct_fs", &total_bytes, &used_bytes);
      if (err == ESP_OK) {
        const size_t free_bytes = total_bytes - used_bytes;
        ESP_LOGI(TAG, "LittleFS filesystem:");
        ESP_LOGI(TAG, "  Total: %u bytes (%.2f MiB)", static_cast<unsigned>(total_bytes), static_cast<double>(total_bytes) / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  Used:  %u bytes (%.2f MiB)", static_cast<unsigned>(used_bytes), static_cast<double>(used_bytes) / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  Free:  %u bytes (%.2f MiB)", static_cast<unsigned>(free_bytes), static_cast<double>(free_bytes) / (1024.0 * 1024.0));
        ESP_LOGI(TAG, "  Usage: %.1f%%", total_bytes > 0 ? (100.0 * used_bytes / total_bytes) : 0.0);
        add_log_("Setup: LittleFS available");
      } else {
        ESP_LOGE(TAG, "LittleFS info failed: %s", esp_err_to_name(err));
        add_log_("ERROR: LittleFS statistics unavailable");
      }
    } // end partition found
    if (littlefs_mounted_) {
      littlefs_list_directory_("/bct_fs");
      littlefs_list_directory_("/bct_fs/history");
    } else {
      ESP_LOGW(TAG, "LittleFS not mounted - skipping directory listing");
    }
  } // end startup delay trace
  
  while (available() > 0) {
    int value = read();
    if (value >= 0) process_byte_(static_cast<uint8_t>(value));
  }
  const uint32_t now = millis();
  
  // Record samples only while an explicitly started test is running.
  if (test_running_ && !history_paused_ && active_history_index_ >= 0 && now - last_history_sample_ms_ >= 2000) {
    const size_t index = static_cast<size_t>(active_history_index_);
    if (index < HISTORY_MAX_RECORDS) {
      TestRecord &record = history_records_[index];
      if (record.used) {
        TestSample sample{};
        sample.elapsed_ms = history_elapsed_ms_ + (now - history_started_ms_);
        sample.voltage_v = voltage_v_;
        sample.current_a = current_a_;
        sample.capacity_ah = capacity_ah_;
        sample.energy_wh = energy_wh_;
        const bool timestamp_is_new = record.sample_count == 0 || sample.elapsed_ms > record.samples[record.sample_count - 1].elapsed_ms;
        if (timestamp_is_new) {
          if (!history_append_sample_(index, sample)) { ESP_LOGW("bct50a", "Could not store sample for history record #%lu", static_cast<unsigned long>(record.id)); }
        }
        // Keep the latest values available as the test progresses.
        record.final_capacity_ah = capacity_ah_;
        record.final_energy_wh = energy_wh_;
        record.runtime_seconds = sample.elapsed_ms / 1000U;
      }
    }
    last_history_sample_ms_ = now;
  }
  // Detect loss of telemetry while previously connected.
  if (hello_received_ && now - last_valid_frame_ms_ >= 6000) {
    hello_received_ = false;
    state_ = connect_retry_enabled_ ? "Connection lost; reconnecting" : "Disconnected";
    if (device_state_text_sensor_ != nullptr)
      device_state_text_sensor_->publish_state(state_);
    add_log_("Connection lost: no valid frame for 6 seconds");
  }
  // Retry connection only when enabled.
  if (connect_retry_enabled_ && !hello_received_ && now - last_connect_attempt_ms_ >= 3000) {
    send_command_(0x05);
    last_connect_attempt_ms_ = now;
    add_log_("TX reconnect 05");
  }
  if (now - last_runtime_publish_ms_ >= 1000) {
    last_runtime_publish_ms_ = now;
    if (runtime_sensor_ != nullptr) runtime_sensor_->publish_state(runtime_seconds_());
  }
  cycle_runner_loop_();
  update_status_led_();
}


void BCT50AComponent::update_status_led_() {
  if (status_light_ == nullptr) return;
  const uint32_t now = millis();
  // Limit updates to approximately 40 frames per second.
  if (now - last_led_update_ms_ < 25) return;
  last_led_update_ms_ = now;
  float r = 0.0f;
  float g = 0.0f;
  float b = 0.0f;
  float brightness = 0.0f;
  constexpr float PI = 3.14159265f;
  // Helper: smooth breathing brightness between minimum and maximum.
  auto breathing = [&](uint32_t period_ms, float minimum, float maximum) -> float {
    const float phase = static_cast<float>(now % period_ms) / period_ms;
    const float wave = 0.5f - 0.5f * cosf(2.0f * PI * phase);
    return minimum + (maximum - minimum) * wave;
  };
  auto heartbeat_beat = [](uint32_t phase, uint32_t start) -> float {
    if (phase < start) return 0.0f;
    const uint32_t elapsed = phase - start;
    if (elapsed >= 120) return 0.0f;
    if (elapsed < 25) return static_cast<float>(elapsed) / 25.0f;
    return 1.0f - static_cast<float>(elapsed - 25) / 95.0f;
  };
  // Startup: short rainbow sweep.
  if (static_cast<int32_t>(led_startup_until_ms_ - now) > 0) {
    const uint32_t elapsed = 1800 - (led_startup_until_ms_ - now);
    const uint32_t segment = (elapsed / 200) % 6;
    const float fraction = static_cast<float>(elapsed % 200) / 200.0f;
    switch (segment) {
      case 0: r = 1.0f; g = fraction; b = 0.0f; break;
      case 1: r = 1.0f - fraction; g = 1.0f; b = 0.0f; break;
      case 2: r = 0.0f; g = 1.0f; b = fraction; break;
      case 3: r = 0.0f; g = 1.0f - fraction; b = 1.0f; break;
      case 4: r = fraction; g = 0.0f; b = 1.0f; break;
      default: r = 1.0f; g = 0.0f; b = 1.0f - fraction; break;
    }
    brightness = 0.35f;
  }
  // Wi-Fi disconnected: blue breathing.
  else if (wifi::global_wifi_component == nullptr || !wifi::global_wifi_component->is_connected()) {
    r = 0.0f;
    g = 0.0f;
    b = 1.0f;
    brightness = breathing(4500, 0.025f, 0.35f);
  }
  // BCT50A disconnected: rapid red blink.
  else if (!hello_received_ || now - last_valid_frame_ms_ >= 6000) {
    r = 1.0f;
    g = 0.0f;
    b = 0.0f;
    brightness = (now % 250 < 120) ? 0.55f : 0.0f;
  }
  // BCT50A has just reconnected: solid green for three seconds.
  else if (static_cast<int32_t>(connected_led_until_ms_ - now) > 0) {
    r = 0.0f;
    g = 1.0f;
    b = 0.0f;
    brightness = 0.55f;
  }
  // Cycle or test paused: amber blink.
  else if (history_paused_ || cycle_paused_) {
    r = 1.0f;
    g = 0.35f;
    b = 0.0f;
    brightness = (now % 1200 < 180) ? 0.45f : 0.0f;
  }
  // Cycle rest: purple breathing.
  else if (cycle_rest_active_) {
    r = 0.55f;
    g = 0.0f;
    b = 1.0f;
    brightness = breathing(5000, 0.025f, 0.35f);
  }
  // Test running: mode-specific heartbeat.
  else if (test_running_) {
    switch (model_) {
      case 0x0C:  // CC charge
        r = 0.0f;
        g = 1.0f;
        b = 0.0f;
        break;
      case 0x0A:  // CC discharge
        r = 0.0f;
        g = 0.0f;
        b = 1.0f;
        break;
      case 0x0B:  // CV discharge
        r = 0.0f;
        g = 1.0f;
        b = 1.0f;
        break;
      default:
        // Unknown running state: white heartbeat.
        r = 1.0f;
        g = 1.0f;
        b = 1.0f;
        break;
    }
    // Scale heartbeat period from 1600 ms at 0 A
    // down to 500 ms at 50 A.
    float current = fabsf(current_a_);
    if (current > 50.0f) current = 50.0f;
    const uint32_t cycle_ms = static_cast<uint32_t>(1600.0f - (current / 50.0f) * 1100.0f);
    const uint32_t phase = now % cycle_ms;
    const float beat1 = heartbeat_beat(phase, 0);
    const float beat2 = heartbeat_beat(phase, 170);
    brightness = std::max(beat1, beat2) * 0.65f;
  }
  // Connected and idle: slow white breathing.
  else {
    r = 1.0f;
    g = 1.0f;
    b = 1.0f;
    brightness = breathing(6000, 0.025f, 0.30f);
  }
  // Keep the light entity logically ON.
  // Create dark intervals by setting RGB to black rather than
  // setting brightness to zero.
  if (brightness <= 0.001f) {
    r = 0.0f;
    g = 0.0f;
    b = 0.0f;
    brightness = 0.01f;
  }
  auto call = status_light_->make_call();
  call.set_state(true);
  call.set_rgb(r, g, b);
  call.set_brightness(brightness);
  call.set_transition_length(0);
  call.perform();
}


void BCT50AComponent::process_byte_(uint8_t b) {
  if (rx_count_ == 0) {
    if (b != 0xFA) return;
    rx_frame_[rx_count_++] = b;
    return;
  }
  if (rx_count_ >= FRAME_LEN) rx_count_ = 0;
  rx_frame_[rx_count_++] = b;
  if (rx_count_ == FRAME_LEN) {
    if (rx_frame_[18] == 0xF8) {
      process_frame_();
      rx_count_ = 0;
    } else {
      size_t start = 1;
      while (start < FRAME_LEN && rx_frame_[start] != 0xFA) start++;
      if (start < FRAME_LEN) {
        size_t remain = FRAME_LEN - start;
        memmove(rx_frame_.data(), rx_frame_.data() + start, remain);
        rx_count_ = remain;
      } else {
        rx_count_ = 0;
      }
    }
  }
}


void BCT50AComponent::process_frame_() {
  uint8_t check = 0;
  for (size_t i = 1; i <= 16; i++) check ^= rx_frame_[i];
  const uint8_t received = rx_frame_[17];
  const bool checksum_ok = (received == check) || (received == static_cast<uint8_t>(check ^ 0xF0));
  if (!checksum_ok) {
    bad_frames_++;
    char msg[80]; snprintf(msg, sizeof(msg), "Rejected frame checksum: got %02X expected %02X", received, check);
    add_log_(msg);
    return;
  }
  valid_frames_++;
  last_valid_frame_ms_ = millis();
  std::ostringstream hex;
  hex << std::uppercase << std::hex << std::setfill('0');
  for (size_t i = 0; i < FRAME_LEN; i++) { if (i) hex << ' '; hex << std::setw(2) << static_cast<unsigned>(rx_frame_[i]); }
  last_frame_hex_ = hex.str();
//  if (last_frame_text_sensor_ != nullptr) last_frame_text_sensor_->publish_state(last_frame_hex_);
  model_ = rx_frame_[1];
  const uint16_t raw_current = be16_(&rx_frame_[2]);
  const uint16_t raw_voltage = be16_(&rx_frame_[4]);
  const uint16_t raw_ah = be16_(&rx_frame_[6]);
  const uint16_t raw_wh = be16_(&rx_frame_[8]);
  const uint16_t raw_set_current = be16_(&rx_frame_[10]);
  const uint16_t raw_set_voltage = be16_(&rx_frame_[12]);
  const uint16_t raw_end_value = be16_(&rx_frame_[14]);
  const uint8_t flags = rx_frame_[16];
  product_code_ = flags & 0x0F;
  const double divisor = ((flags & 0xF0) < 32) ? 10.0 : 100.0;
  // 0x64 is a one-shot connect/identify response. In this packet the
  // constructor's TotalTime formula deliberately reuses the SetVol field.
  if (model_ == 100) {
    if (!hello_received_) { connected_led_until_ms_ = millis() + 3000; }
    hello_received_ = true;
    const uint16_t set_current_decoded = oem_decode_(raw_set_current);
    const uint16_t set_voltage_decoded = oem_decode_(raw_set_voltage);
    version_ = "V" + std::to_string(set_current_decoded / 100) + ".";
    char ver[16]; snprintf(ver, sizeof(ver), "V%.2f", set_current_decoded / 100.0);
    version_ = ver;
    runtime_base_seconds_ = static_cast<uint32_t>((set_voltage_decoded / 100.0) * 100.0 * 60.0) + rx_frame_[14];
    runtime_running_ = false;
    running_since_ms_ = 0;
    state_ = "Connected / identified";
    char msg[120]; snprintf(msg, sizeof(msg), "RX hello 64; version=%s; retained runtime=%lu s", version_.c_str(), static_cast<unsigned long>(runtime_base_seconds_));
    add_log_(msg);
    if (firmware_version_text_sensor_ != nullptr) firmware_version_text_sensor_->publish_state(version_);
    if (runtime_sensor_ != nullptr) runtime_sensor_->publish_state(runtime_base_seconds_);
    // The shared counters are also present in this hello packet.
    capacity_ah_ = oem_decode_(raw_ah) / divisor;
    energy_wh_ = oem_decode_(raw_wh) / divisor;
    if (capacity_ah_sensor_ != nullptr) capacity_ah_sensor_->publish_state(capacity_ah_);
    if (energy_wh_sensor_ != nullptr) energy_wh_sensor_->publish_state(energy_wh_);
    publish_();
    return;
  }
  // Known status values from the reverse-engineered protocol.
  switch (model_) {
    case 0x00: state_ = "Paused CC discharge"; break;
    case 0x01: state_ = "Paused CV discharge"; break;
    case 0x02: state_ = "Idle / paused CC charge"; break;
    case 0x0A: state_ = "Running CC discharge"; break;
    case 0x0B: state_ = "Running CV discharge"; break;
    case 0x0C: state_ = "Running CC charge"; break;
    default: state_ = "Unknown status 0x"; { char s[8]; snprintf(s, sizeof(s), "%02X", model_); state_ += s; } break;
  }
  const bool was_running = test_running_;
  const bool now_running = model_ == 0x0A || model_ == 0x0B || model_ == 0x0C;
  const uint32_t now = millis();
  // A running-to-idle transition ends the active history record.
  // A pause requested through our software is recorded as paused;
  // an unrequested transition is treated as natural completion.
  if (was_running && !now_running && active_history_index_ >= 0) {
    if (pause_requested_) {
      // The pause handler has already retained the active record.
      pause_requested_ = false;
    } else {
      history_elapsed_ms_ += now - history_started_ms_;
      history_finish_("completed");
      history_elapsed_ms_ = 0;
      history_paused_ = false;
      
      // Notify the cycle runner that this test step completed naturally.
      if (cycle_running_ && !cycle_paused_) {
        cycle_step_completed_ = true;
      }
      
    }
  }
  if (now_running && !runtime_running_) {
    runtime_running_ = true;
    running_since_ms_ = now;
  } else if (!now_running && runtime_running_) {
    runtime_base_seconds_ += (now - running_since_ms_) / 1000U;
    runtime_running_ = false;
    running_since_ms_ = 0;
  }
  test_running_ = now_running;
  // OEM conversions: current/set-current in hundredths of an amp, voltage in mV,
  // capacity/energy divided by the product/format nibble's scale.
  current_a_ = oem_decode_(raw_current) / 100.0f;
  voltage_v_ = oem_decode_(raw_voltage) / 1000.0f;
  capacity_ah_ = oem_decode_(raw_ah) / divisor;
  energy_wh_ = oem_decode_(raw_wh) / divisor;
  set_current_a_ = oem_decode_(raw_set_current) / 100.0f;
  set_voltage_v_ = oem_decode_(raw_set_voltage) / 100.0f;
  end_value_ = oem_decode_(raw_end_value) / 100.0f;
  if (product_code_ == 10) product_name_ = "YPSDZ-0550";
  else if (product_code_ == 11) product_name_ = "YPSDZ-3010";
  else product_name_ = "Product code " + std::to_string(product_code_);
  add_log_("RX " + last_frame_hex_);
  publish_();
}


void BCT50AComponent::publish_() {
  if (voltage_sensor_ != nullptr) voltage_sensor_->publish_state(voltage_v_);
  if (current_sensor_ != nullptr) current_sensor_->publish_state(current_a_);
  if (capacity_ah_sensor_ != nullptr) capacity_ah_sensor_->publish_state(capacity_ah_);
  if (energy_wh_sensor_ != nullptr) energy_wh_sensor_->publish_state(energy_wh_);
  if (set_current_sensor_ != nullptr) set_current_sensor_->publish_state(set_current_a_);
  if (set_voltage_sensor_ != nullptr) set_voltage_sensor_->publish_state(set_voltage_v_);
  if (end_value_sensor_ != nullptr) end_value_sensor_->publish_state(end_value_);
  if (runtime_sensor_ != nullptr) runtime_sensor_->publish_state(runtime_seconds_());
  if (device_state_text_sensor_ != nullptr) device_state_text_sensor_->publish_state(state_);
  if (product_text_sensor_ != nullptr) product_text_sensor_->publish_state(product_name_);
}


uint32_t BCT50AComponent::runtime_seconds_() const {
  if (runtime_running_) return runtime_base_seconds_ + (millis() - running_since_ms_) / 1000U;
  return runtime_base_seconds_;
}


void BCT50AComponent::send_command_(uint8_t cmd, const uint8_t payload[6]) {
  uint8_t frame[10] = {0xFA, cmd, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], 0, 0xF8};
  uint8_t check = oem_checksum_(cmd, payload);
  frame[8] = check;
  write_array(frame, sizeof(frame));
  char msg[100]; snprintf(msg, sizeof(msg), "TX %02X %02X %02X %02X %02X %02X %02X XOR=%02X", cmd, payload[0], payload[1], payload[2], payload[3], payload[4], payload[5], check);
  add_log_(msg);
}


void BCT50AComponent::send_command_(uint8_t cmd) {
  const uint8_t payload[6] = {0, 0, 0, 0, 0, 0};
  send_command_(cmd, payload);
}


uint8_t BCT50AComponent::oem_checksum_(uint8_t cmd, const uint8_t payload[6]) {
  uint16_t value = cmd;
  for (int i = 0; i < 6; i++) {
    value ^= payload[i];
  }
  return static_cast<uint8_t>(oem_encode_(value) & 0xFF);
}


void BCT50AComponent::start_test_(uint8_t cmd, bool charge) {
  if (requested_current_a_ <= 0 || requested_current_a_ > 50 ||
      requested_voltage_v_ <= 0 || requested_voltage_v_ > 5) {
    add_log_("Start rejected: requested current/voltage outside configured safety bounds");
    return;
  }
  const char *mode = charge ? "cc_charge" : (cmd == 0x11 ? "cv_discharge" : "cc_discharge");
  uint16_t i_count = static_cast<uint16_t>(std::lround(requested_current_a_ * 100.0f));
  uint16_t v_count = static_cast<uint16_t>(std::lround(requested_voltage_v_ * 100.0f));
  uint16_t i_enc = oem_encode_(i_count);
  uint16_t v_enc = oem_encode_(v_count);
  uint16_t t_enc = oem_encode_(static_cast<uint16_t>(std::lround(termination_current_a_ * 100.0f)));
  uint8_t payload[6] = {
    static_cast<uint8_t>(i_enc >> 8), static_cast<uint8_t>(i_enc),
    static_cast<uint8_t>(v_enc >> 8), static_cast<uint8_t>(v_enc),
    0, 0
  };
  if (charge) {
    payload[4] = static_cast<uint8_t>(t_enc >> 8);
    payload[5] = static_cast<uint8_t>(t_enc);
  }
  bool resuming = false;
  // Resume the existing record only when the requested test mode matches.
  if (history_paused_ && active_history_index_ >= 0 && history_records_ != nullptr) {
    const size_t index = static_cast<size_t>(active_history_index_);
    if (index < HISTORY_MAX_RECORDS) {
      TestRecord &record = history_records_[index];
      if (record.used && strcmp(record.mode, mode) == 0) {
        resuming = true;
        history_paused_ = false;
        pause_requested_ = false;
        history_started_ms_ = millis();
        last_history_sample_ms_ = history_started_ms_;
        snprintf(record.status, sizeof(record.status), "%s", "running");
        // Use the original test settings for the resumed test.
        requested_current_a_ = record.set_current_a;
        requested_voltage_v_ = record.set_voltage_v;
        termination_current_a_ = record.termination_current_a;
        add_log_("Resuming existing history record");
      }
    }
  }
  if (!resuming) {
    pause_requested_ = false;
    history_paused_ = false;
    history_elapsed_ms_ = 0;
    clear_counters();
    delay(90);
    const int16_t history_index = history_create_record_(mode);
    if (history_index >= 0) {
      TestRecord &record = history_records_[history_index];
      record.set_current_a = requested_current_a_;
      record.set_voltage_v = requested_voltage_v_;
      record.termination_current_a = termination_current_a_;
      history_started_ms_ = millis();
      last_history_sample_ms_ = history_started_ms_;
    }
  }
  send_command_(cmd, payload);
  state_ = (cmd == 0x01) ? "Start requested: CC discharge" :
      (cmd == 0x11 ? "Start requested: CV discharge" :
                     "Start requested: CC charge");
  if (device_state_text_sensor_ != nullptr)
    device_state_text_sensor_->publish_state(state_);
}


void BCT50AComponent::start_cc_discharge() { start_test_(0x01, false); }

void BCT50AComponent::start_cv_discharge() { start_test_(0x11, false); }

void BCT50AComponent::start_cc_charge() { start_test_(0x21, true); }


void BCT50AComponent::pause_test() {
  if (active_history_index_ >= 0 && test_running_ && !history_paused_) {
    const uint32_t now = millis();
    // Accumulate only the active portion of this run segment.
    history_elapsed_ms_ += now - history_started_ms_;
    history_paused_ = true;
    pause_requested_ = true;
    const size_t index = static_cast<size_t>(active_history_index_);
    if (history_records_ != nullptr && index < HISTORY_MAX_RECORDS) {
      TestRecord &record = history_records_[index];
      if (record.used) {
        snprintf(record.status, sizeof(record.status), "%s", "paused");
        record.runtime_seconds = history_elapsed_ms_ / 1000U;
        record.final_capacity_ah = capacity_ah_;
        record.final_energy_wh = energy_wh_;
      }
    }
  }
  send_command_(0x02);
  state_ = "Pause requested";
  if (device_state_text_sensor_ != nullptr) {
    device_state_text_sensor_->publish_state(state_);
  }
}


void BCT50AComponent::suspend_test(uint8_t seconds) {
  if (seconds == 0) {
    // Matches the OEM Suspended(-1) special packet:
    // FA 0F 00 00 00 00 00 00 0F F8
    send_command_(0x0F);
    add_log_("Suspend 00s special command sent");
    delay(50);
    return;
  }
  if (seconds > 60) {
    add_log_("Suspend rejected: duration must be 1-60 seconds");
    return;
  }
  // OEM double2ushort() encoding: hundredths, using the
  // same encoded numeric representation as other commands.
  const uint16_t encoded = oem_encode_(static_cast<uint16_t>(seconds));
  const uint8_t payload[6] = {
      0, 0, 0, 0,
      static_cast<uint8_t>(encoded >> 8),
      static_cast<uint8_t>(encoded)
  };
  send_command_(0x0F, payload);
  delay(50);
  char msg[64];
  snprintf(msg, sizeof(msg), "Suspend %u seconds command sent", static_cast<unsigned>(seconds));
  add_log_(msg);
}


void BCT50AComponent::clear_counters() {
//  send_command_(0x02);
  send_command_(0x08);
  // Remove the active record, including its PSRAM samples and PNG.
  if (active_history_index_ >= 0 && history_records_ != nullptr) {
    const size_t index = static_cast<size_t>(active_history_index_);
    if (index < HISTORY_MAX_RECORDS)
      history_free_record_(history_records_[index]);
  }
  active_history_index_ = -1;
  history_paused_ = false;
  pause_requested_ = false;
  history_elapsed_ms_ = 0;
  history_started_ms_ = 0;
  last_history_sample_ms_ = 0;
  state_ = "Clear requested";
  test_running_ = false;
  runtime_base_seconds_ = 0;
  runtime_running_ = false;
  running_since_ms_ = 0;
  capacity_ah_ = 0;
  energy_wh_ = 0;
  if (capacity_ah_sensor_ != nullptr) capacity_ah_sensor_->publish_state(0);
  if (energy_wh_sensor_ != nullptr) energy_wh_sensor_->publish_state(0);
  if (runtime_sensor_ != nullptr) runtime_sensor_->publish_state(0);
  add_log_("Local counters reset; active history record removed");
}


void BCT50AComponent::connect_device() {
  const uint32_t now = millis();
  // Already connected: do not send another 0x05.
  if (hello_received_ && now - last_valid_frame_ms_ < 6000) {
    connect_retry_enabled_ = true;
    add_log_("Connect requested; already connected, no command sent");
    return;
  }
  connect_retry_enabled_ = true;
  hello_received_ = false;
  state_ = "Connecting to BCT50A";
  if (device_state_text_sensor_ != nullptr)
    device_state_text_sensor_->publish_state(state_);
  // Send connect immediately instead of waiting for the retry timer.
  send_command_(0x05);
  last_connect_attempt_ms_ = millis();
  add_log_("Manual connect requested: TX 05");
}


void BCT50AComponent::disconnect_device() {
  // Prevent the automatic retry loop from undoing the disconnect.
  connect_retry_enabled_ = false;
  hello_received_ = false;
  send_command_(0x06);
  send_command_(0x06);
  send_command_(0x06);
  state_ = "Disconnected";
  if (device_state_text_sensor_ != nullptr)
    device_state_text_sensor_->publish_state(state_);
  add_log_("Manual disconnect requested: TX 06 x3");
}


void BCT50AComponent::toggle_pc_mode() {
  const bool connected =
      hello_received_ &&
      millis() - last_valid_frame_ms_ < 6000;
  if (connected) {
    disconnect_device();
  } else {
    connect_device();
  }
}


void BCT50AComponent::dashboard_command(const std::string &action, float current, float voltage, float termination_current) {
  if (std::isfinite(current)) set_current(current);
  if (std::isfinite(voltage)) set_voltage(voltage);
  if (std::isfinite(termination_current)) set_termination_current(termination_current);
  if (action == "start_cc_discharge") start_cc_discharge();
  else if (action == "start_cv_discharge") start_cv_discharge();
  else if (action == "start_cc_charge") start_cc_charge();
  else if (action == "pause") pause_test();
  else if (action == "clear") { pause_test(); clear_counters(); } // also stops and clears
  else if (action == "disconnect") disconnect_device();
  else if (action == "adjust") {
    // Apply the requested CC-discharge current/voltage using the same encoded fields as 0x01.
    uint16_t i_count = static_cast<uint16_t>(std::lround(requested_current_a_ * 100.0f));
    uint16_t v_count = static_cast<uint16_t>(std::lround(requested_voltage_v_ * 100.0f));
    uint16_t i_enc = oem_encode_(i_count), v_enc = oem_encode_(v_count);
    uint8_t payload[6] = {static_cast<uint8_t>(i_enc >> 8), static_cast<uint8_t>(i_enc), static_cast<uint8_t>(v_enc >> 8), static_cast<uint8_t>(v_enc), 0, 0};
    send_command_(0x07, payload);
    add_log_("TX adjust 07 using CC-discharge current/voltage field layout");
  }
  else if (action == "connect") connect_device();
  else if (action == "toggle_pc_mode") toggle_pc_mode();
  else if (action == "cycle_stop") stop_cycle();
  else if (action == "cycle_pause") {
    if (cycle_running_) {
      cycle_paused_ = true;
      cycle_waiting_for_resume_ = true;
      if (cycle_rest_active_) {
        const uint32_t now = millis();
        cycle_step_elapsed_ms_ += now - cycle_step_started_ms_;
        cycle_step_started_ms_ = now;
        suspend_test(0);
        cycle_rest_active_ = false;
      } else if (test_running_) {
        cycle_status_ = "paused";
      }
      add_log_("Cycle paused");
    }
  }
  else if (action == "cycle_resume") {
    if (cycle_running_ && cycle_paused_) {
      if (cycle_step_index_ < cycle_step_count_ && cycle_steps_[cycle_step_index_].type == CycleStepType::PAUSE) {
        cycle_step_index_++;
        cycle_step_initialized_ = false;
        cycle_step_completed_ = false;
        cycle_step_elapsed_ms_ = 0;
      }
      cycle_paused_ = false;
      cycle_waiting_for_resume_ = false;
      if (cycle_step_index_ < cycle_step_count_) {
        const CycleStep &step = cycle_steps_[cycle_step_index_];
        if (step.type == CycleStepType::REST) {
          const uint32_t total_ms = static_cast<uint32_t>(step.duration_seconds) * 1000UL;
          const uint32_t remaining_ms = cycle_step_elapsed_ms_ >= total_ms ? 0 : total_ms - cycle_step_elapsed_ms_;
          const uint32_t remaining_seconds = (remaining_ms + 999U) / 1000U;
          if (remaining_seconds > 0) {
            suspend_test(static_cast<uint8_t>(remaining_seconds > 255 ? 255 : remaining_seconds));
            cycle_step_started_ms_ = millis();
            cycle_rest_active_ = true;
          }
          else {
            cycle_advance_step_();
          }
        } else if (active_history_index_ >= 0 && history_paused_) {
          // Resume the same test using its original settings.
          const TestRecord &record = history_records_[static_cast<size_t>(active_history_index_)];
          requested_current_a_ = record.set_current_a;
          requested_voltage_v_ = record.set_voltage_v;
          termination_current_a_ = record.termination_current_a;
          if (step.type == CycleStepType::CC_CHARGE) start_cc_charge();
          else if (step.type == CycleStepType::CC_DISCHARGE) start_cc_discharge();
          else if (step.type == CycleStepType::CV_DISCHARGE) start_cv_discharge();
        }
      }
      add_log_("Cycle resumed");
      cycle_status_ = "running";
    }
  }
}


void BCT50AComponent::add_log_(const std::string &line) {
  char stamp[20]; snprintf(stamp, sizeof(stamp), "%lu s", static_cast<unsigned long>(millis() / 1000U));
  logs_[log_head_] = std::string("[") + stamp + "] " + line;
  log_head_ = (log_head_ + 1) % logs_.size();
  if (log_count_ < logs_.size()) log_count_++;
  ESP_LOGI(TAG, "%s", line.c_str());
}


bool BCT50AComponent::dashboard_cycle_command(const std::string &body, std::string &response) {
  auto reject = [&response](const char *error) {
    response = "{\"ok\":false,\"error\":\"";
    response += error;
    response += "\"}";
    return false;
  };
  CycleJsonValue root;
  CycleJsonParser parser(body);
  if (!parser.parse(root) || root.type != CycleJsonValue::OBJECT)
    return reject("Invalid JSON request");
  const CycleJsonValue *action_value = root.get("action");
  if (action_value == nullptr || action_value->type != CycleJsonValue::STRING) {
    return reject("Missing or invalid action");
  }
  const std::string &action = action_value->string_value;
  // Control commands do not replace the configured cycle steps.
  if (action == "pause") {
    if (!cycle_running_ || cycle_paused_)
      return reject("Cycle is not running or is already paused");
    dashboard_command("cycle_pause", NAN, NAN, NAN);
  } else if (action == "resume") {
    if (!cycle_running_ || !cycle_paused_)
      return reject("Cycle is not paused");
    dashboard_command("cycle_resume", NAN, NAN, NAN);
  } else if (action == "stop") {
    if (!cycle_running_)
      return reject("Cycle is not running");
    dashboard_command("cycle_stop", NAN, NAN, NAN);
  } else if (action == "start") {
    if (cycle_running_)
      return reject("A cycle is already running");
    const CycleJsonValue *steps_value = root.get("steps");
    if (steps_value == nullptr ||
        steps_value->type != CycleJsonValue::ARRAY ||
        steps_value->array.empty()) {
      return reject("Cycle requires at least one step");
    }
    if (steps_value->array.size() > CYCLE_MAX_STEPS)
      return reject("Too many cycle steps");
    std::array<CycleStep, CYCLE_MAX_STEPS> new_steps{};
    size_t count = 0;
    for (const auto &item : steps_value->array) {
      if (item.type != CycleJsonValue::OBJECT)
        return reject("Each step must be an object");
      const CycleJsonValue *id_value = item.get("id");
      const CycleJsonValue *mode_value = item.get("mode");
      const CycleJsonValue *voltage_value = item.get("voltage");
      const CycleJsonValue *current_value = item.get("current");
      const CycleJsonValue *end_value = item.get("end");
      uint32_t id;
      if (!cycle_json_integer(id_value, 1, CYCLE_MAX_STEPS, id) || id != count + 1) {
        return reject("Step IDs must be sequential starting at 1");
      }
      if (mode_value == nullptr || mode_value->type != CycleJsonValue::STRING) {
        return reject("Missing or invalid step mode");
      }
      const std::string &mode = mode_value->string_value;
      CycleStep step{};
      if (mode == "rest" || mode == "loop") {
        double voltage, current;
        uint32_t end;
        if (!cycle_json_number(voltage_value, voltage) ||
            !cycle_json_number(current_value, current) ||
            voltage != 0 || current != 0) {
          return reject("Rest and loop steps require zero voltage and current");
        }
        if (mode == "rest") {
          if (!cycle_json_integer(end_value, 1, 4294967295.0, end))
            return reject("Rest duration must be a positive whole number of seconds");
          step.type = CycleStepType::REST;
          step.duration_seconds = end;
        } else {
          if (!cycle_json_integer( end_value, 1, 65535, end)) {
            return reject("Loop count must be a positive whole number");
          }
          step.type = CycleStepType::LOOP;
          step.repeat_count = static_cast<uint16_t>(end);
        }
      } else {
        double voltage, current, end;
        if (!cycle_json_number(voltage_value, voltage) || !cycle_json_number(current_value, current)) {
          return reject("Missing or invalid voltage or current");
        }
        if (voltage < 0.1 || voltage > 5.0 || current < 0.01 || current > 50.0) {
          return reject("Voltage must be 0.1-5 V and current 0.01-50 A");
        }
        if (mode == "cc_charge") {
          if (!cycle_json_number(end_value, end) || end < 0.01 || end > 50.0) {
            return reject("Charge termination current must be 0.01-50 A");
          }
          step.type = CycleStepType::CC_CHARGE;
          step.termination_current_a = static_cast<float>(end);
        } else if (mode == "cc_discharge") {
          step.type = CycleStepType::CC_DISCHARGE;
        } else if (mode == "cv_discharge") {
          step.type = CycleStepType::CV_DISCHARGE;
        } else {
          return reject("Unsupported cycle step mode");
        }
        step.voltage_v = static_cast<float>(voltage);
        step.current_a = static_cast<float>(current);
      }
      new_steps[count++] = step;
    }
    // Commit the configuration only after every step validates.
    cycle_steps_ = new_steps;
    cycle_step_count_ = count;
    if (!cycle_start_())
      return reject("Cycle runner rejected the start request");
  } else {
    return reject("Unknown cycle action");
  }
  const char *status = cycle_running_ ? (cycle_paused_ ? "paused" : "running") : "completed";
  response = "{\"ok\":true,\"status\":\"";
  response += status;
  response += "\",\"current_step\":";
  response += std::to_string(cycle_step_index_ + 1);
  response += "}";
  return true;
}


std::string BCT50AComponent::json_escape_(const std::string &s) {
  std::string out; out.reserve(s.size() + 8);
  for (char c : s) { if (c == '"' || c == '\\') { out += '\\'; out += c; } else if (c == '\n') out += "\\n"; else if (c == '\r') out += "\\r"; else if (static_cast<unsigned char>(c) >= 0x20) out += c; }
  return out;
}


std::string BCT50AComponent::status_json() {
  char head[900];
  const bool pc_connected = hello_received_ && millis() - last_valid_frame_ms_ < 6000;
  const char *cycle_status = cycle_status_.c_str();
  const uint32_t current_timestamp = get_timestamp_();
  const float cpu_temp = cpu_temp_sensor_ != nullptr ? cpu_temp_sensor_->state : NAN;
  char cpu_temp_json[24];
  if (std::isfinite(cpu_temp)) {
    snprintf(cpu_temp_json, sizeof(cpu_temp_json), "%.1f", cpu_temp);
  } else {
    snprintf(cpu_temp_json, sizeof(cpu_temp_json), "null");
  }
  // Wi-Fi connection details
  wifi_ap_record_t ap_info{};
  const bool wifi_connected = esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK;
  const std::string wifi_ssid = wifi_connected ? json_escape_(std::string( reinterpret_cast<const char *>(ap_info.ssid), strnlen(reinterpret_cast<const char *>(ap_info.ssid), sizeof(ap_info.ssid)))) : "";
  snprintf(head, sizeof(head), "{\"voltage\":%.3f,\"current\":%.3f,\"ah\":%.2f,\"wh\":%.2f,\"set_current\":%.2f,\"set_voltage\":%.2f,\"end_value\":%.2f,\"runtime\":%lu,\"state\":\"%s\",\"running\":%s,\"version\":\"%s\",\"product\":\"%s\",\"connected\":%s,\"valid_frames\":%lu,\"bad_frames\":%lu,\"cycle_status\":\"%s\",\"cycle_step\":%u,\"time_source\":\"%s\",\"unix_time\":%lu,\"cpu_temp\":\"%s\",\"wifi\":{\"connected\":%s,\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u}}",
    voltage_v_, current_a_, capacity_ah_, energy_wh_, set_current_a_, set_voltage_v_, end_value_,
     static_cast<unsigned long>(runtime_seconds_()),
     json_escape_(state_).c_str(),
     test_running_ ? "true" : "false",
     json_escape_(version_).c_str(),
     json_escape_(product_name_).c_str(),
     hello_received_ ? "true" : "false",
     static_cast<unsigned long>(valid_frames_),
     static_cast<unsigned long>(bad_frames_),
     cycle_status,
     cycle_running_ ? static_cast<unsigned>(cycle_step_index_ + 1) : static_cast<unsigned>(cycle_step_index_),
     json_escape_(time_source_).c_str(),
     static_cast<unsigned long>(current_timestamp),
     cpu_temp_json,
     wifi_connected ? "true" : "false",
     wifi_ssid.c_str(),
     wifi_connected ? static_cast<int>(ap_info.rssi) : 0,
     wifi_connected ? static_cast<unsigned>(ap_info.primary) : 0
  );
  // PSRAM statistics
  const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const bool psram_available = psram_total > 0;
  const float psram_usage = psram_available && psram_total > 0 ? (100.0f * static_cast<float>(psram_total - psram_free) / static_cast<float>(psram_total)) : 0.0f;
  // LittleFS statistics
  size_t fs_total = 0;
  size_t fs_used = 0;
  const esp_err_t fs_err = littlefs_mounted_ ? esp_littlefs_info("bct_fs", &fs_total, &fs_used) : ESP_ERR_NOT_FOUND;
  const bool fs_available = fs_err == ESP_OK;
  const size_t fs_free = fs_available && fs_total >= fs_used ? fs_total - fs_used : 0;
  const float fs_usage = fs_available && fs_total > 0 ? (100.0f * static_cast<float>(fs_used) / static_cast<float>(fs_total)) : 0.0f;
  char storage[320];
  snprintf(storage, sizeof(storage),
      ",\"storage\":{"
        "\"psram\":{"
          "\"available\":%s,"
          "\"total\":%u,"
          "\"free\":%u,"
          "\"usage_percent\":%.1f"
        "},"
        "\"littlefs\":{"
          "\"available\":%s,"
          "\"mounted\":%s,"
          "\"total\":%u,"
          "\"used\":%u,"
          "\"free\":%u,"
          "\"usage_percent\":%.1f"
        "}"
      "}}",
      psram_available ? "true" : "false",
      static_cast<unsigned>(psram_total),
      static_cast<unsigned>(psram_free),
      psram_usage,
      fs_available ? "true" : "false",
      littlefs_mounted_ ? "true" : "false",
      static_cast<unsigned>(fs_total),
      static_cast<unsigned>(fs_used),
      static_cast<unsigned>(fs_free),
      fs_usage
  );
  std::string out(head);
  out.pop_back();  // Remove original closing brace
  out += storage;
  return out;
}


std::string BCT50AComponent::history_json( int32_t id_filter, uint32_t sample_offset, uint32_t sample_limit ) {
  std::ostringstream out;
  out << std::fixed << std::setprecision(3);
  out << "{\"records\":[";
  bool first_record = true;
  // ---------------------------------------------------------
  // 1. Read completed records from LittleFS.
  // ---------------------------------------------------------
  if (littlefs_mounted_) {
    DIR *dir = opendir(HISTORY_DIR);
    if (dir != nullptr) {
      struct dirent *entry;
      while ((entry = readdir(dir)) != nullptr) {
        const char *name = entry->d_name;
        const size_t name_len = strlen(name);
        if (name_len < 5 || strcmp(name + name_len - 4, ".rec") != 0) {
          continue;
        }
        char path[320];
        int path_len = snprintf(path, sizeof(path), "%s/%s", HISTORY_DIR, name);
        if (path_len < 0 || static_cast<size_t>(path_len) >= sizeof(path)) { continue; }
        FILE *file = fopen(path, "rb");
        if (file == nullptr) { continue; }
        HistoryDiskHeader header{};
        if (fread(&header, sizeof(header), 1, file) != 1) {
          fclose(file);
          continue;
        }
        // Validate the record header.
        if (header.magic != HISTORY_FILE_MAGIC || header.version != HISTORY_FILE_VERSION || header.header_size != sizeof(HistoryDiskHeader) || header.id == 0) {
          fclose(file);
          continue;
        }
        // Validate the complete file size.
        struct stat st{};
        if (stat(path, &st) != 0) {
          fclose(file);
          continue;
        }
        const uint64_t expected_size = sizeof(HistoryDiskHeader) + static_cast<uint64_t>(header.sample_count) * sizeof(HistoryDiskSample);
        if (expected_size != static_cast<uint64_t>(st.st_size)) {
          fclose(file);
          continue;
        }
        // ID filtering.
        if (id_filter == 0 || (id_filter > 0 && header.id != static_cast<uint32_t>(id_filter))) {
          fclose(file);
          continue;
        }
        // Ensure strings are null-terminated.
        header.mode[sizeof(header.mode) - 1] = '\0';
        header.status[sizeof(header.status) - 1] = '\0';
        header.start_time[sizeof(header.start_time) - 1] = '\0';
        header.end_time[sizeof(header.end_time) - 1] = '\0';
        if (!first_record) { out << ','; }
        first_record = false;
        out << "{\"id\":" << header.id
            << ",\"source\":\"littlefs\""
            << ",\"mode\":\"" << json_escape_(header.mode) << '"'
            << ",\"step_cycle\":\"" << header.cycle_step << '"'
            << ",\"status\":\"" << json_escape_(header.status) << '"'
            << ",\"start_time\":\"" << json_escape_(header.start_time) << '"'
            << ",\"end_time\":\"" << json_escape_(header.end_time) << '"'
            << ",\"set_current_a\":" << header.set_current_a
            << ",\"set_voltage_v\":" << header.set_voltage_v
            << ",\"termination_current_a\":" << header.termination_current_a
            << ",\"final_capacity_ah\":" << header.final_capacity_ah
            << ",\"final_energy_wh\":" << header.final_energy_wh
            << ",\"runtime_seconds\":" << header.runtime_seconds
            << ",\"sample_count\":" << header.sample_count
            << ",\"has_png\":false"
            << ",\"samples_offset\":" << sample_offset
            << ",\"samples\":[";
        // Only load samples when a specific record is requested.
        // id_filter == -1 means metadata only.
        if (id_filter != -1) {
          const uint32_t first_sample = (sample_offset < header.sample_count) ? sample_offset : header.sample_count;
          const uint32_t remaining = header.sample_count - first_sample;
          const uint32_t samples_to_read = (remaining < sample_limit) ? remaining : sample_limit;
          // Seek directly to the first requested sample.
          const uint64_t sample_position = sizeof(HistoryDiskHeader) + static_cast<uint64_t>(first_sample) * sizeof(HistoryDiskSample);
          if (fseek(file, static_cast<long>(sample_position), SEEK_SET) == 0) {
            for (uint32_t s = 0; s < samples_to_read; s++) {
              HistoryDiskSample sample{};
              if (fread(&sample, sizeof(sample), 1, file) != 1) {
                ESP_LOGW(TAG,
                         "Failed reading sample %lu from history #%lu",
                         static_cast<unsigned long>(first_sample + s),
                         static_cast<unsigned long>(header.id));
                break;
              }
              if (s != 0) { out << ','; }
              out << "{\"elapsed_ms\":" << sample.elapsed_ms
                  << ",\"voltage_v\":" << sample.voltage_v
                  << ",\"current_a\":" << sample.current_a
                  << ",\"capacity_ah\":" << sample.capacity_ah
                  << ",\"energy_wh\":" << sample.energy_wh
                  << '}';
            }
          } else {
            ESP_LOGW(TAG, "Failed seeking history #%lu to sample %lu",
                     static_cast<unsigned long>(header.id),
                     static_cast<unsigned long>(first_sample));
          }
        }
        out << "]}";
        fclose(file);
      }
      closedir(dir);
    } else {
      ESP_LOGW(TAG, "Cannot open history directory for JSON");
    }
  }
  // ---------------------------------------------------------
  // 2. Read records still retained in PSRAM.
  // ---------------------------------------------------------
  if (history_records_ != nullptr) {
    for (size_t index = 0; index < HISTORY_MAX_RECORDS; index++) {
      const TestRecord &record = history_records_[index];
      if (!record.used) { continue; }
      // ID filtering.
      if (id_filter == 0) {
        if (strcmp(record.status, "running") != 0) { continue; }
      } else if (id_filter > 0) {
        if (record.id != static_cast<uint32_t>(id_filter)) {
          continue;
        }
      }
      if (!first_record) { out << ','; }
      first_record = false;
      out << "{\"id\":" << record.id
          << ",\"source\":\"psram\""
          << ",\"mode\":\"" << json_escape_(record.mode) << '"'
          << ",\"step_cycle\":\"" << record.cycle_step << '"'
          << ",\"status\":\"" << json_escape_(record.status) << '"'
          << ",\"start_time\":\"" << json_escape_(record.start_time) << '"'
          << ",\"end_time\":\"" << json_escape_(record.end_time) << '"'
          << ",\"set_current_a\":" << record.set_current_a
          << ",\"set_voltage_v\":" << record.set_voltage_v
          << ",\"termination_current_a\":" << record.termination_current_a
          << ",\"final_capacity_ah\":" << record.final_capacity_ah
          << ",\"final_energy_wh\":" << record.final_energy_wh
          << ",\"runtime_seconds\":" << record.runtime_seconds
          << ",\"sample_count\":" << record.sample_count
          << ",\"has_png\":" << (record.png_data != nullptr ? "true" : "false")
          << ",\"samples_offset\":" << sample_offset
          << ",\"samples\":[";
      // Only load samples when a specific record is requested.
      if (id_filter != -1) {
        const size_t first_sample = (sample_offset < record.sample_count) ? sample_offset : record.sample_count;
        const size_t remaining = record.sample_count - first_sample;
        const size_t samples_to_read = (remaining < sample_limit) ? remaining : sample_limit;
        for (size_t s = 0; s < samples_to_read; s++) {
          const TestSample &sample = record.samples[first_sample + s];
          if (s != 0) { out << ','; }
          out << "{\"elapsed_ms\":" << sample.elapsed_ms
              << ",\"voltage_v\":" << sample.voltage_v
              << ",\"current_a\":" << sample.current_a
              << ",\"capacity_ah\":" << sample.capacity_ah
              << ",\"energy_wh\":" << sample.energy_wh
              << '}';
        }
      }
      out << "]}";
    }
  }
  out << "]}";
  return out.str();
}


esp_err_t BCT50AComponent::history_delete_handler(httpd_req_t *req) {
  auto *self = static_cast<BCT50AComponent *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Invalid component context");
    return ESP_FAIL;
  }
  // Get record ID from query string.
  char query[64] = {};
  char id_str[16] = {};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", id_str, sizeof(id_str)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing record ID");
    return ESP_FAIL;
  }
  char *end_ptr = nullptr;
  const unsigned long id = strtoul(id_str, &end_ptr, 10);
  if (end_ptr == id_str || *end_ptr != '\0' || id == 0 || id > UINT32_MAX) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid record ID");
    return ESP_FAIL;
  }
  const uint32_t record_id = static_cast<uint32_t>(id);
  // ---------------------------------------------------------
  // 1. Check PSRAM records.
  // ---------------------------------------------------------
  if (self->history_records_ != nullptr) {
    for (size_t i = 0; i < HISTORY_MAX_RECORDS; i++) {
      TestRecord &record = self->history_records_[i];
      if (!record.used || record.id != record_id) {
        continue;
      }
      // Never allow deletion of a running or active record.
      if (strcmp(record.status, "running") == 0 ||
          self->active_history_index_ == static_cast<int16_t>(i)) {
        //httpd_resp_send_err(req, HTTPD_409_CONFLICT, "Cannot delete a running test");
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Cannot delete a running test");
        return ESP_FAIL;
      }
      self->history_free_record_(record);
      if (self->history_count_ > 0) {
        --self->history_count_;
      }
      self->history_recalculate_next_id_();
      ESP_LOGI(TAG, "Deleted PSRAM history record #%lu", static_cast<unsigned long>(record_id));
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"ok\":true,\"source\":\"psram\",\"message\":\"Record deleted\"}");
      return ESP_OK;
    }
  }
  // ---------------------------------------------------------
  // 2. Check LittleFS records.
  // ---------------------------------------------------------
  if (!self->littlefs_mounted_) {
    //httpd_resp_send_err(req, HTTPD_503_SERVICE_UNAVAILABLE, "LittleFS unavailable");
    httpd_resp_set_status(req, "503 Service Unavailable");
    httpd_resp_set_type(req, "text/plain");
    httpd_resp_sendstr(req, "LittleFS unavailable");
    return ESP_FAIL;
  }
  char path[128];
  const int path_len = snprintf( path, sizeof(path), "%s/%06lu.rec", HISTORY_DIR, static_cast<unsigned long>(record_id));
  if (path_len < 0 || static_cast<size_t>(path_len) >= sizeof(path)) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "History filename error");
    return ESP_FAIL;
  }
  struct stat st{};
  if (stat(path, &st) != 0) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "History record not found");
    return ESP_FAIL;
  }
  if (unlink(path) != 0) {
    ESP_LOGE(TAG, "Failed deleting LittleFS history #%lu: %s", static_cast<unsigned long>(record_id), strerror(errno));
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to delete history record");
    return ESP_FAIL;
  }
  self->history_recalculate_next_id_();
  ESP_LOGI(TAG, "Deleted LittleFS history record #%lu", static_cast<unsigned long>(record_id));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_sendstr(req, "{\"ok\":true,\"source\":\"littlefs\",\"message\":\"Record deleted\"}");
  return ESP_OK;
}


static esp_err_t history_handler(httpd_req_t *req) {
  auto *self = static_cast<BCT50AComponent *>(req->user_ctx);
  if (self == nullptr) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Component unavailable");
    return ESP_FAIL;
  }
  // -1 = all records (existing behaviour)
  //  0 = currently running record
  // >0 = specific record ID
  int32_t id_filter = -1;
  uint32_t sample_offset = 0;
  uint32_t sample_limit = 256;
  const size_t query_len = httpd_req_get_url_query_len(req);
  if (query_len > 0) {
    std::string query(query_len + 1, '\0');
    if (httpd_req_get_url_query_str(req, query.data(), query.size()) == ESP_OK) {
      char id_value[16] = {};
      if (httpd_query_key_value(query.c_str(), "id", id_value, sizeof(id_value)) == ESP_OK) {
        char *end = nullptr;
        const long parsed_id = strtol(id_value, &end, 10);
        if (end == id_value || *end != '\0' || parsed_id < 0 || parsed_id > INT32_MAX) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid history ID");
          return ESP_FAIL;
        }
        id_filter = static_cast<int32_t>(parsed_id);
      }
      // Optional sample pagination parameters.
      char offset_value[16] = {};
      if (httpd_query_key_value(query.c_str(), "offset", offset_value, sizeof(offset_value)) == ESP_OK) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(offset_value, &end, 10);
        if (end == offset_value || *end != '\0' || parsed > UINT32_MAX) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid history offset");
          return ESP_FAIL;
        }
        sample_offset = static_cast<uint32_t>(parsed);
      }
      char limit_value[16] = {};
      if (httpd_query_key_value(query.c_str(), "limit", limit_value, sizeof(limit_value)) == ESP_OK) {
        char *end = nullptr;
        const unsigned long parsed = strtoul(limit_value, &end, 10);
        if (end == limit_value || *end != '\0' ||
            parsed == 0 || parsed > 256) {
          httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid history limit (1-256)");
          return ESP_FAIL;
        }
        sample_limit = static_cast<uint32_t>(parsed);
      }
    }
  }
  const std::string body = self->history_json(id_filter, sample_offset, sample_limit);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), static_cast<ssize_t>(body.size()));
}


static esp_err_t root_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, DASHBOARD_HTML, HTTPD_RESP_USE_STRLEN);
}


static esp_err_t status_handler(httpd_req_t *req) {
  auto *self = static_cast<BCT50AComponent *>(req->user_ctx);
  const std::string body = self->status_json();
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.size());
}


static esp_err_t cycle_handler(httpd_req_t *req) {
  auto *component = static_cast<BCT50AComponent *>(req->user_ctx);
  if (component == nullptr) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Component unavailable\"}");
    return ESP_OK;
  }
  // Reject empty or unreasonably large request bodies.
  if (req->content_len <= 0 || req->content_len > 8192) {
    httpd_resp_set_status(req, "400 Bad Request");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Invalid request body size\"}");
    return ESP_OK;
  }
  std::string body;
  body.reserve(req->content_len);
  char buffer[512];
  int remaining = req->content_len;
  while (remaining > 0) {
    const size_t to_read = std::min(static_cast<size_t>(remaining), sizeof(buffer));
    const int received = httpd_req_recv(req, buffer, to_read);
    if (received <= 0) {
      if (received == HTTPD_SOCK_ERR_TIMEOUT) { continue; }
      httpd_resp_set_status(req, "400 Bad Request");
      httpd_resp_set_type(req, "application/json");
      httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Failed to read request body\"}");
      return ESP_OK;
    }
    body.append(buffer, received);
    remaining -= received;
  }
  std::string response;
  if (!component->dashboard_cycle_command( body, response)) {
    httpd_resp_set_status(req, "400 Bad Request");
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, response.c_str(), static_cast<ssize_t>(response.size()));
  return ESP_OK;
}


static esp_err_t format_handler(httpd_req_t *req) {
  if (http_component == nullptr) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Component unavailable\"}");
    return ESP_OK;
  }
  esp_err_t err = http_component->format_storage();
  httpd_resp_set_type(req, "application/json");
  if (err == ESP_ERR_INVALID_STATE) {
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"A test or cycle is running\"}");
  } else if (err != ESP_OK) {
    httpd_resp_set_status(req, "500 Internal Server Error");
    char response[128];
    snprintf(response, sizeof(response), "{\"ok\":false,\"error\":\"%s\"}", esp_err_to_name(err));
    httpd_resp_sendstr(req, response);
  } else {
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"LittleFS formatted successfully\"}");
  }
  return ESP_OK;
}


static esp_err_t command_handler(httpd_req_t *req) {
  auto *self = static_cast<BCT50AComponent *>(req->user_ctx);
  char query[256] = {0}; char action[48] = {0}; char current_s[24] = {0}; char voltage_s[24] = {0}; char term_s[24] = {0};
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
    httpd_query_key_value(query, "action", action, sizeof(action));
    httpd_query_key_value(query, "current", current_s, sizeof(current_s));
    httpd_query_key_value(query, "voltage", voltage_s, sizeof(voltage_s));
    httpd_query_key_value(query, "termination_current", term_s, sizeof(term_s));
  }
  if (action[0] == 0) { httpd_resp_set_status(req, "400 Bad Request"); return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"missing action\"}"); }
  const float current = current_s[0] ? strtof(current_s, nullptr) : NAN;
  const float voltage = voltage_s[0] ? strtof(voltage_s, nullptr) : NAN;
  const float term = term_s[0] ? strtof(term_s, nullptr) : NAN;
  const std::string a(action);
  const bool known =
    a == "start_cc_discharge" ||
    a == "start_cv_discharge" ||
    a == "start_cc_charge" ||
    a == "pause" ||
    a == "clear" ||
    a == "connect" ||
    a == "disconnect" ||
    a == "toggle_pc_mode" || a == "suspend_30" || a == "suspend_00" || a == "adjust";
  if (!known) { httpd_resp_set_status(req, "400 Bad Request"); return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"unknown action\"}"); }
  self->dashboard_command(a, current, voltage, term);
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, "{\"ok\":true}");
}


void BCT50AComponent::start_dashboard() {
  this->start_http_server_();
}


void BCT50AComponent::stop_dashboard() {
  this->stop_http_server_();
}


void BCT50AComponent::start_http_server_() {
  if (http_server_ != nullptr) return;
  http_component = this;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.max_uri_handlers = 8;
  if (httpd_start(&http_server_, &config) != ESP_OK) {
    http_server_ = nullptr;
    ESP_LOGE(TAG, "Could not start dashboard HTTP server on port 80");
    return;
  }
  httpd_uri_t root = {}; root.uri = "/"; root.method = HTTP_GET; root.handler = root_handler; root.user_ctx = this;
  httpd_uri_t status = {}; status.uri = "/api/status"; status.method = HTTP_GET; status.handler = status_handler; status.user_ctx = this;
  httpd_uri_t command = {}; command.uri = "/api/cmd"; command.method = HTTP_GET; command.handler = command_handler; command.user_ctx = this;
  httpd_uri_t history = {}; history.uri = "/api/history"; history.method = HTTP_GET; history.handler = history_handler; history.user_ctx = this;
  httpd_uri_t cycle = {}; cycle.uri = "/api/cycle"; cycle.method = HTTP_POST; cycle.handler = cycle_handler; cycle.user_ctx = this;
  httpd_uri_t format = {}; format.uri = "/api/format"; format.method = HTTP_POST; format.handler = format_handler; format.user_ctx = this;
  httpd_uri_t history_delete_uri = {.uri = "/api/history/delete", .method = HTTP_POST, .handler = history_delete_handler, .user_ctx = this };
  httpd_register_uri_handler(http_server_, &root);
  httpd_register_uri_handler(http_server_, &status);
  httpd_register_uri_handler(http_server_, &command);
  httpd_register_uri_handler(http_server_, &history);
  httpd_register_uri_handler(http_server_, &cycle);
  httpd_register_uri_handler(http_server_, &format);
  httpd_register_uri_handler(http_server_, &history_delete_uri);
  ESP_LOGI(TAG, "Custom dashboard started on HTTP port 80");
}


void BCT50AComponent::stop_http_server_() {
  if (http_server_ == nullptr) return;
  httpd_stop(http_server_);
  http_server_ = nullptr;
  if (http_component == this) http_component = nullptr;
  ESP_LOGI(TAG, "Custom dashboard stopped to release HTTP port 80");
}


// time
uint32_t BCT50AComponent::get_timestamp_() {
  if (home_time_ != nullptr) {
    const auto now = home_time_->now();
    if (now.is_valid()) {
      snprintf(time_source_, sizeof(time_source_), "ha");
      return static_cast<uint32_t>(now.timestamp);
    }
  }
  if (sntp_time_ != nullptr) {
    auto now = sntp_time_->now();
    if (now.is_valid()) {
      snprintf(time_source_, sizeof(time_source_), "sntp");
      return static_cast<uint32_t>(now.timestamp);
    }
  }
  return 0;
}


// psram recording
void BCT50AComponent::history_init_() {
  if (history_records_ != nullptr) { return; }
  history_records_ = static_cast<TestRecord *>( heap_caps_calloc(HISTORY_MAX_RECORDS, sizeof(TestRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (history_records_ == nullptr) {
    ESP_LOGE("bct50a", "Unable to allocate test history in PSRAM");
    return;
  }
  history_count_ = 0;
  history_next_index_ = 0;
  next_history_id_ = 1;
  active_history_index_ = -1;
  // Initialise persistent history and recover the next record ID.
  history_storage_init_();
  ESP_LOGI("bct50a", "Test history ready: %u record slots in PSRAM", static_cast<unsigned>(HISTORY_MAX_RECORDS));
}


void BCT50AComponent::history_free_record_(TestRecord &record) {
  if (record.samples != nullptr) {
    heap_caps_free(record.samples);
  }
  if (record.png_data != nullptr) {
    heap_caps_free(record.png_data);
  }
  // These are plain-data fields, so resetting the structure is safe.
  memset(&record, 0, sizeof(record));
}


int16_t BCT50AComponent::history_create_record_(const char *mode) {
  if (history_records_ == nullptr) {
    ESP_LOGE("bct50a", "Cannot create history record: PSRAM unavailable");
    return -1;
  }
  const size_t index = history_next_index_;
  TestRecord &record = history_records_[index];
  // The ring buffer reuses the oldest slot when all slots are occupied.
  history_free_record_(record);
  record.id = next_history_id_++;
  record.used = true;
  if (cycle_running_) {
    record.cycle_step = cycle_step_index_ + 1;
  }
  const uint32_t timestamp = get_timestamp_();
  snprintf(record.start_time, sizeof(record.start_time), "%lu", static_cast<unsigned long>(timestamp));
  if (mode != nullptr) {
    snprintf(record.mode, sizeof(record.mode), "%s", mode);
  }
  snprintf(record.status, sizeof(record.status), "%s", "running");
  history_next_index_ = (history_next_index_ + 1) % HISTORY_MAX_RECORDS;
  if (history_count_ < HISTORY_MAX_RECORDS) {
    ++history_count_;
  }
  active_history_index_ = static_cast<int16_t>(index);
  ESP_LOGI(
      "bct50a",
      "Created history record #%lu (%s)",
      static_cast<unsigned long>(record.id),
      record.mode);
  return active_history_index_;
}


bool BCT50AComponent::history_append_sample_(
    size_t index, const TestSample &sample) {
  if (history_records_ == nullptr || index >= HISTORY_MAX_RECORDS) {
    return false;
  }
  TestRecord &record = history_records_[index];
  if (!record.used) { return false; }
  if (record.sample_count >= record.sample_capacity) {
    const size_t new_capacity = record.sample_capacity == 0 ? HISTORY_INITIAL_SAMPLES : record.sample_capacity * 2;
    if (new_capacity < record.sample_capacity || new_capacity > SIZE_MAX / sizeof(TestSample)) {
      ESP_LOGE("bct50a", "Sample buffer capacity overflow");
      return false;
    }
    auto *new_samples = static_cast<TestSample *>( heap_caps_malloc(new_capacity * sizeof(TestSample), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (new_samples == nullptr) {
      ESP_LOGE("bct50a", "Unable to grow sample buffer for record #%lu", static_cast<unsigned long>(record.id));
      return false;
    }
    if (record.samples != nullptr && record.sample_count > 0) {
      memcpy( new_samples, record.samples, record.sample_count * sizeof(TestSample));
    }
    if (record.samples != nullptr) {
      heap_caps_free(record.samples);
    }
    record.samples = new_samples;
    record.sample_capacity = new_capacity;
  }
  record.samples[record.sample_count++] = sample;
  return true;
}


void BCT50AComponent::history_finish_(const char *status) {
  if (active_history_index_ < 0 || history_records_ == nullptr) return;
  const size_t index = static_cast<size_t>(active_history_index_);
  if (index >= HISTORY_MAX_RECORDS) return;
  TestRecord &record = history_records_[index];
  if (!record.used) return;
  snprintf(record.status, sizeof(record.status), "%s", status);
  record.final_capacity_ah = capacity_ah_;
  record.final_energy_wh = energy_wh_;
  record.runtime_seconds = history_elapsed_ms_ / 1000U;
  const uint32_t timestamp = get_timestamp_();
  snprintf(record.end_time, sizeof(record.end_time), "%lu", static_cast<unsigned long>(timestamp));
  ESP_LOGI(TAG, "Finished history record #%lu: %s", static_cast<unsigned long>(record.id), record.status);
  // Save completed record to LittleFS when available.
  if (littlefs_mounted_) {
    if (history_storage_save_(record)) {
      ESP_LOGI(TAG, "History record #%lu saved to LittleFS", static_cast<unsigned long>(record.id));
      history_free_record_(record);
      if (history_count_ > 0)
        --history_count_;
    } else {
      ESP_LOGW(TAG, "History record #%lu retained in PSRAM: LittleFS save failed", static_cast<unsigned long>(record.id));
    }
  } else {
    ESP_LOGW(TAG, "History record #%lu retained in PSRAM: LittleFS unavailable", static_cast<unsigned long>(record.id));
  }
  active_history_index_ = -1;
  pause_requested_ = false;
}


// cycle runner
bool BCT50AComponent::cycle_start_() {
  if (cycle_running_ || cycle_step_count_ == 0) {
    add_log_("Cycle start rejected: already running or no steps");
    return false;
  }
  clear_counters();
  delay(50); // give time for 0x08 / clear counters ??? - 50ms works
  cycle_step_index_ = 0;
  cycle_running_ = true;
  cycle_paused_ = false;
  cycle_stop_requested_ = false;
  cycle_step_completed_ = false;
  cycle_step_started_ms_ = millis();
  cycle_step_elapsed_ms_ = 0;
  cycle_loop_remaining_ = 0;
  cycle_step_initialized_ = false;
  cycle_loop_step_index_ = 0;
  cycle_loop_block_start_ = 0;
  cycle_status_ = "running";
  add_log_("Cycle test started");
  cycle_runner_loop_();
  return true;
}


bool BCT50AComponent::cycle_start_current_step_() {
  if (cycle_step_index_ >= cycle_step_count_) {
    cycle_finish_("completed");
    return false;
  }
  const CycleStep &step = cycle_steps_[cycle_step_index_];
  // Mark this step initialised before taking any action.
  cycle_step_initialized_ = true;
  cycle_step_started_ms_ = millis();
  cycle_step_elapsed_ms_ = 0;
  cycle_step_completed_ = false;
  switch (step.type) {
    case CycleStepType::CC_CHARGE:
      requested_current_a_ = step.current_a;
      requested_voltage_v_ = step.voltage_v;
      termination_current_a_ = step.termination_current_a;
      start_cc_charge();
      return true;
    case CycleStepType::CC_DISCHARGE:
      requested_current_a_ = step.current_a;
      requested_voltage_v_ = step.voltage_v;
      termination_current_a_ = step.termination_current_a;
      start_cc_discharge();
      return true;
    case CycleStepType::CV_DISCHARGE:
      requested_current_a_ = step.current_a;
      requested_voltage_v_ = step.voltage_v;
      termination_current_a_ = step.termination_current_a;
      start_cv_discharge();
      return true;
    case CycleStepType::REST:
      cycle_rest_active_ = true;
      suspend_test(static_cast<uint8_t>(step.duration_seconds > 255 ? 255 : step.duration_seconds));
      add_log_("Cycle rest started");
      return true;
    case CycleStepType::STOP:
      cycle_finish_("completed");
      return false;
    case CycleStepType::LOOP:
      // The runner handles loop instructions.
      cycle_step_initialized_ = false;
      return true;
    case CycleStepType::PAUSE:
      cycle_paused_ = true;
      cycle_waiting_for_resume_ = true;
      add_log_("Cycle reached pause step; waiting for resume");
      return true;
  }
  return false;
}


void BCT50AComponent::cycle_advance_step_() {
  const bool has_next = cycle_has_next_executable_step_();
  cycle_step_index_++;
  cycle_step_initialized_ = false;
  cycle_step_completed_ = false;
  cycle_step_elapsed_ms_ = 0;
  if (cycle_step_index_ >= cycle_step_count_) {
    cycle_finish_("completed");
    return;
  }
  // Clear only after the previous step has finished and only when
  // another measurement step will run.
  if (has_next && active_history_index_ >= 0) {
    clear_counters();
  }
  cycle_start_current_step_();
}


void BCT50AComponent::cycle_finish_(const char *status) {
  if (cycle_rest_active_) {
    suspend_test(0);
    cycle_rest_active_ = false;
  }
  cycle_running_ = false;
  cycle_paused_ = false;
  cycle_stop_requested_ = false;
  cycle_step_completed_ = false;
  cycle_loop_remaining_ = 0;
  cycle_waiting_for_resume_ = false;
  cycle_loop_step_index_ = 0;
  cycle_step_initialized_ = false;
  cycle_step_index_ = 0;
  cycle_status_ = status;
  add_log_(std::string("Cycle test ") + status);
}


void BCT50AComponent::cycle_runner_loop_() {
  if (!cycle_running_ || cycle_paused_) {
    return;
  }
  if (cycle_stop_requested_) {
    cycle_finish_("idle");
    return;
  }
  if (cycle_step_completed_) {
    cycle_advance_step_();
    return;
  }
  if (cycle_step_index_ >= cycle_step_count_) {
    cycle_finish_("completed");
    return;
  }
  const CycleStep &step = cycle_steps_[cycle_step_index_];
  if (step.type == CycleStepType::LOOP) {
    cycle_handle_loop_();
    return;
  }
  // Initialise each step exactly once.
  if (!cycle_step_initialized_) {
    cycle_start_current_step_();
    return;
  }
  if (step.type == CycleStepType::REST) {
    if (millis() - cycle_step_started_ms_ >= static_cast<uint32_t>(step.duration_seconds) * 1000UL) {
      suspend_test(0);
      cycle_rest_active_ = false;
      add_log_("Cycle rest ended");
      cycle_advance_step_();
    }
  }
}


bool BCT50AComponent::cycle_handle_loop_() {
  if (cycle_step_index_ >= cycle_step_count_) {
    return false;
  }
  const CycleStep &step = cycle_steps_[cycle_step_index_];
  // A loop at index zero has no preceding block.
  if (cycle_step_index_ == 0) {
    add_log_("Cycle loop ignored: no preceding steps");
    cycle_advance_step_();
    return true;
  }
  // Initialise the repeat count only on first reaching this loop.
  if (cycle_loop_step_index_ != cycle_step_index_) {
    cycle_loop_step_index_ = cycle_step_index_;
    cycle_loop_block_start_ = 0;
    cycle_loop_remaining_ = step.repeat_count;
    add_log_("Cycle loop initialised");
  }
  if (cycle_loop_remaining_ > 0) {
    --cycle_loop_remaining_;
    // Restart the preceding block from its first step.
    cycle_step_index_ = cycle_loop_block_start_;
    cycle_step_initialized_ = false;
    cycle_step_completed_ = false;
    cycle_step_elapsed_ms_ = 0;
    return true;
  }
  // Repeats exhausted; continue after the loop instruction.
  cycle_loop_step_index_ = 0;
  cycle_loop_remaining_ = 0;
  cycle_advance_step_();
  return true;
}


void BCT50AComponent::stop_cycle() {
  if (!cycle_running_) {
    return;
  }
  // Stop the measurement and discard its active history record.
  if (active_history_index_ >= 0) {
    if (test_running_ && !history_paused_) {
      history_elapsed_ms_ += millis() - history_started_ms_;
    }
    send_command_(0x02);
    delay(100);
    add_log_("Cycle measurement aborted; history discarded");
  }
  // End any active rest timer.
  if (cycle_rest_active_) {
    suspend_test(0);
    cycle_rest_active_ = false;
  }
  clear_counters();
  cycle_status_ = "idle";
  cycle_running_ = false;
  history_paused_ = false;
  pause_requested_ = false;

  cycle_stop_requested_ = true;
  cycle_runner_loop_();
}


bool BCT50AComponent::cycle_has_next_executable_step_() {
  for (size_t i = cycle_step_index_ + 1; i < cycle_step_count_; i++) {
    const CycleStepType type = cycle_steps_[i].type;
    if (type == CycleStepType::STOP) {
      return false;
    }
    if (type == CycleStepType::CC_CHARGE ||
        type == CycleStepType::CC_DISCHARGE ||
        type == CycleStepType::CV_DISCHARGE) {
      return true;
    }
    if (type == CycleStepType::LOOP &&
        cycle_steps_[i].repeat_count > 0) {
      // The loop repeats the preceding block, which may include
      // another executable measurement step.
      for (size_t j = 0; j < i; j++) {
        const CycleStepType previous = cycle_steps_[j].type;

        if (previous == CycleStepType::CC_CHARGE ||
            previous == CycleStepType::CC_DISCHARGE ||
            previous == CycleStepType::CV_DISCHARGE) {
          return true;
        }
      }
    }
  }
  return false;
}


}  // namespace bct50a

}  // namespace esphome
