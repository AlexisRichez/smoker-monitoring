
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>

// =====================================================
// PIN CONFIGURATION - NODEMCU ESP8266
// =====================================================

#define DS18B20_PIN D0       // GPIO16
#define MQ2_PIN A0

#define LED_GREEN  D1         // GPIO5
#define LED_YELLOW D2         // GPIO4
#define LED_RED    D5         // GPIO14
#define LED_BLUE   D6         // GPIO12

// =====================================================
// WIFI ACCESS POINT
// =====================================================

const char* ssid = "FUMOIR_COLD";
const char* password = "Fumage2026";

ESP8266WebServer server(80);

// =====================================================
// SENSORS
// =====================================================

OneWire oneWire(DS18B20_PIN);
DallasTemperature sensors(&oneWire);

float temperature = NAN;
bool temperatureOK = false;

int mq2Raw = 0;
int mq2Baseline = 0;

float smokeVariation = 0.0;

bool smokeAlert = false;
bool mq2SensorFault = false;

// =====================================================
// PARAMETERS
// =====================================================

// Initial conditioning : 24 hours
const uint64_t INITIAL_PREHEAT_MS =
  24ULL * 60ULL * 60ULL * 1000ULL;

// Stabilization before calibration : 5 minutes
const unsigned long STABILIZATION_MS = 300000UL;

// Calibration
const int CALIBRATION_SAMPLES = 100;
const unsigned long CALIBRATION_INTERVAL = 200UL;

// EEPROM save interval
const unsigned long SAVE_INTERVAL = 300000UL;

// DS18B20 conversion time at 12 bits
const unsigned long TEMP_CONVERSION_MS = 750UL;

// Temperature refresh interval
const unsigned long TEMP_REFRESH_MS = 2000UL;

// MQ2 sampling interval
const unsigned long MQ2_REFRESH_MS = 500UL;

// Smoke threshold (% relative to baseline)
const float SMOKE_ALERT_THRESHOLD = 15.0;

// Hotspot retry interval
const unsigned long AP_RETRY_INTERVAL = 10000UL;

// =====================================================
// SYSTEM STATES
// =====================================================

enum SystemState {
  INITIAL_PREHEAT,
  WAIT_CALIBRATION,
  STABILIZATION,
  CALIBRATION,
  MONITORING
};

SystemState state = INITIAL_PREHEAT;

// =====================================================
// EEPROM
// =====================================================

#define EEPROM_SIZE 128

const uint32_t EEPROM_MAGIC = 0xF0A01234;

struct PersistentData {
  uint32_t magic;
  uint64_t preheatElapsed;
  int baseline;
  bool conditioned;
};

PersistentData persistentData;

uint64_t preheatElapsed = 0;

// =====================================================
// TIMERS
// =====================================================

unsigned long lastPreheatUpdate = 0;
unsigned long lastSave = 0;

unsigned long lastTempRequest = 0;
unsigned long lastTempRefresh = 0;
bool temperaturePending = false;

unsigned long lastMQ2Read = 0;

unsigned long lastGreenBlink = 0;
bool greenLedState = false;

unsigned long stabilizationStart = 0;

unsigned long calibrationLastSample = 0;
int calibrationCount = 0;
long calibrationSum = 0;

unsigned long lastAPRetry = 0;

// =====================================================
// STATUS
// =====================================================

bool hotspotOK = false;

// =====================================================
// EEPROM FUNCTIONS
// =====================================================

void saveData() {

  persistentData.magic = EEPROM_MAGIC;
  persistentData.preheatElapsed = preheatElapsed;
  persistentData.baseline = mq2Baseline;

  persistentData.conditioned =
    preheatElapsed >= INITIAL_PREHEAT_MS;

  EEPROM.put(0, persistentData);
  EEPROM.commit();
}

void loadData() {

  EEPROM.get(0, persistentData);

  if (persistentData.magic != EEPROM_MAGIC) {

    preheatElapsed = 0;
    mq2Baseline = 0;

    state = INITIAL_PREHEAT;

    saveData();
    return;
  }

  preheatElapsed = persistentData.preheatElapsed;
  mq2Baseline = persistentData.baseline;

  if (preheatElapsed > INITIAL_PREHEAT_MS)
    preheatElapsed = INITIAL_PREHEAT_MS;

  if (persistentData.conditioned ||
      preheatElapsed >= INITIAL_PREHEAT_MS) {

    preheatElapsed = INITIAL_PREHEAT_MS;
    state = WAIT_CALIBRATION;

  } else {

    state = INITIAL_PREHEAT;
  }
}

// =====================================================
// TEMPERATURE
// =====================================================

void requestTemperature() {

  sensors.requestTemperatures();

  lastTempRequest = millis();
  temperaturePending = true;
}

void updateTemperature() {

  unsigned long now = millis();

  // Retrieve the result after conversion
  if (temperaturePending &&
      now - lastTempRequest >= TEMP_CONVERSION_MS) {

    float t = sensors.getTempCByIndex(0);

    if (t != DEVICE_DISCONNECTED_C &&
        t >= -55.0 &&
        t <= 125.0) {

      temperature = t;
      temperatureOK = true;

    } else {

      temperature = NAN;
      temperatureOK = false;
    }

    temperaturePending = false;
  }

  // Request the next conversion
  if (!temperaturePending &&
      now - lastTempRefresh >= TEMP_REFRESH_MS) {

    requestTemperature();
    lastTempRefresh = now;
  }
}

// =====================================================
// MQ2
// =====================================================

void updateMQ2() {

  if (millis() - lastMQ2Read < MQ2_REFRESH_MS)
    return;

  lastMQ2Read = millis();

  mq2Raw = analogRead(MQ2_PIN);

  // Detect values close to ADC limits
  mq2SensorFault =
    (mq2Raw <= 5 || mq2Raw >= 1018);

  // Also check the baseline during monitoring
  if (state == MONITORING &&
      (mq2Baseline <= 5 || mq2Baseline >= 1018)) {

    mq2SensorFault = true;
  }

  if (mq2Baseline > 0) {

    smokeVariation =
      ((float)(mq2Raw - mq2Baseline) /
       (float)mq2Baseline) * 100.0;

  } else {

    smokeVariation = 0.0;
  }

  smokeAlert =
    state == MONITORING &&
    !mq2SensorFault &&
    mq2Baseline > 0 &&
    smokeVariation >= SMOKE_ALERT_THRESHOLD;
}

// =====================================================
// INITIAL CONDITIONING
// =====================================================

void updatePreheat() {

  if (state != INITIAL_PREHEAT)
    return;

  unsigned long now = millis();

  if (lastPreheatUpdate == 0) {
    lastPreheatUpdate = now;
    return;
  }

  // Cumulative powered time
  preheatElapsed += now - lastPreheatUpdate;

  lastPreheatUpdate = now;

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {

    preheatElapsed = INITIAL_PREHEAT_MS;

    state = WAIT_CALIBRATION;

    saveData();

    lastSave = now;
  }

  // Save progress every 5 minutes
  if (now - lastSave >= SAVE_INTERVAL) {

    saveData();
    lastSave = now;
  }
}

// =====================================================
// CALIBRATION
// =====================================================

void startCalibration() {

  if (state != WAIT_CALIBRATION &&
      state != MONITORING)
    return;

  state = STABILIZATION;

  stabilizationStart = millis();

  smokeAlert = false;

  calibrationCount = 0;
  calibrationSum = 0;
}

void updateCalibration() {

  // Stabilization
  if (state == STABILIZATION) {

    if (millis() - stabilizationStart >=
        STABILIZATION_MS) {

      state = CALIBRATION;

      calibrationCount = 0;
      calibrationSum = 0;

      calibrationLastSample = 0;
    }

    return;
  }

  // Calibration sampling
  if (state != CALIBRATION)
    return;

  unsigned long now = millis();

  if (now - calibrationLastSample <
      CALIBRATION_INTERVAL)
    return;

  calibrationLastSample = now;

  int sample = analogRead(MQ2_PIN);

  calibrationSum += sample;
  calibrationCount++;

  if (calibrationCount >= CALIBRATION_SAMPLES) {

    mq2Baseline =
      calibrationSum / CALIBRATION_SAMPLES;

    state = MONITORING;

    smokeVariation = 0.0;
    smokeAlert = false;

    saveData();
  }
}

// =====================================================
// SYSTEM STATE
// =====================================================

String getStateName() {

  switch (state) {

    case INITIAL_PREHEAT:
      return "Calibration initiale en cours";

    case WAIT_CALIBRATION:
      return "Conditionnement termine - Calibration requise";

    case STABILIZATION:
      return "Stabilisation";

    case CALIBRATION:
      return "Calibration en cours";

    case MONITORING:
      return "Surveillance active";
  }

  return "Unknown";
}

// =====================================================
// HOTSPOT
// =====================================================

void updateHotspot() {

  hotspotOK =
    WiFi.getMode() == WIFI_AP &&
    WiFi.softAPIP() != IPAddress(0, 0, 0, 0);

  if (hotspotOK)
    return;

  if (millis() - lastAPRetry < AP_RETRY_INTERVAL)
    return;

  lastAPRetry = millis();

  WiFi.mode(WIFI_AP);

  hotspotOK = WiFi.softAP(ssid, password);
}

// =====================================================
// STATUS LEDS
// =====================================================

void updateStatusLeds() {

  // GREEN: hotspot + monitoring + no sensor faults

  bool sensorFault =
    !temperatureOK ||
    mq2SensorFault ||
    (state == MONITORING && mq2Baseline <= 0);

  bool operational =
    hotspotOK &&
    state == MONITORING &&
    !sensorFault;

  if (operational) {

    greenLedState = true;
    digitalWrite(LED_GREEN, HIGH);

  } else {

    if (millis() - lastGreenBlink >= 500UL) {

      lastGreenBlink = millis();

      greenLedState = !greenLedState;

      digitalWrite(LED_GREEN,
                   greenLedState ? HIGH : LOW);
    }
  }

  // YELLOW: initial conditioning,
  // stabilization or calibration

  bool calibrationInProgress =
    state == INITIAL_PREHEAT ||
    state == STABILIZATION ||
    state == CALIBRATION;

  digitalWrite(LED_YELLOW,
               calibrationInProgress ? HIGH : LOW);

  // RED: smoke alert

  digitalWrite(LED_RED,
               smokeAlert ? HIGH : LOW);

  // BLUE: sensor fault

  digitalWrite(LED_BLUE,
               sensorFault ? HIGH : LOW);
}

// =====================================================
// WEB DATA
// =====================================================

String getPreheatRemaining() {

  uint64_t remaining =
    INITIAL_PREHEAT_MS - preheatElapsed;

  uint64_t hours =
    remaining / 3600000ULL;

  uint64_t minutes =
    (remaining / 60000ULL) % 60ULL;

  return String((unsigned long)hours) +
         " h " +
         String((unsigned long)minutes) +
         " min";
}

void handleData() {

  String json = "{";

  json += "\"temperature\":";

  if (temperatureOK)
    json += String(temperature, 1);
  else
    json += "null";

  json += ",\"temperatureOK\":";
  json += temperatureOK ? "true" : "false";

  json += ",\"mq2Raw\":";
  json += String(mq2Raw);

  json += ",\"baseline\":";
  json += String(mq2Baseline);

  json += ",\"variation\":";
  json += String(smokeVariation, 1);

  json += ",\"smokeAlert\":";
  json += smokeAlert ? "true" : "false";

  json += ",\"mq2SensorFault\":";
  json += mq2SensorFault ? "true" : "false";

  json += ",\"hotspotOK\":";
  json += hotspotOK ? "true" : "false";

  json += ",\"state\":\"";
  json += getStateName();
  json += "\"";

  json += ",\"preheatRemaining\":\"";
  json += getPreheatRemaining();
  json += "\"";

  json += ",\"calibrationCount\":";
  json += String(calibrationCount);

  json += "}";

  server.send(200, "application/json", json);
}

// =====================================================
// CALIBRATION REQUEST
// =====================================================

void handleCalibration() {

  if (state == WAIT_CALIBRATION ||
      state == MONITORING) {

    startCalibration();

    server.send(200, "text/plain", "OK");

  } else {

    server.send(409,
                "text/plain",
                "Calibration unavailable");
  }
}

// =====================================================
// WEB PAGE
// =====================================================

void handleRoot() {

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
<meta charset="UTF-8">
<meta name="viewport"
      content="width=device-width,initial-scale=1">

<title>Fumoir Cold</title>

<style>

body {
  font-family: Arial, sans-serif;
  background: #111827;
  color: white;
  margin: 0;
  padding: 20px;
}

.container {
  max-width: 600px;
  margin: auto;
}

.card {
  background: #1f2937;
  padding: 20px;
  margin-bottom: 15px;
  border-radius: 12px;
}

h1 {
  text-align: center;
}

.value {
  font-size: 28px;
  font-weight: bold;
}

button {
  padding: 15px;
  width: 100%;
  border: none;
  border-radius: 8px;
  font-size: 16px;
  background: #16a34a;
  color: white;
  margin-top: 10px;
}

button:disabled {
  opacity: 0.5;
}

.alert {
  color: #f87171;
}

.good {
  color: #4ade80;
}

.small {
  color: #9ca3af;
}

</style>
</head>

<body>

<div class="container">

<h1>Fumoir Cold</h1>

<div class="card">
<h2>Temperature</h2>
<div class="value" id="temperature">--</div>
<div id="temperatureStatus"></div>
</div>

<div class="card">
<h2>MQ-2</h2>
<div>ADC : <span id="raw">--</span></div>
<div>Baseline : <span id="baseline">--</span></div>
<div class="value" id="variation">--</div>
<div id="smokeStatus"></div>
<div id="mq2Status"></div>
</div>

<div class="card">
<h2>System status</h2>
<div id="state" class="value">--</div>
<div id="hotspot"></div>
<div id="preheat"></div>
<div id="calibrationCount"></div>
</div>

<div class="card">
<h2>Calibration</h2>

<p>
Calibration must be performed in clean air.
</p>

<button id="calibrateButton"
        onclick="calibrate()">
Start calibration
</button>

<div id="calibrationMessage"></div>
</div>

<div class="card small">
Automatic update every 2 seconds.
</div>

</div>

<script>

async function refresh() {

  try {

    const response = await fetch('/data');
    const d = await response.json();

    document.getElementById('temperature').textContent =
      d.temperatureOK
        ? d.temperature + ' °C'
        : 'Sensor error';

    document.getElementById('temperatureStatus').textContent =
      d.temperatureOK
        ? 'DS18B20 OK'
        : 'DS18B20 unavailable';

    document.getElementById('raw').textContent =
      d.mq2Raw;

    document.getElementById('baseline').textContent =
      d.baseline;

    document.getElementById('variation').textContent =
      d.variation + ' %';

    document.getElementById('smokeStatus').textContent =
      d.smokeAlert
        ? 'SMOKE ALERT'
        : 'No threshold exceeded';

    document.getElementById('smokeStatus').className =
      d.smokeAlert ? 'alert' : 'good';

    document.getElementById('mq2Status').textContent =
      d.mq2SensorFault
        ? 'MQ-2 sensor fault detected'
        : 'MQ-2 electrical reading OK';

    document.getElementById('mq2Status').className =
      d.mq2SensorFault ? 'alert' : 'good';

    document.getElementById('state').textContent =
      d.state;

    document.getElementById('hotspot').textContent =
      d.hotspotOK
        ? 'Hotspot active'
        : 'Hotspot unavailable';

    document.getElementById('preheat').textContent =
      d.state === 'Calibration initiale en cours'
        ? 'Remaining : ' + d.preheatRemaining
        : '';

    document.getElementById('calibrationCount').textContent =
      d.state === 'Calibration en cours'
        ? 'Samples : ' + d.calibrationCount + '/100'
        : '';

    const btn =
      document.getElementById('calibrateButton');

    btn.disabled =
      d.state !== 'Conditionnement termine - Calibration requise' &&
      d.state !== 'Surveillance active';

  } catch(e) {

    document.getElementById('hotspot').textContent =
      'Communication error';
  }
}

async function calibrate() {

  const message =
    document.getElementById('calibrationMessage');

  message.textContent =
    'Calibration request sent...';

  try {

    const response =
      await fetch('/calibrate', {method:'POST'});

    message.textContent =
      response.ok
        ? 'Stabilization started'
        : 'Calibration unavailable';

  } catch(e) {

    message.textContent =
      'Communication error';
  }
}

refresh();

setInterval(refresh, 2000);

</script>

</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);

  // LED outputs

  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_BLUE, OUTPUT);

  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_BLUE, HIGH);

  // EEPROM

  EEPROM.begin(EEPROM_SIZE);
  loadData();

  // DS18B20

  sensors.begin();
  sensors.setResolution(12);
  sensors.setWaitForConversion(false);

  requestTemperature();

  // MQ2

  lastMQ2Read = millis();

  // WiFi access point

  WiFi.mode(WIFI_AP);

  hotspotOK =
    WiFi.softAP(ssid, password);

  Serial.println();
  Serial.println("Fumoir Cold");

  if (hotspotOK) {

    Serial.print("IP address: ");
    Serial.println(WiFi.softAPIP());

  } else {

    Serial.println("AP startup failed");
  }

  // Web server

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/calibrate",
            HTTP_POST,
            handleCalibration);

  server.begin();

  // Timers

  lastPreheatUpdate = millis();
  lastSave = millis();

  lastTempRequest = millis();
  lastTempRefresh = millis();

  lastAPRetry = millis();

  Serial.println("Web server started");
}

// =====================================================
// LOOP
// =====================================================

void loop() {

  server.handleClient();

  updateHotspot();

  updateTemperature();
  updateMQ2();

  updatePreheat();
  updateCalibration();

  updateStatusLeds();

  yield();
}
