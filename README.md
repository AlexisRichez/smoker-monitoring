# Cold Smoker Monitoring System

A Wi-Fi-enabled monitoring system for cold smoking, based on an ESP8266 NodeMCU, a DS18B20 temperature sensor and an MQ-2 smoke sensor.

The system continuously monitors the smoker temperature and smoke level, detects insufficient smoke and provides a responsive web dashboard accessible from a smartphone or computer.

## Features

- 🌡️ **Temperature monitoring** using a DS18B20 digital sensor.
- ☁️ **Smoke monitoring** using an MQ-2 analog sensor.
- 📉 **Minimum smoke threshold** detection to identify insufficient smoke.
- ⏱️ **Initial sensor conditioning** with a non-blocking 24-hour period.
- 🎯 **Manual calibration** based on the smoke level under normal operating conditions.
- 📶 **Standalone Wi-Fi access point** — no router or Internet connection required.
- 🌐 **Responsive web dashboard** for real-time monitoring.
- 💾 **EEPROM persistence** for the conditioning progress and calibrated smoke threshold.
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

## Web dashboard

The system creates its own Wi-Fi access point.

| Setting | Value |
|---|---|
| SSID | `FUMOIR_COLD` |
| Password | `Fumage2026` |
| Dashboard URL | `http://192.168.4.1` |

Connect a smartphone, tablet or computer to the access point and open the dashboard in a web browser.

The dashboard provides:

- Overall system status.
- Current smoker temperature.
- Smoke level and minimum threshold.
- Smoke sufficiency indication.
- Initial conditioning progress.
- Calibration controls and progress.
- Sensor and Wi-Fi status.
- Technical measurements.

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

EEPROM support is also provided by the ESP8266 Arduino Core.

### Uploading the firmware

1. Install the ESP8266 board support in Arduino IDE.
2. Install the OneWire and DallasTemperature libraries.
3. Select your NodeMCU ESP8266 board.
4. Connect the board through USB.
5. Upload the firmware.
6. Open the Serial Monitor at 115200 baud.

### First startup

1. Connect to the `FUMOIR_COLD` Wi-Fi network.
2. Open `http://192.168.4.1`.
3. Allow the initial 24-hour conditioning period to complete.
4. Start the smoker under normal operating conditions.
5. Launch the manual calibration from the dashboard.
6. Wait for stabilization and acquisition to finish.
7. The system automatically enters monitoring mode.

## EEPROM persistence

The firmware stores the following information:

- Initial conditioning progress.
- Calibrated minimum smoke threshold.
- Conditioning completion status.

The conditioning progress is saved every 5 minutes.

Consequently, a sudden power loss can result in losing up to approximately 5 minutes of conditioning progress.

The EEPROM data format is identified by a magic value. Changing the stored data structure or its magic value may require a fresh initialization.

## Serial diagnostics

The Serial Monitor provides regular diagnostic information, including:

- Current system state.
- Wi-Fi access point status.
- DS18B20 temperature.
- Raw MQ-2 ADC value.
- Calibrated minimum threshold.
- Deviation from the threshold.
- Smoke alert status.
- Sensor fault status.
- Conditioning and calibration progress.

Baud rate: **115200**

## Configuration

The main parameters can be modified directly in the firmware.

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

The Wi-Fi credentials can also be changed:

```cpp
const char* WIFI_SSID = "FUMOIR_COLD";
const char* WIFI_PASSWORD = "Fumage2026";
```

## Limitations and safety considerations

- The MQ-2 is a broad gas and smoke sensor, not a precision smoke concentration instrument.
- Its output is affected by other gases, temperature, humidity, sensor aging and operating conditions.
- The analog reading is not a calibrated ppm measurement.
- The 24-hour conditioning period is a configurable initial operating parameter; follow the requirements of the actual sensor manufacturer.
- The MQ-2 must not be used as a fire, carbon monoxide or life-safety detector.
- The dashboard is intended for monitoring and does not replace physical supervision of the smoker.
- The default Wi-Fi password should be changed before deployment.

## Project status

The project is based on an ESP8266 firmware implementation with a browser-based monitoring interface.

Hardware validation, calibration behavior and sensor response should be tested with the actual smoker setup before relying on the alerts.

---

**Built with ESP8266, Arduino and a little smoke.**
