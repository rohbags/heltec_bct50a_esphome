# Heltec BCT50A Controller

A Wi-Fi-enabled controller and monitoring dashboard for the Heltec BCT50A battery capacity tester.

The controller connects to the BCT50A through its USB interface and provides a browser-based dashboard for live monitoring, individual charge and discharge tests, automated cycle testing, and permanent test-result storage.

It also integrates with Home Assistant through ESPHome, allowing the tester's measurements and controller status to be monitored alongside other devices.

---

## Features

- **Live monitoring** – View battery voltage, current, capacity, energy and elapsed runtime.
- **Single Test** – Configure and run constant-current charge, constant-current discharge and constant-voltage discharge tests.
- **Cycle Test** – Create automated sequences of charge, discharge, rest and repeat steps.
- **Test history** – Completed tests and cycle steps are saved to the controller's filesystem.
- **Graph restoration** – Reload saved test data and rebuild graphs after refreshing or reopening the dashboard.
- **CSV export** – Download the recorded samples from individual tests.
- **PNG export** – Save graph images for completed tests.
- **Storage management** – Monitor PSRAM and filesystem usage, delete individual results, or format the filesystem.
- **Wi-Fi provisioning** – Configure the controller's Wi-Fi connection through its setup access point.
- **Home Assistant integration** – Expose tester measurements, settings and controller information as ESPHome entities.
- **RGB status LED** – Visual indication of Wi-Fi, tester connection and operating state.

## Hardware

The project is designed specifically for:

| Component | Specification |
|---|---|
| Controller | ESP32-S3 N16R8 |
| Flash | 16 MB |
| PSRAM | 8 MB |
| Tester | Heltec BCT50A |
| USB interface | CH340 USB-to-UART |
| Firmware framework | ESPHome with ESP-IDF |

The ESP32-S3 N16R8 is the supported controller target. The available PSRAM is used for test-history buffers and other memory-intensive operations, while the onboard flash contains the firmware and a dedicated LittleFS data partition.

The BCT50A communicates with the ESP32-S3 through its USB interface. The controller uses a customised ESPHome CH340 USB-UART component with the Heltec-specific register initialisation required by the tester.

### Custom CH340 support

The project includes a customised USB-UART CH340 component. This is important: the BCT50A requires additional vendor-specific CH340 register initialisation and cannot simply be treated as an ordinary CH340 serial connection.

The YAML configuration must retain:

```yaml
usb_uart:
  - type: ch340
    heltec_reg: true
    channels:
      - id: bct_uart
        baud_rate: 115200
        data_bits: 8
        parity: NONE
        stop_bits: 1
        debug: true
        dummy_receiver: false
```

The `heltec_reg: true` option enables the additional Heltec-specific CH340 initialisation. Do not remove it when configuring the controller.

The custom `bct50a` and `usb_uart` components must both be available in the ESPHome project's local `my_components` directory.

---

## Installation and Wi-Fi setup

The firmware is built and installed using ESPHome. The supplied configuration targets the ESP32-S3 N16R8 and includes the custom USB-UART and BCT50A components.

The project is supplied without fixed Wi-Fi network credentials. On initial startup, the controller creates its own setup access point.

### First-time Wi-Fi provisioning

1. Power up the controller.
2. Connect a phone, tablet or computer to the controller's Wi-Fi access point:

   **SSID:** `BCT50A Setup`  
   **Password:** `bct50a-setup`

3. Allow the captive portal to open, or open a browser and navigate to:

   `http://192.168.4.1`

4. Select your normal Wi-Fi network.
5. Enter its password and submit the settings.

The controller will connect to the selected network and save the Wi-Fi credentials in its preferences. The credentials survive normal restarts and firmware reflashing, provided the device's preferences/NVS are not erased.

**Android captive portal note:** Android does not always display the automatic sign-in notification when connecting to the setup access point. If the notification does not appear, open a browser and navigate to any website. The captive portal should redirect the browser to the setup page. You can also enter `http://192.168.4.1` directly.

### Accessing the dashboard

Once the controller has connected to your Wi-Fi network, its custom dashboard starts automatically.

Open a browser and navigate to the controller's IP address, using the address assigned by your router.

The dashboard uses HTTP on port 80. It is intended for use on the local network.

The custom dashboard is started when the Wi-Fi connection is established and stopped if that connection is lost. This allows the ESPHome captive portal and the custom dashboard to share port 80 without conflicting.

### Optional fixed Wi-Fi configuration

Users who prefer a fixed Wi-Fi configuration can edit the YAML and supply their network SSID and password using ESPHome secrets or direct configuration. The setup access point can remain configured as a fallback.

---

## Dashboard overview

The dashboard is divided into the following main areas:

- Live measurements
- Single Test
- Cycle Test
- Results
- Device details
- Test graph

The header displays the controller name, storage summary, theme control and current dashboard time.

A light/dark theme preference is retained by the browser.

The dashboard clock uses the available Home Assistant or SNTP time source. The selected time source is indicated alongside the clock. Accurate system time is important for meaningful test start times and exported graph images.

### Storage summary

The storage indicator in the dashboard header shows the current PSRAM and filesystem usage.

Selecting it opens the Storage Details dialog, which provides:

- PSRAM total, used and free capacity.
- PSRAM usage percentage.
- LittleFS total, used and free capacity.
- LittleFS usage percentage.
- Filesystem formatting controls.

This makes it possible to check available memory and storage before running longer tests.

---

## Live measurements

The live measurements area displays the values currently reported by the BCT50A:

| Measurement | Description |
|---|---|
| Voltage | Measured battery voltage |
| Current | Measured charge or discharge current |
| Capacity | Accumulated capacity in Ah |
| Energy | Accumulated energy in Wh |
| Elapsed runtime | Current test runtime |

The values are updated from the tester's serial telemetry.

The controller also displays the tester's connection state. If the BCT50A is disconnected, the dashboard indicates that telemetry is no longer being received.

### Monitoring

The **Monitor** button starts or stops graph monitoring without starting a charge or discharge test.

Monitoring is useful for observing the tester and battery without creating a test-history record.

Monitoring-only graph data is temporary. It is not saved as a completed test and is not restored as a historical test after a browser refresh.

Starting an actual test automatically records its samples.

---

## Single Test

The Single Test tab is used to configure and run an individual charge or discharge test.

### Test modes

The available modes are:

**CC Charge – Constant-current charge**

Charges the battery using the configured current and voltage limit. The test terminates when the configured charge termination current is reached during the constant-voltage phase.

**CC Discharge – Constant-current discharge**

Discharges the battery at the configured current until the tester reaches its voltage limit.

**CV Discharge – Constant-voltage discharge**

Runs the tester in its constant-voltage discharge mode.

### Test settings

The Single Test controls include:

- **Mode** – Select CC Charge, CC Discharge or CV Discharge.
- **Current** – Requested charge or discharge current.
- **Voltage limit** – Requested voltage limit.
- **Charge termination** – Termination current for CC charging.
- **Preset** – Select a predefined battery profile.

The requested settings are applied when the Start button is pressed. Changing a value does not independently start a test.

Presets are provided for common LiFePO4 and Li-ion battery configurations. Selecting a preset fills in the corresponding test settings; the values can still be adjusted before starting.

### Test controls

- **Start** – Starts the selected test.
- **Pause** – Pauses an active test. The test can subsequently be resumed.
- **Clear/Stop** – Stops the active test and clears the BCT50A counters.
- **Adjust** – Applies an updated current and voltage setting where supported.
- **Monitor** – Starts or stops monitoring without starting a test.
- **Connect** – Requests a connection to the BCT50A.

A test started from the dashboard is automatically recorded. Samples are captured during the test and the record is finalised when the test completes.

Pausing and resuming a test retains its existing history record and elapsed test time.

---

## Cycle Test

Cycle Test allows multiple charge and discharge operations to be arranged into an automated sequence.

This is useful for battery capacity testing, charge/discharge cycling and repeatable battery evaluation.

The Cycle Test tab contains two main areas:

- **Parameter setting / Results** – The left panel displays the programmed steps or the results of completed cycle steps.
- **Step controls** – The right panel is used to configure and control the cycle.

### Cycle step configuration

Each programmed step has a sequential step number, operating mode and associated settings.

The available step types are:

| Step type | Function |
|---|---|
| CC Charge | Constant-current charge with voltage limit and termination current |
| CC Discharge | Constant-current discharge with voltage limit |
| CV Discharge | Constant-voltage discharge |
| Rest | Wait for a specified duration |
| Loop | Repeat a preceding block of steps |

A cycle can contain up to 32 configured steps.

### Configuring a cycle

1. Select the step number.
2. Select the operating mode.
3. Enter the voltage, current and end value as appropriate for that mode.
4. Press **Set Row** to save the step.
5. Repeat for the remaining steps.

The available controls are:

- **Set Row** – Add or update the selected step.
- **Delete Row** – Delete the selected step.
- **Clear All** – Remove all programmed steps and cycle results.
- **Start** – Start the programmed cycle.
- **Pause / Resume** – Pause the running cycle or resume it.
- **Stop** – Stop the cycle.

The cycle cannot be edited while it is actively running. Pause or stop it before making changes.

### Rest steps

A Rest step suspends the tester for the configured duration.

The duration is entered in seconds. When the rest period expires, the cycle runner advances to the next step.

Rest timing is controlled by the programmed cycle step rather than a fixed delay.

### Loop steps

A Loop step repeats the preceding block of steps for the configured number of additional repetitions.

The current implementation supports one loop step for the cycle. Nested loops are not supported.

### Cycle presets

Two predefined cycle configurations are available.

**LiFePO4**

- CC Charge: 3.65 V, 50 A, 0.10 A termination current.
- Rest: 30 seconds.
- CC Discharge: 2.50 V, 50 A.

**Li-ion**

- CC Charge: 4.20 V, 1 A, 0.10 A termination current.
- Rest: 30 seconds.
- CC Discharge: 2.50 V, 1 A.

Selecting a preset populates the cycle step list. Existing steps may be replaced after confirmation.

These are starting configurations, not a substitute for checking the limits and requirements of the battery being tested. Adjust the settings to suit the battery and test equipment.

### Cycle operation and history

The cycle runner executes each step sequentially. Charge and discharge steps use the same test-history mechanism as individual Single Tests.

Each completed charge or discharge step receives its own result record, including its measurements, runtime and final Ah/Wh values.

Rest and loop steps control cycle execution; they are not separate battery measurement records.

The controller manages the BCT50A counters between executable test steps when another test step follows. It does not unnecessarily clear the final counters after the last executable step.

Pausing a cycle pauses the current operation or rest period. Resuming continues the cycle from the appropriate point.

The left-hand panel can be switched between **Parameter setting** and **Results** to review the results of completed cycle steps.

---

## Results and test history

The Results tab provides access to completed individual tests and cycle results.

Each result displays:

- Record ID
- Test mode or cycle
- Start date and time
- Runtime
- Final capacity (Ah)
- Final energy (Wh)
- Storage source
- PNG export
- CSV export
- Delete control

Cycle results are grouped together. Expand a cycle to see its individual charge and discharge steps.

Select an individual test or cycle step to load its recorded samples into the graph.

### Saved history

Test samples are captured by the controller and retained as history records. The ESP32-S3's PSRAM is used for history buffers, while the LittleFS data partition provides persistent storage.

Completed test records and their saved graph images remain available after a browser is closed, refreshed or reopened.

The dashboard retrieves historical samples from the controller in pages, allowing larger records to be loaded without requiring one oversized response. A progress indicator is displayed while history samples are being retrieved.

The graph is rebuilt from the stored samples rather than relying on the browser's previous session.

This also allows an active test's recorded graph data to be recovered after refreshing the dashboard. Monitoring-only data is not stored in the same way.

### Graph display

The graph displays the recorded voltage and current traces against elapsed test time.

The graph includes test information such as:

- Test mode
- Start date and time
- Runtime
- Final capacity (Ah)
- Final energy (Wh)
- Configured test settings

The graph is sized to provide a useful view of longer tests as well as shorter tests.

### CSV export

Each saved result has a CSV download control.

CSV files contain the recorded sample data for the selected test, making it possible to examine or process the measurements in spreadsheet software or other analysis tools.

CSV data is generated when requested; a separate CSV file does not need to be retained for every test.

### PNG export

Each saved result also has a PNG download control.

The controller generates a graph image from the stored test samples. The image includes the graph and the associated test information.

Completed graph images are retained in the filesystem until the corresponding result is deleted or the filesystem is formatted.

The graph can therefore be exported again without needing to rerun the original test.

---

## Storage management

The controller uses two memory/storage areas for different purposes.

### PSRAM

The ESP32-S3 N16R8 provides 8 MB of PSRAM.

PSRAM is used for memory-intensive operations, including test-history buffers and graph/image processing.

The dashboard displays total, used and free PSRAM capacity.

### LittleFS

A dedicated LittleFS partition is used for persistent test data and graph images.

The dashboard displays filesystem capacity and usage so that available storage can be monitored.

### Deleting individual results

The Delete control in the Results table removes the selected history record.

For cycle results, individual steps can be deleted separately.

Deletion requires confirmation and cannot be undone.

### Formatting the filesystem

The Storage Details dialog includes a **Format Filesystem** control.

Formatting permanently deletes all stored test results and graph images.

The operation requires explicit confirmation and displays its progress and final success or failure status. On successful completion, the storage figures and result lists are refreshed.

Formatting is disabled when the filesystem is unavailable or a test/cycle is running.

**Important:** Formatting the filesystem is different from clearing the BCT50A counters. It removes the controller's saved history and images; it does not simply reset the tester's displayed Ah and Wh counters.

---

## RGB status LED

The onboard WS2812 RGB LED provides a visual indication of the controller and tester operating state.

The LED patterns are updated without blocking the controller's normal operation.

| Condition | LED indication |
|---|---|
| Startup | Brief cyan/rainbow animation |
| Wi-Fi unavailable / AP provisioning | Slow blue breathing |
| Wi-Fi connected, BCT50A disconnected | Rapid red blinking |
| BCT50A connected and idle | Slow white breathing |
| CC Charge | Green heartbeat |
| CC Discharge | Blue heartbeat |
| CV Discharge | Cyan heartbeat |
| Test or cycle paused | Slow amber blinking |
| Cycle rest period | Purple breathing |
| BCT50A reconnect | Solid green for 3 seconds |

Heartbeat speed increases with the measured current, providing a visual indication of the actual operating load rather than simply the requested current setting.

The same operating colours are used for individual tests and cycle tests.

The Status LED is also exposed as an ESPHome light entity.

---

## CPU temperature

The ESP32-S3 internal temperature sensor is included in the controller configuration.

CPU temperature is exposed through ESPHome and displayed in the dashboard header.

This provides a convenient way to monitor controller temperature during longer test sessions.

---

## Home Assistant integration

The controller uses the ESPHome native API to publish measurements and controller information to Home Assistant.

The available entities include:

### BCT50A measurements

- Voltage
- Current
- Capacity (Ah)
- Energy (Wh)
- Set current
- Set voltage
- End value
- Runtime

### Device information

- BCT50A state
- Firmware version
- Product/model information
- CPU temperature

### Controller information

- Wi-Fi signal strength
- Free heap RAM
- Maximum free heap block
- Free PSRAM
- Device information
- Last reset reason
- Status LED

The controller also provides Home Assistant controls for requested current, voltage limit and charge termination current, together with buttons for starting CC discharge, CV discharge and CC charge, pausing a test, clearing counters and toggling the BCT50A PC connection mode.

The requested settings are applied when the corresponding test-start operation is issued.

The raw last-frame diagnostic information is retained internally for troubleshooting, but is not exposed as a Home Assistant sensor.

---

## Troubleshooting

### The setup access point is not visible

- Allow the controller time to complete startup.
- Check that the controller is powered correctly.
- Look for the `BCT50A Setup` wireless network.
- If Android connects but does not show a sign-in notification, open a browser and visit `http://192.168.4.1`.

### The dashboard does not load

- Confirm that the controller has joined your normal Wi-Fi network.
- Check the controller's IP address in your router.
- Try opening the dashboard using the IP address rather than a hostname.
- Remember that the custom dashboard is available after the controller connects to Wi-Fi.

### The BCT50A is not detected

- Check the USB connection between the ESP32-S3 and the BCT50A.
- Confirm that the controller is using the customised CH340 component.
- Ensure `heltec_reg: true` is present in the USB-UART configuration.
- Check the ESPHome logs for USB host, CH340 and BCT50A connection messages.
- Confirm that the BCT50A is powered and operating normally.

### Measurements are not updating

- Check the displayed device connection state.
- Confirm that valid telemetry is being received.
- Check the USB connection and ESPHome logs.
- If necessary, use the dashboard Connect control to request a new connection.

### A saved result or graph does not appear

- Open the Results tab and allow the history list to load.
- Select the individual test or expand the relevant cycle.
- Allow the history progress indicator to finish retrieving the samples.
- Check the available LittleFS storage in the Storage Details dialog.

### Storage is full

- Delete individual results that are no longer required.
- Export important CSV and PNG files before deleting their records.
- If all stored results can be discarded, use Format Filesystem in Storage Details.

### Home Assistant does not show an entity

- Confirm that the ESPHome API connection is online.
- Check the ESPHome logs for configuration or sensor errors.
- Confirm that the relevant entity is enabled in Home Assistant.

---

## Operating notes and safety

The BCT50A is a high-current battery test instrument. Always use suitable wiring, connectors, fusing and battery protection for the battery and test current involved.

- Check the battery manufacturer's permitted charge and discharge voltage and current limits before configuring a test.
- Ensure the battery is correctly connected with the correct polarity.
- Use wiring and connections rated for the intended current.
- Provide adequate ventilation and cooling for the tester and battery.
- Do not leave potentially hazardous battery tests unattended.
- Stop a test if the battery, wiring or tester behaves unexpectedly.
- Do not assume that a preset is suitable for every battery of the same chemistry.

The controller provides remote configuration, monitoring and test-history functions. It does not replace the BCT50A's own operating protections or the precautions required when testing batteries.

---

## Project configuration

The project is intended to be built with ESPHome using the ESP-IDF framework.

The supported target is the ESP32-S3 N16R8, with 16 MB flash and 8 MB octal PSRAM.

The firmware configuration includes:

- The customised CH340 USB-UART component.
- The BCT50A controller component.
- A dedicated LittleFS data partition.
- ESPHome native API and OTA support.
- Wi-Fi provisioning and captive portal.
- Home Assistant and SNTP time sources.
- Status LED and CPU temperature monitoring.
- The custom HTTP dashboard.

The custom dashboard uses HTTP port 80. Do not enable ESPHome's standard `web_server` component on the same port.

**Firmware and component versions:** The project was developed using ESPHome 2026.10.0 and ESP-IDF 5.5.6.

---

## Acknowledgements

The BCT50A serial interface was reverse-engineered through observation of the communication between the tester and its original PC software. The controller and dashboard provide an alternative interface for operating the tester, monitoring measurements and retaining test results.

This is an independent project and is not an official Heltec firmware or software release.
