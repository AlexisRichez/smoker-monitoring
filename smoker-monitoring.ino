
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>

// =====================================================
// CONFIGURATION DES BROCHES
// =====================================================

#define DS18B20_PIN D0
#define MQ2_PIN A0

#define LED_SYSTEM_STATUS       D1
#define LED_CALIBRATION_STATUS  D2
#define LED_SMOKE_ALERT         D5
#define LED_SENSOR_FAULT        D6

#define LED_ON  HIGH
#define LED_OFF LOW

// =====================================================
// CONFIGURATION WIFI
// =====================================================

const char* AP_SSID = "FUMOIR_COLD";
const char* AP_PASSWORD = "Fumage2026";

ESP8266WebServer server(80);

// =====================================================
// CAPTEURS
// =====================================================

OneWire oneWire(DS18B20_PIN);
DallasTemperature ds18b20(&oneWire);

// =====================================================
// PARAMETRES
// =====================================================

const uint64_t INITIAL_PREHEAT_MS =
    24ULL * 60ULL * 60ULL * 1000ULL;

const unsigned long STABILIZATION_MS = 300000UL;

const int CALIBRATION_SAMPLES = 100;
const unsigned long CALIBRATION_INTERVAL_MS = 200UL;

const unsigned long EEPROM_SAVE_INTERVAL_MS = 300000UL;

const unsigned long TEMP_CONVERSION_MS = 750UL;
const unsigned long TEMP_REFRESH_MS = 2000UL;

const unsigned long MQ2_REFRESH_MS = 500UL;
const unsigned long SERIAL_REFRESH_MS = 1000UL;

const float SMOKE_THRESHOLD_PERCENT = 15.0;

const int MQ2_ADC_MIN = 5;
const int MQ2_ADC_MAX = 1018;

const unsigned long AP_RETRY_INTERVAL_MS = 10000UL;

// =====================================================
// ETATS DU SYSTEME
// =====================================================

enum SystemState {
  INITIAL_PREHEAT,
  WAIT_CALIBRATION,
  STABILIZATION,
  CALIBRATION,
  MONITORING
};

SystemState systemState = INITIAL_PREHEAT;

// =====================================================
// EEPROM
// =====================================================

struct PersistentData {
  uint32_t magic;
  uint64_t preheatElapsed;
  int baseline;
  bool conditioned;
};

const uint32_t EEPROM_MAGIC = 0xF0A01234;
const int EEPROM_SIZE = 128;

PersistentData persistentData;

// =====================================================
// VARIABLES CAPTEURS
// =====================================================

float temperatureC = NAN;
bool temperatureOK = false;

int mq2Raw = 0;
int mq2Baseline = 0;

float smokeVariation = 0.0;
bool smokeAlert = false;
bool mq2SensorFault = false;

// =====================================================
// VARIABLES TEMPORELLES
// =====================================================

unsigned long lastTempRequest = 0;
unsigned long lastTempRefresh = 0;
unsigned long lastMQ2Refresh = 0;
unsigned long lastSerialRefresh = 0;

unsigned long lastPreheatUpdate = 0;
unsigned long lastEEPROMsave = 0;

unsigned long stabilizationStart = 0;

unsigned long lastCalibrationSample = 0;

unsigned long lastLEDUpdate = 0;
unsigned long lastAPRetry = 0;

// =====================================================
// VARIABLES CONDITIONNEMENT
// =====================================================

uint64_t preheatElapsed = 0;

bool preheatCompleted = false;

// =====================================================
// VARIABLES CALIBRATION
// =====================================================

int calibrationCount = 0;
long calibrationSum = 0;

// =====================================================
// WIFI
// =====================================================

bool hotspotOK = false;

// =====================================================
// LED
// =====================================================

bool systemLedState = false;

// =====================================================
// EEPROM : SAUVEGARDE
// =====================================================

void saveEEPROM() {

  persistentData.magic = EEPROM_MAGIC;
  persistentData.preheatElapsed = preheatElapsed;
  persistentData.baseline = mq2Baseline;
  persistentData.conditioned = preheatCompleted;

  EEPROM.put(0, persistentData);
  EEPROM.commit();

}

// =====================================================
// EEPROM : LECTURE
// =====================================================

void loadEEPROM() {

  EEPROM.get(0, persistentData);

  if (persistentData.magic != EEPROM_MAGIC) {

    preheatElapsed = 0;
    mq2Baseline = 0;
    preheatCompleted = false;

    saveEEPROM();

    return;
  }

  preheatElapsed = persistentData.preheatElapsed;
  mq2Baseline = persistentData.baseline;
  preheatCompleted = persistentData.conditioned;

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {
    preheatCompleted = true;
  }

  if (preheatCompleted) {
    systemState = WAIT_CALIBRATION;
  }

}

// =====================================================
// TEMPERATURE DS18B20
// =====================================================

void updateTemperature() {

  unsigned long now = millis();

  if (now - lastTempRefresh >= TEMP_REFRESH_MS) {

    lastTempRefresh = now;

    if (lastTempRequest == 0 ||
        now - lastTempRequest >= TEMP_CONVERSION_MS) {

      ds18b20.requestTemperatures();

      lastTempRequest = now;

    }

  }

  if (lastTempRequest != 0 &&
      now - lastTempRequest >= TEMP_CONVERSION_MS) {

    float value = ds18b20.getTempCByIndex(0);

    if (value != DEVICE_DISCONNECTED_C &&
        value >= -55.0 &&
        value <= 125.0) {

      temperatureC = value;
      temperatureOK = true;

    } else {

      temperatureOK = false;

    }

    lastTempRequest = 0;

  }

}

// =====================================================
// CAPTEUR MQ-2
// =====================================================

void updateMQ2() {

  unsigned long now = millis();

  if (now - lastMQ2Refresh < MQ2_REFRESH_MS) {
    return;
  }

  lastMQ2Refresh = now;

  mq2Raw = analogRead(MQ2_PIN);

  mq2SensorFault =
      (mq2Raw <= MQ2_ADC_MIN ||
       mq2Raw >= MQ2_ADC_MAX);

  if (mq2Baseline > 0) {

    smokeVariation =
        ((float)(mq2Raw - mq2Baseline) /
         (float)mq2Baseline) * 100.0;

  } else {

    smokeVariation = 0.0;

  }

  smokeAlert =
      (systemState == MONITORING &&
       !mq2SensorFault &&
       mq2Baseline > 0 &&
       smokeVariation >= SMOKE_THRESHOLD_PERCENT);

}

// =====================================================
// CONDITIONNEMENT INITIAL
// =====================================================

void updatePreheat() {

  if (systemState != INITIAL_PREHEAT) {
    return;
  }

  unsigned long now = millis();

  preheatElapsed += now - lastPreheatUpdate;
  lastPreheatUpdate = now;

  if (now - lastEEPROMsave >= EEPROM_SAVE_INTERVAL_MS) {

    saveEEPROM();
    lastEEPROMsave = now;

  }

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {

    preheatElapsed = INITIAL_PREHEAT_MS;

    preheatCompleted = true;

    systemState = WAIT_CALIBRATION;

    saveEEPROM();

    Serial.println();
    Serial.println(">>> CONDITIONNEMENT INITIAL TERMINE");
    Serial.println(">>> Calibration manuelle requise");

  }

}

// =====================================================
// DEMARRAGE CALIBRATION
// =====================================================

void startCalibration() {

  if (systemState != WAIT_CALIBRATION &&
      systemState != MONITORING) {
    return;
  }

  calibrationCount = 0;
  calibrationSum = 0;

  smokeAlert = false;

  stabilizationStart = millis();

  systemState = STABILIZATION;

  Serial.println();
  Serial.println(">>> DEBUT DE LA CALIBRATION");
  Serial.println(">>> Stabilisation MQ-2 : 5 minutes");

}

// =====================================================
// CALIBRATION
// =====================================================

void updateCalibration() {

  unsigned long now = millis();

  if (systemState == STABILIZATION) {

    if (now - stabilizationStart >= STABILIZATION_MS) {

      systemState = CALIBRATION;

      calibrationCount = 0;
      calibrationSum = 0;

      lastCalibrationSample = 0;

      Serial.println();
      Serial.println(">>> STABILISATION TERMINEE");
      Serial.println(">>> Acquisition des mesures MQ-2");

    }

    return;
  }

  if (systemState != CALIBRATION) {
    return;
  }

  if (now - lastCalibrationSample <
      CALIBRATION_INTERVAL_MS) {
    return;
  }

  lastCalibrationSample = now;

  int value = analogRead(MQ2_PIN);

  if (value <= MQ2_ADC_MIN ||
      value >= MQ2_ADC_MAX) {
    return;
  }

  calibrationSum += value;
  calibrationCount++;

  if (calibrationCount % 10 == 0) {

    Serial.print("Calibration : ");
    Serial.print(calibrationCount);
    Serial.print("/");
    Serial.println(CALIBRATION_SAMPLES);

  }

  if (calibrationCount >= CALIBRATION_SAMPLES) {

    mq2Baseline =
        calibrationSum / CALIBRATION_SAMPLES;

    if (mq2Baseline > 0) {

      systemState = MONITORING;

      saveEEPROM();

      Serial.println();
      Serial.println(">>> CALIBRATION TERMINEE");
      Serial.print(">>> Valeur de reference MQ-2 : ");
      Serial.println(mq2Baseline);

    } else {

      systemState = WAIT_CALIBRATION;

      Serial.println(">>> ERREUR : calibration invalide");

    }

  }

}

// =====================================================
// WIFI : HOTSPOT
// =====================================================

void updateHotspot() {

  bool connected =
      (WiFi.getMode() == WIFI_AP &&
       WiFi.softAPIP() != IPAddress(0, 0, 0, 0));

  hotspotOK = connected;

  if (!hotspotOK &&
      millis() - lastAPRetry >= AP_RETRY_INTERVAL_MS) {

    lastAPRetry = millis();

    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASSWORD);

    Serial.println(">>> Tentative de redemarrage du hotspot");

  }

}

// =====================================================
// ETAT CAPTEURS
// =====================================================

bool hasSensorFault() {

  if (!temperatureOK) {
    return true;
  }

  if (mq2SensorFault) {
    return true;
  }

  if (systemState == MONITORING &&
      mq2Baseline <= 0) {
    return true;
  }

  return false;

}

// =====================================================
// GESTION DES LED
// =====================================================

void updateStatusLeds() {

  unsigned long now = millis();

  bool sensorFault = hasSensorFault();

  bool systemOperational =
      hotspotOK &&
      systemState == MONITORING &&
      !sensorFault;

  if (systemOperational) {

    digitalWrite(LED_SYSTEM_STATUS, LED_ON);

  } else {

    if (now - lastLEDUpdate >= 500) {

      lastLEDUpdate = now;
      systemLedState = !systemLedState;

    }

    digitalWrite(
      LED_SYSTEM_STATUS,
      systemLedState ? LED_ON : LED_OFF
    );

  }

  bool calibrationActive =
      (systemState == INITIAL_PREHEAT ||
       systemState == STABILIZATION ||
       systemState == CALIBRATION);

  digitalWrite(
    LED_CALIBRATION_STATUS,
    calibrationActive ? LED_ON : LED_OFF
  );

  digitalWrite(
    LED_SMOKE_ALERT,
    smokeAlert ? LED_ON : LED_OFF
  );

  digitalWrite(
    LED_SENSOR_FAULT,
    sensorFault ? LED_ON : LED_OFF
  );

}

// =====================================================
// NOM DES ETATS
// =====================================================

String getStateName() {

  switch (systemState) {

    case INITIAL_PREHEAT:
      return "Conditionnement initial en cours";

    case WAIT_CALIBRATION:
      return "Conditionnement termine - Calibration requise";

    case STABILIZATION:
      return "Stabilisation MQ-2";

    case CALIBRATION:
      return "Calibration MQ-2";

    case MONITORING:
      return "Surveillance";

    default:
      return "Inconnu";

  }

}

// =====================================================
// FORMATAGE D'UNE DUREE
// =====================================================

String formatDuration(uint64_t durationMs) {

  uint64_t totalSeconds = durationMs / 1000ULL;

  uint64_t hours = totalSeconds / 3600ULL;
  uint64_t minutes = (totalSeconds % 3600ULL) / 60ULL;

  return String((unsigned long)hours) +
         "h " +
         String((unsigned long)minutes) +
         "m";

}

// =====================================================
// AFFICHAGE CONSOLE
// =====================================================

void printSerialStatus() {

  unsigned long now = millis();

  if (now - lastSerialRefresh < SERIAL_REFRESH_MS) {
    return;
  }

  lastSerialRefresh = now;

  Serial.println();
  Serial.println("========================================");
  Serial.println("           ETAT DU FUMOIR");
  Serial.println("========================================");

  Serial.print("Etat systeme       : ");
  Serial.println(getStateName());

  Serial.print("Hotspot Wi-Fi      : ");
  Serial.println(hotspotOK ? "ACTIF" : "INACTIF");

  Serial.print("Adresse IP         : ");
  Serial.println(WiFi.softAPIP());

  Serial.println();
  Serial.println("--- CAPTEUR MQ-2 ---");

  Serial.print("Valeur brute ADC   : ");
  Serial.println(mq2Raw);

  Serial.print("Valeur de reference: ");

  if (mq2Baseline > 0) {
    Serial.println(mq2Baseline);
  } else {
    Serial.println("NON CALIBRE");
  }

  Serial.print("Variation          : ");
  Serial.print(smokeVariation, 2);
  Serial.println(" %");

  Serial.print("Alerte fumee       : ");
  Serial.println(smokeAlert ? "OUI" : "NON");

  Serial.print("Defaut MQ-2        : ");
  Serial.println(mq2SensorFault ? "OUI" : "NON");

  Serial.println();
  Serial.println("--- CAPTEUR DS18B20 ---");

  Serial.print("Temperature        : ");

  if (temperatureOK) {
    Serial.print(temperatureC, 2);
    Serial.println(" °C");
  } else {
    Serial.println("ERREUR / NON DETECTE");
  }

  Serial.print("Etat               : ");
  Serial.println(temperatureOK ? "OK" : "DEFAUT");

  Serial.println();
  Serial.println("--- CONDITIONNEMENT ---");

  Serial.print("Duree ecoulee      : ");
  Serial.println(formatDuration(preheatElapsed));

  if (systemState == INITIAL_PREHEAT) {

    uint64_t remaining =
        INITIAL_PREHEAT_MS - preheatElapsed;

    Serial.print("Duree restante     : ");
    Serial.println(formatDuration(remaining));

  } else {

    Serial.println("Duree restante     : 0h 00m");

  }

  Serial.println();
  Serial.println("--- DEFAUTS ---");

  Serial.print("Defaut capteur     : ");
  Serial.println(hasSensorFault() ? "OUI" : "NON");

  Serial.println("========================================");

}

// =====================================================
// PAGE WEB
// =====================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fumoir</title>
<style>
body {
  font-family: Arial;
  background: #20242a;
  color: white;
  text-align: center;
  margin: 0;
  padding: 20px;
}
.card {
  background: #303740;
  border-radius: 10px;
  padding: 15px;
  margin: 12px auto;
  max-width: 420px;
}
.value {
  font-size: 28px;
  font-weight: bold;
  margin: 10px;
}
button {
  padding: 14px 20px;
  font-size: 16px;
  border: 0;
  border-radius: 7px;
  background: #16a085;
  color: white;
}
button:disabled {
  background: #555;
}
</style>
</head>
<body>

<h1>Fumoir froid</h1>

<div class="card">
  <h2>Etat du systeme</h2>
  <div class="value" id="state">...</div>
  <p id="wifi">Hotspot : ...</p>
</div>

<div class="card">
  <h2>Temperature</h2>
  <div class="value" id="temperature">-- °C</div>
  <p id="temperatureStatus"></p>
</div>

<div class="card">
  <h2>Capteur MQ-2</h2>
  <div class="value" id="mq2">---</div>
  <p>Valeur de reference : <span id="baseline">---</span></p>
  <p>Variation : <span id="variation">---</span> %</p>
  <p id="smokeAlert"></p>
</div>

<div class="card">
  <h2>Conditionnement initial</h2>
  <p>Ecoule : <span id="elapsed">---</span></p>
  <p>Restant : <span id="remaining">---</span></p>
</div>

<div class="card">
  <h2>Calibration</h2>
  <button id="calibrate" onclick="calibrate()">Lancer la calibration</button>
  <p id="calibrationInfo"></p>
</div>

<script>
function formatDuration(ms) {
  let total = Math.floor(ms / 1000);
  let h = Math.floor(total / 3600);
  let m = Math.floor((total % 3600) / 60);
  return h + "h " + m + "min";
}

async function refresh() {
  try {
    const response = await fetch('/data');
    const d = await response.json();

    document.getElementById('state').textContent = d.state;
    document.getElementById('wifi').textContent =
      'Hotspot : ' + (d.hotspot ? 'ACTIF' : 'INACTIF');

    document.getElementById('temperature').textContent =
      d.temperatureOK ? d.temperature.toFixed(2) + ' °C' : 'ERREUR';

    document.getElementById('temperatureStatus').textContent =
      d.temperatureOK ? 'Capteur OK' : 'Capteur non detecte';

    document.getElementById('mq2').textContent = d.mq2;
    document.getElementById('baseline').textContent =
      d.baseline > 0 ? d.baseline : 'Non calibre';

    document.getElementById('variation').textContent =
      d.variation.toFixed(2);

    document.getElementById('smokeAlert').textContent =
      d.smokeAlert ? 'ALERTE FUMEE' : 'Pas d alerte';

    document.getElementById('elapsed').textContent =
      formatDuration(d.preheatElapsed);

    document.getElementById('remaining').textContent =
      formatDuration(d.preheatRemaining);

    const canCalibrate =
      d.stateCode === 1 || d.stateCode === 4;

    document.getElementById('calibrate').disabled = !canCalibrate;

    document.getElementById('calibrationInfo').textContent =
      d.stateCode === 0
        ? 'Conditionnement initial en cours'
        : d.stateCode === 1
          ? 'Pret pour la calibration'
          : d.stateCode === 2
            ? 'Stabilisation en cours'
            : d.stateCode === 3
              ? 'Acquisition des mesures'
              : 'Surveillance active';

  } catch (e) {
    document.getElementById('state').textContent =
      'Connexion perdue';
  }
}

async function calibrate() {
  await fetch('/calibrate', { method: 'POST' });
  refresh();
}

setInterval(refresh, 2000);
refresh();
</script>

</body>
</html>
)rawliteral";

// =====================================================
// DONNEES JSON
// =====================================================

void handleData() {

  uint64_t remaining = 0;

  if (systemState == INITIAL_PREHEAT &&
      preheatElapsed < INITIAL_PREHEAT_MS) {

    remaining = INITIAL_PREHEAT_MS - preheatElapsed;

  }

  String json = "{";

  json += "\"state\":\"" + getStateName() + "\",";
  json += "\"stateCode\":" + String((int)systemState) + ",";
  json += "\"hotspot\":" + String(hotspotOK ? "true" : "false") + ",";
  json += "\"temperatureOK\":" + String(temperatureOK ? "true" : "false") + ",";
  json += "\"temperature\":" + String(temperatureOK ? temperatureC : 0.0, 2) + ",";
  json += "\"mq2\":" + String(mq2Raw) + ",";
  json += "\"baseline\":" + String(mq2Baseline) + ",";
  json += "\"variation\":" + String(smokeVariation, 2) + ",";
  json += "\"smokeAlert\":" + String(smokeAlert ? "true" : "false") + ",";
  json += "\"preheatElapsed\":" + String((unsigned long)(preheatElapsed)) + ",";
  json += "\"preheatRemaining\":" + String((unsigned long)(remaining));

  json += "}";

  server.send(200, "application/json", json);

}

// =====================================================
// ROUTE CALIBRATION
// =====================================================

void handleCalibrate() {

  startCalibration();

  server.send(200, "text/plain", "OK");

}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("========================================");
  Serial.println("        DEMARRAGE DU FUMOIR");
  Serial.println("========================================");

  pinMode(LED_SYSTEM_STATUS, OUTPUT);
  pinMode(LED_CALIBRATION_STATUS, OUTPUT);
  pinMode(LED_SMOKE_ALERT, OUTPUT);
  pinMode(LED_SENSOR_FAULT, OUTPUT);

  digitalWrite(LED_SYSTEM_STATUS, LED_OFF);
  digitalWrite(LED_CALIBRATION_STATUS, LED_OFF);
  digitalWrite(LED_SMOKE_ALERT, LED_OFF);
  digitalWrite(LED_SENSOR_FAULT, LED_OFF);

  EEPROM.begin(EEPROM_SIZE);
  loadEEPROM();

  ds18b20.begin();
  ds18b20.setResolution(12);
  ds18b20.setWaitForConversion(false);

  WiFi.mode(WIFI_AP);
  hotspotOK = WiFi.softAP(AP_SSID, AP_PASSWORD);

  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", MAIN_PAGE);
  });

  server.on("/data", HTTP_GET, handleData);
  server.on("/calibrate", HTTP_POST, handleCalibrate);

  server.begin();

  lastPreheatUpdate = millis();
  lastEEPROMsave = millis();
  lastTempRefresh = 0;
  lastMQ2Refresh = 0;

  Serial.print("Hotspot : ");
  Serial.println(hotspotOK ? "ACTIF" : "ERREUR");

  Serial.print("Adresse IP : ");
  Serial.println(WiFi.softAPIP());

  Serial.print("SSID : ");
  Serial.println(AP_SSID);

  Serial.print("Etat initial : ");
  Serial.println(getStateName());

  Serial.println("Serveur web demarre.");

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

  printSerialStatus();

  yield();

}
