#pragma once

#include <array>
#include <string>
#include <stdint.h>
#include "esphome/core/component.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/text_sensor/text_sensor.h"
#include "esphome/components/time/real_time_clock.h"
#include "esp_http_server.h"
#include "esphome/components/light/light_state.h"


namespace esphome {
namespace bct50a {

class BCT50AComponent : public Component, public uart::UARTDevice {
 public:
  void setup() override;
  void loop() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  void set_voltage_sensor(sensor::Sensor *s) { voltage_sensor_ = s; }
  void set_current_sensor(sensor::Sensor *s) { current_sensor_ = s; }
  void set_capacity_ah_sensor(sensor::Sensor *s) { capacity_ah_sensor_ = s; }
  void set_energy_wh_sensor(sensor::Sensor *s) { energy_wh_sensor_ = s; }
  void set_set_current_sensor(sensor::Sensor *s) { set_current_sensor_ = s; }
  void set_set_voltage_sensor(sensor::Sensor *s) { set_voltage_sensor_ = s; }
  void set_end_value_sensor(sensor::Sensor *s) { end_value_sensor_ = s; }
  void set_runtime_sensor(sensor::Sensor *s) { runtime_sensor_ = s; }
  void set_device_state_text_sensor(text_sensor::TextSensor *s) { device_state_text_sensor_ = s; }
  void set_firmware_version_text_sensor(text_sensor::TextSensor *s) { firmware_version_text_sensor_ = s; }
  void set_product_text_sensor(text_sensor::TextSensor *s) { product_text_sensor_ = s; }
/*  void set_last_frame_text_sensor(text_sensor::TextSensor *s) { last_frame_text_sensor_ = s; }*/

  void set_current(float value) { requested_current_a_ = value; }
  void set_voltage(float value) { requested_voltage_v_ = value; }
  void set_termination_current(float value) { termination_current_a_ = value; }
  void start_dashboard();
  void stop_dashboard();
  void start_cc_discharge();
  void start_cv_discharge();
  void start_cc_charge();
  void pause_test();
  void clear_counters();
  void suspend_test(uint8_t seconds);
  void disconnect_device();
  void connect_device();
  void toggle_pc_mode();
  void stop_cycle();
  esp_err_t format_storage();
  
  void set_home_time(esphome::time::RealTimeClock *time) { home_time_ = time; }
  void set_sntp_time(esphome::time::RealTimeClock *time) { sntp_time_ = time; }
  
  bool dashboard_cycle_command(const std::string &body, std::string &response);

  std::string status_json();
  std::string history_json( int32_t id_filter = -1, uint32_t sample_offset = 0, uint32_t sample_limit = 256 );
  void dashboard_command(const std::string &action, float current, float voltage, float termination_current);
  
  void set_status_light(light::LightState *light) { status_light_ = light; }
  void set_cpu_temp_sensor(sensor::Sensor *s) { cpu_temp_sensor_ = s; }


 protected:
  static constexpr size_t FRAME_LEN = 19;
  std::array<uint8_t, FRAME_LEN> rx_frame_{};
  
    // PSRAM-backed test history.
//  static constexpr size_t HISTORY_MAX_RECORDS = 16;
  static constexpr size_t HISTORY_MAX_RECORDS = 128;
  static constexpr size_t HISTORY_INITIAL_SAMPLES = 64;
  
  /// time
  esphome::time::RealTimeClock *home_time_{nullptr};
  esphome::time::RealTimeClock *sntp_time_{nullptr};
  char time_source_[8] = "None";


  struct TestSample {
    uint32_t elapsed_ms;
    float voltage_v;
    float current_a;
    float capacity_ah;
    float energy_wh;
  };

  // POD structure: safe to allocate with heap_caps_calloc().
  struct TestRecord {
    uint32_t id;
    bool used;
    
    uint32_t cycle_step = 0;

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

    TestSample *samples;
    size_t sample_count;
    size_t sample_capacity;

    uint8_t *png_data;
    size_t png_size;
  };
  
  
  // Cycle Test configuration and runner state.
  static constexpr size_t CYCLE_MAX_STEPS = 32;
  enum class CycleStepType : uint8_t {
    CC_CHARGE,
    CC_DISCHARGE,
    CV_DISCHARGE,
    REST,
    LOOP,
    PAUSE,
    STOP
  };
  struct CycleStep {
    CycleStepType type{CycleStepType::STOP};

    float current_a{1.0f};
    float voltage_v{3.65f};
    float termination_current_a{0.1f};

    // Used by REST and timed PAUSE steps.
    uint32_t duration_seconds{0};

    // LOOP repeats the preceding block this many additional times.
    uint16_t repeat_count{1};
  };

  
  size_t rx_count_{0};
  sensor::Sensor *voltage_sensor_{nullptr};
  sensor::Sensor *current_sensor_{nullptr};
  sensor::Sensor *capacity_ah_sensor_{nullptr};
  sensor::Sensor *energy_wh_sensor_{nullptr};
  sensor::Sensor *set_current_sensor_{nullptr};
  sensor::Sensor *set_voltage_sensor_{nullptr};
  sensor::Sensor *end_value_sensor_{nullptr};
  sensor::Sensor *runtime_sensor_{nullptr};
  text_sensor::TextSensor *device_state_text_sensor_{nullptr};
  text_sensor::TextSensor *firmware_version_text_sensor_{nullptr};
  text_sensor::TextSensor *product_text_sensor_{nullptr};
/*  text_sensor::TextSensor *last_frame_text_sensor_{nullptr};*/

  float voltage_v_{0};
  float current_a_{0};
  float capacity_ah_{0};
  float energy_wh_{0};
  float set_current_a_{0};
  float set_voltage_v_{0};
  float end_value_{0};
  float requested_current_a_{5.0f};
  float requested_voltage_v_{3.65f};
  float termination_current_a_{0.1f};
  uint32_t runtime_base_seconds_{0};
  uint32_t running_since_ms_{0};
  bool runtime_running_{false};
  bool hello_received_{false};
  bool connect_retry_enabled_{true};
  uint32_t last_valid_frame_ms_{0};
  bool test_running_{false};
  bool pause_requested_{false};
  uint8_t model_{0};
  uint8_t product_code_{0};
  std::string state_{"Waiting for BCT50A"};
  std::string version_{"Unknown"};
  std::string product_name_{"Unknown"};
  std::string last_frame_hex_{"No frame received"};
  std::array<std::string, 20> logs_{};
  size_t log_head_{0};
  size_t log_count_{0};
  uint32_t last_connect_attempt_ms_{0};
  uint32_t last_runtime_publish_ms_{0};
  uint32_t valid_frames_{0};
  uint32_t bad_frames_{0};
  // Status LED timing
  uint32_t led_startup_until_ms_{0};
  uint32_t connected_led_until_ms_{0};
  uint32_t last_led_update_ms_{0};
  
  // temp debug logging
  bool startup_trace_pending_{true};
  uint32_t startup_trace_after_ms_{0};
  
  // The record array and its sample/image buffers live in PSRAM.
  TestRecord *history_records_{nullptr};
  size_t history_count_{0};
  size_t history_next_index_{0};
  uint32_t next_history_id_{1};
  uint32_t history_started_ms_{0};
  uint32_t last_history_sample_ms_{0};
  uint32_t history_elapsed_ms_{0};
  bool history_paused_{false};

  // Index of the record currently being captured; -1 means none.
  int16_t active_history_index_{-1};
  
  static esp_err_t history_delete_handler(httpd_req_t *req);
  
  // Cycle Test program.
  std::array<CycleStep, CYCLE_MAX_STEPS> cycle_steps_{};
  size_t cycle_step_count_{0};
  size_t cycle_step_index_{0};
  bool cycle_running_{false};
  bool cycle_paused_{false};
  bool cycle_stop_requested_{false};
  uint32_t cycle_step_started_ms_{0};
  uint32_t cycle_step_elapsed_ms_{0};
  
  // Initial support for a LOOP step; nested loops can be added later.
  size_t cycle_loop_step_index_{0};
  size_t cycle_loop_block_start_{0};
  uint16_t cycle_loop_remaining_{0};
  bool cycle_step_completed_{false};
  bool cycle_step_initialized_{false};
  bool cycle_rest_active_{false};
  bool cycle_waiting_for_resume_{false};
  std::string cycle_status_{"idle"};
  bool littlefs_mounted_{false};
  
  light::LightState *status_light_{nullptr};
  sensor::Sensor *cpu_temp_sensor_{nullptr};
    

  static uint16_t be16_(const uint8_t *p);
  static uint16_t oem_decode_(uint16_t raw);
  static uint16_t oem_encode_(uint16_t decoded);
  static std::string json_escape_(const std::string &s);
  void process_byte_(uint8_t b);
  void process_frame_();
  void publish_();
  void add_log_(const std::string &line);
  void send_command_(uint8_t cmd, const uint8_t payload[6]);
  void send_command_(uint8_t cmd);
  void start_test_(uint8_t cmd, bool charge);
  uint32_t runtime_seconds_() const;
  void start_http_server_();
  void stop_http_server_();
  httpd_handle_t http_server_{nullptr};
  uint8_t oem_checksum_(uint8_t cmd, const uint8_t payload[6]);
  
  // time
  uint32_t get_timestamp_();
  
  // Cycle Test runner.
  bool cycle_start_();
  void cycle_runner_loop_();
  void cycle_advance_step_();
  void cycle_finish_(const char *status);
  bool cycle_start_current_step_();
  bool cycle_handle_loop_();
  bool cycle_has_next_executable_step_();
  
  // psram recorder
  void history_init_();
  void history_free_record_(TestRecord &record);
  int16_t history_create_record_(const char *mode);
  bool history_append_sample_(size_t index, const TestSample &sample);
  void history_finish_(const char *status);
  
  // LittleFS history storage
  bool history_storage_init_();
  bool history_storage_save_(const TestRecord &record);
  void history_recalculate_next_id_();
  
  void update_status_led_();
  
};

}  // namespace bct50a
}  // namespace esphome
