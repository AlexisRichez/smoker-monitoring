# Cold Smoker Monitoring System

A Wi-Fi-enabled monitoring system for cold smoking, based on an ESP8266 NodeMCU, a DS18B20 temperature sensor and an MQ-2 smoke sensor.

The system continuously monitors the smoker temperature and smoke level, detects insufficient smoke and provides a responsive web dashboard accessible from a smartphone or computer.

A permanent Wi-Fi configuration access point allows users to configure or update their home Wi-Fi credentials directly from a web page, without uploading new firmware.

## Features

- 🌡️ **Temperature monitoring** using a DS18B20 digital sensor.
- ☁️ **Smoke monitoring** using an MQ-2 analog sensor.
- 📉 **Minimum smoke threshold** detection to identify insufficient smoke.
- ⏱️ **Initial sensor conditioning** with a non-blocking 24-hour period.
- 🎯 **Manual calibration** based on the smoke level under normal operating conditions.
- 📶 **Permanent Wi-Fi access point** for configuration and local access.
- 🏠 **Home Wi-Fi connectivity** configurable through a web interface.
- 🔄 **Wi-Fi credential management** without uploading new firmware.
- 🌐 **Responsive web dashboard** for real-time monitoring.
- 💾 **EEPROM persistence** for conditioning progress, calibrated smoke threshold and Wi-Fi credentials.
- 🚦 **Status LEDs** for system operation, calibration, smoke alerts and sensor faults.
- 🖥️ **Serial monitoring** for debugging and sensor diagnostics.

## Hardware

| Component | Description |
|---|---|
| Microcontroller | NodeMCU ESP8266 |
| Temperature sensor | DS18B20 |
| Smoke sensor | MQ-2 analog module |
| Status indicators | 4 LEDs |
| Pull-up resistor | 4.7 kΩ for DS18B20 |

## Wiring

### Pin assignment

| Component | NodeMCU pin | ESP8266 GPIO |
|---|---|---|
| DS18B20 DATA | D0 | GPIO16 |
| MQ-2 Analog Output | A0 | ADC |
| System status LED | D1 | GPIO5 |
| Calibration status LED | D2 | GPIO4 |
| Smoke alert LED | D5 | GPIO14 |
| Sensor fault LED | D6 | GPIO12 |

### DS18B20

- Connect VCC to 3.3V.
- Connect GND to GND.
- Connect DATA to D0.
- Add a 4.7 kΩ pull-up resistor between DATA and 3.3V.

### MQ-2

- Power the MQ-2 module using a suitable 5V supply.
- Connect GND to the NodeMCU GND.
- Connect AO to A0 through an appropriate voltage divider if required.

**Important:** Check the maximum permitted voltage on the A0 input of your specific NodeMCU board. Some boards accept approximately 3.3V, while others have a 1V ADC input limit.

The MQ-2 heater requires a suitable power supply. Do not assume that the NodeMCU 3.3V output can power the sensor module.

### Status LEDs

The LEDs are configured as active HIGH outputs.

| LED | Function |
|---|---|
| Green | System operational |
| Yellow | Initial conditioning or calibration |
| Red | Insufficient smoke detected |
| Blue | Sensor fault |

The LED logic can be inverted in the source code if the hardware uses active LOW wiring.

## Operating principle

### 1. Initial conditioning

When the system starts for the first time, the MQ-2 sensor enters an initial conditioning period of 24 hours.

This process is non-blocking:

- The Wi-Fi access point remains available.
- The DS18B20 continues measuring temperature.
- The MQ-2 continues being read.
- Conditioning progress is periodically saved to EEPROM.
- The home Wi-Fi connection can be configured while conditioning is in progress.

After the conditioning period, manual calibration becomes available.

### 2. Smoke calibration

Calibration must be performed while the smoker is operating under normal conditions and producing a satisfactory amount of smoke.

The calibration process consists of:

1. A 5-minute sensor stabilization period.
2. Acquisition of 100 analog measurements.
3. Calculation of the average sensor value.
4. Calculation of the minimum smoke threshold.

The threshold is calculated as follows:

```cpp
smokeThreshold = calibrationAverage * 0.85;
```

This corresponds to a 15% margin below the calibrated reference value.

### 3. Smoke monitoring

During normal operation, the measured MQ-2 value is compared with the calibrated minimum threshold.

```cpp
smokeAlert = (mq2Raw < smokeThreshold);
```

- **Above the threshold:** smoke level considered sufficient.
- **Below the threshold:** insufficient smoke alert.

There is no maximum smoke threshold.

The current implementation assumes that the MQ-2 analog output increases as the smoke level increases. This behavior must be experimentally verified with the actual sensor module.

### 4. Temperature monitoring

The DS18B20 provides continuous temperature measurements.

A sensor fault is reported if the temperature reading is invalid or the sensor becomes disconnected.

## Wi-Fi configuration and web dashboard

The ESP8266 operates in access point + station mode (`WIFI_AP_STA`).

It maintains its own Wi-Fi access point permanently, even when connected to the home Wi-Fi network.

This provides two ways to access the system:

- **Configuration access point:** always available, independently of the home router.
- **Home Wi-Fi:** allows access to the dashboard through the local network.

### Configuration access point

| Setting | Value |
|---|---|
| SSID | `FUMOIR_CONFIG` |
| Password | `Fumage2026` |
| Configuration URL | `http://192.168.4.1/wifi` |
| Dashboard URL | `http://192.168.4.1` |

Connect a smartphone, tablet or computer to the `FUMOIR_CONFIG` network and open the configuration page.

### Configuring the home Wi-Fi network

1. Connect to the `FUMOIR_CONFIG` access point.
2. Open `http://192.168.4.1/wifi` in a browser.
3. Enter your home Wi-Fi network name (SSID).
4. Enter your home Wi-Fi password.
5. Submit the form.

The ESP8266 saves the credentials in EEPROM and attempts to connect to the home Wi-Fi network.

**No firmware upload or restart is required.** The monitoring and conditioning processes continue running during Wi-Fi configuration.

The configuration access point remains active after connecting to the home network. This means that the Wi-Fi credentials can be changed at any time using the same procedure.

### Accessing the dashboard

The dashboard is available through either connection:

| Connection | URL |
|---|---|
| Configuration access point | `http://192.168.4.1` |
| Home Wi-Fi network | `http://fumoir.local` or the ESP8266 IP address assigned by the router |

The `fumoir.local` address requires mDNS support on the client device and a functioning local network.

The dashboard provides:

- Overall system status.
- Current smoker temperature.
- Smoke level and minimum threshold.
- Smoke sufficiency indication.
- Initial conditioning progress.
- Calibration controls and progress.
- Sensor and Wi-Fi status.
- Technical measurements.
- A link to the Wi-Fi configuration page.

The interface refreshes automatically without requiring a page reload.

## Installation

### Requirements

- Arduino IDE
- ESP8266 Arduino Core
- NodeMCU ESP8266 board support

Required libraries:

- OneWire
- DallasTemperature

The following libraries are included with the ESP8266 Arduino Core:

- ESP8266WiFi
- ESP8266WebServer
- ESP8266mDNS
- EEPROM

### Uploading the firmware

1. Install the ESP8266 board support in Arduino IDE.
2. Install the OneWire and DallasTemperature libraries.
3. Select your NodeMCU ESP8266 board.
4. Connect the board through USB.
5. Upload the firmware.
6. Open the Serial Monitor at 115200 baud.

### First startup

1. Power on the NodeMCU.
2. Connect a smartphone or computer to the `FUMOIR_CONFIG` Wi-Fi network.
3. Open `http://192.168.4.1/wifi`.
4. Enter your home Wi-Fi credentials and save them.
5. The ESP8266 attempts to connect to the configured network.
6. Access the dashboard through `http://192.168.4.1` or, when connected to the home network, `http://fumoir.local` if mDNS is supported.
7. Allow the initial 24-hour conditioning period to complete.
8. Start the smoker under normal operating conditions.
9. Launch the manual calibration from the dashboard.
10. Wait for stabilization and acquisition to finish.
11. The system automatically enters monitoring mode.

The configuration access point remains available throughout the entire process, including after the home Wi-Fi connection is established.

## EEPROM persistence

The firmware stores the following information:

**Smoker monitoring data:**

- Initial conditioning progress.
- Calibrated minimum smoke threshold.
- Conditioning completion status.

**Wi-Fi configuration:**

- Home Wi-Fi SSID.
- Home Wi-Fi password.

The conditioning progress is saved every 5 minutes.

Consequently, a sudden power loss can result in losing up to approximately 5 minutes of conditioning progress.

Wi-Fi credentials are saved when the configuration form is submitted. They remain available after a power cycle, so there is no need to configure the network again.

The EEPROM data format uses magic values to identify valid stored data. Changing the stored data structures or their magic values may require a fresh initialization.

**Security note:** Wi-Fi credentials are stored in the ESP8266 EEPROM emulation and are not encrypted by the application. Anyone with physical access to the device and the ability to read its flash memory may be able to retrieve them. Use a strong password for the configuration access point.

## Serial diagnostics

The Serial Monitor provides regular diagnostic information, including:

- Current system state.
- Configuration access point status.
- Home Wi-Fi connection status.
- DS18B20 temperature.
- Raw MQ-2 ADC value.
- Calibrated minimum threshold.
- Deviation from the threshold.
- Smoke alert status.
- Sensor fault status.
- Conditioning and calibration progress.

Baud rate: **115200**

## Configuration

The main operating parameters can be modified directly in the firmware.

```cpp
// Initial conditioning period
const uint64_t INITIAL_PREHEAT_MS =
    24ULL * 60ULL * 60ULL * 1000ULL;

// Calibration stabilization
const unsigned long CALIBRATION_STABILIZATION_MS = 300000UL;

// Number of calibration measurements
const int CALIBRATION_SAMPLES = 100;

// Margin below the calibrated smoke level
const float MIN_SMOKE_MARGIN_PERCENT = 15.0;
```

The configuration access point credentials are defined in the firmware:

```cpp
const char* AP_SSID = "FUMOIR_CONFIG";
const char* AP_PASSWORD = "Fumage2026";
```

These credentials are independent of the home Wi-Fi credentials.

Home Wi-Fi credentials are no longer hardcoded in the firmware. They are entered through the web configuration page and stored in EEPROM.

## Limitations and safety considerations

- The MQ-2 is a broad gas and smoke sensor, not a precision smoke concentration instrument.
- Its output is affected by other gases, temperature, humidity, sensor aging and operating conditions.
- The analog reading is not a calibrated ppm measurement.
- The 24-hour conditioning period is a configurable initial operating parameter; follow the requirements of the actual sensor manufacturer.
- The MQ-2 must not be used as a fire, carbon monoxide or life-safety detector.
- The dashboard is intended for monitoring and does not replace physical supervision of the smoker.
- The configuration access point uses a default password. Change it in the firmware before deployment.
- Wi-Fi connectivity is not required for the local monitoring logic to continue operating.
- The configuration access point remains available even if the home router is offline.

## Project status

The project is based on an ESP8266 firmware implementation with a browser-based monitoring interface and a persistent Wi-Fi configuration portal.

Hardware validation, calibration behavior and sensor response should be tested with the actual smoker setup before relying on the alerts.

---

**Built with ESP8266, Arduino and a little smoke.**
