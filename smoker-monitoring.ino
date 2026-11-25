
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <math.h>

// =====================================================
// CONFIGURATION DES BROCHES
// =====================================================

#define DS18B20_PIN D0
#define MQ2_PIN A0

#define LED_SYSTEM_STATUS       D1
#define LED_CALIBRATION_STATUS  D2
#define LED_SMOKE_ALERT         D5
#define LED_SENSOR_FAULT        D6

// Adapter ces deux valeurs au cablage des LED
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
// PARAMETRES GENERAUX
// =====================================================

// Conditionnement initial du MQ-2 : 24 heures
const uint64_t INITIAL_PREHEAT_MS =
    24ULL * 60ULL * 60ULL * 1000ULL;

// Stabilisation avant calibration : 5 minutes
const unsigned long STABILIZATION_MS = 300000UL;

// Calibration : 100 mesures espacées de 200 ms
const int CALIBRATION_SAMPLES = 100;
const unsigned long CALIBRATION_INTERVAL_MS = 200UL;

// Marge sous la moyenne de calibration
// Exemple : moyenne 400 -> seuil minimal 340
const float MIN_SMOKE_MARGIN_PERCENT = 15.0;

// Seuils de detection d'une anomalie ADC
const int MQ2_ADC_MIN = 5;
const int MQ2_ADC_MAX = 1018;

// Temporisations
const unsigned long EEPROM_SAVE_INTERVAL_MS = 300000UL;
const unsigned long TEMP_CONVERSION_MS = 750UL;
const unsigned long TEMP_REFRESH_MS = 2000UL;
const unsigned long MQ2_REFRESH_MS = 500UL;
const unsigned long SERIAL_REFRESH_MS = 1000UL;
const unsigned long LED_REFRESH_MS = 500UL;
const unsigned long AP_RETRY_INTERVAL_MS = 10000UL;

// Delai maximal d'acquisition de la calibration
const unsigned long CALIBRATION_TIMEOUT_MS = 60000UL;

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
  int smokeThreshold;
  bool conditioned;
};

const uint32_t EEPROM_MAGIC = 0xF0A01235;
const int EEPROM_SIZE = 128;

PersistentData persistentData;

// =====================================================
// VARIABLES CAPTEURS
// =====================================================

float temperatureC = NAN;
bool temperatureOK = false;

int mq2Raw = 0;
int smokeThreshold = 0;

float smokeVariation = 0.0;
bool smokeAlert = false;
bool mq2SensorFault = false;

// Moyenne obtenue pendant la calibration
int calibrationAverage = 0;

// =====================================================
// VARIABLES TEMPORELLES
// =====================================================

unsigned long lastTempRequest = 0;
unsigned long lastTempRefresh = 0;
bool tempConversionPending = false;

unsigned long lastMQ2Refresh = 0;
unsigned long lastSerialRefresh = 0;

unsigned long lastPreheatUpdate = 0;
unsigned long lastEEPROMsave = 0;

unsigned long stabilizationStart = 0;
unsigned long calibrationStart = 0;
unsigned long lastCalibrationSample = 0;

unsigned long lastLEDUpdate = 0;
unsigned long lastAPRetry = 0;

// =====================================================
// CONDITIONNEMENT INITIAL
// =====================================================

uint64_t preheatElapsed = 0;
bool preheatCompleted = false;

// =====================================================
// CALIBRATION
// =====================================================

int calibrationCount = 0;
int calibrationAttempts = 0;
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
  persistentData.smokeThreshold = smokeThreshold;
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
    smokeThreshold = 0;
    preheatCompleted = false;

    saveEEPROM();

    return;
  }

  preheatElapsed = persistentData.preheatElapsed;
  smokeThreshold = persistentData.smokeThreshold;
  preheatCompleted = persistentData.conditioned;

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {
    preheatElapsed = INITIAL_PREHEAT_MS;
    preheatCompleted = true;
  }

  if (!preheatCompleted) {

    systemState = INITIAL_PREHEAT;

  } else if (smokeThreshold > 0) {

    systemState = MONITORING;

  } else {

    systemState = WAIT_CALIBRATION;

  }

}

// =====================================================
// TEMPERATURE DS18B20
// =====================================================

void updateTemperature() {

  unsigned long now = millis();

  // Demande de conversion sans bloquer le programme
  if (!tempConversionPending &&
      now - lastTempRefresh >= TEMP_REFRESH_MS) {

    ds18b20.requestTemperatures();

    lastTempRequest = now;
    lastTempRefresh = now;
    tempConversionPending = true;

  }

  // Lecture une fois la conversion terminee
  if (tempConversionPending &&
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

    tempConversionPending = false;

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

  // Ecart par rapport au seuil minimal
  if (smokeThreshold > 0) {

    smokeVariation =
        ((float)(mq2Raw - smokeThreshold) /
         (float)smokeThreshold) * 100.0;

  } else {

    smokeVariation = 0.0;

  }

  // Nouvelle logique :
  // alerte lorsque la fumee passe sous le seuil minimal
  smokeAlert =
      (systemState == MONITORING &&
       !mq2SensorFault &&
       smokeThreshold > 0 &&
       mq2Raw < smokeThreshold);

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

  // Sauvegarde periodique de la progression
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
    Serial.println(">>> Demarrer le fumoir avec une fumee satisfaisante");
    Serial.println(">>> Puis lancer la calibration");

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
  calibrationAttempts = 0;
  calibrationSum = 0;

  smokeAlert = false;

  stabilizationStart = millis();

  systemState = STABILIZATION;

  Serial.println();
  Serial.println(">>> DEBUT DE LA CALIBRATION");
  Serial.println(">>> Verifier que la fumee est suffisante");
  Serial.println(">>> Stabilisation : 5 minutes");

}

// =====================================================
// CALIBRATION
// =====================================================

void updateCalibration() {

  unsigned long now = millis();

  // Phase 1 : stabilisation
  if (systemState == STABILIZATION) {

    if (now - stabilizationStart >= STABILIZATION_MS) {

      systemState = CALIBRATION;

      calibrationCount = 0;
      calibrationAttempts = 0;
      calibrationSum = 0;

      lastCalibrationSample = 0;
      calibrationStart = now;

      Serial.println();
      Serial.println(">>> STABILISATION TERMINEE");
      Serial.println(">>> Acquisition des 100 mesures");

    }

    return;
  }

  // Phase 2 : acquisition
  if (systemState != CALIBRATION) {
    return;
  }

  // Securite : abandon si les mesures sont invalides
  if (now - calibrationStart >= CALIBRATION_TIMEOUT_MS) {

    systemState = WAIT_CALIBRATION;

    Serial.println();
    Serial.println(">>> ERREUR : calibration abandonnee");
    Serial.println(">>> Verifier le capteur MQ-2");

    return;
  }

  if (now - lastCalibrationSample <
      CALIBRATION_INTERVAL_MS) {
    return;
  }

  lastCalibrationSample = now;
  calibrationAttempts++;

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
    Serial.print(CALIBRATION_SAMPLES);
    Serial.print(" - derniere mesure : ");
    Serial.println(value);

  }

  // Acquisition terminee
  if (calibrationCount >= CALIBRATION_SAMPLES) {

    calibrationAverage =
        calibrationSum / CALIBRATION_SAMPLES;

    // Calcul du seuil minimal
    smokeThreshold =
        (int)(calibrationAverage *
        (1.0 - MIN_SMOKE_MARGIN_PERCENT / 100.0));

    if (smokeThreshold > 0) {

      systemState = MONITORING;

      smokeAlert = false;

      saveEEPROM();

      Serial.println();
      Serial.println(">>> CALIBRATION TERMINEE");
      Serial.print("Moyenne mesuree       : ");
      Serial.println(calibrationAverage);

      Serial.print("Marge minimale        : ");
      Serial.print(MIN_SMOKE_MARGIN_PERCENT);
      Serial.println(" %");

      Serial.print("Seuil minimal de fumee: ");
      Serial.println(smokeThreshold);

      Serial.println(">>> SURVEILLANCE ACTIVE");

    } else {

      smokeThreshold = 0;
      systemState = WAIT_CALIBRATION;

      Serial.println(">>> ERREUR : seuil de calibration invalide");

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
      smokeThreshold <= 0) {
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

  // LED systeme :
  // fixe si le systeme est operationnel
  // clignotante dans les autres cas
  if (systemOperational) {

    digitalWrite(LED_SYSTEM_STATUS, LED_ON);

  } else {

    if (now - lastLEDUpdate >= LED_REFRESH_MS) {

      lastLEDUpdate = now;
      systemLedState = !systemLedState;

    }

    digitalWrite(
      LED_SYSTEM_STATUS,
      systemLedState ? LED_ON : LED_OFF
    );

  }

  // LED calibration
  bool calibrationActive =
      (systemState == INITIAL_PREHEAT ||
       systemState == STABILIZATION ||
       systemState == CALIBRATION);

  digitalWrite(
    LED_CALIBRATION_STATUS,
    calibrationActive ? LED_ON : LED_OFF
  );

  // LED alerte fumee insuffisante
  digitalWrite(
    LED_SMOKE_ALERT,
    smokeAlert ? LED_ON : LED_OFF
  );

  // LED defaut capteur
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
      return "Calibration requise";

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
  Serial.println("             ETAT DU FUMOIR");
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

  Serial.print("Moyenne calibration: ");

  if (calibrationAverage > 0) {
    Serial.println(calibrationAverage);
  } else {
    Serial.println("NON DISPONIBLE");
  }

  Serial.print("Seuil minimal      : ");

  if (smokeThreshold > 0) {
    Serial.println(smokeThreshold);
  } else {
    Serial.println("NON DEFINI");
  }

  Serial.print("Ecart au seuil     : ");
  Serial.print(smokeVariation, 2);
  Serial.println(" %");

  Serial.print("Fumee suffisante   : ");

  if (systemState != MONITORING) {
    Serial.println("SURVEILLANCE INACTIVE");
  } else {
    Serial.println(smokeAlert ? "NON" : "OUI");
  }

  Serial.print("Defaut MQ-2        : ");
  Serial.println(mq2SensorFault ? "OUI" : "NON");

  Serial.println();
  Serial.println("--- CAPTEUR DS18B20 ---");

  Serial.print("Temperature        : ");

  if (temperatureOK) {
    Serial.print(temperatureC, 2);
    Serial.println(" C");
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
<title>Fumoir froid</title>

<style>
body {
  font-family: Arial, sans-serif;
  background: #20242a;
  color: white;
  text-align: center;
  margin: 0;
  padding: 20px;
}

h1 {
  margin-bottom: 20px;
}

.card {
  background: #303740;
  border-radius: 10px;
  padding: 16px;
  margin: 12px auto;
  max-width: 440px;
}

.value {
  font-size: 28px;
  font-weight: bold;
  margin: 10px;
}

.label {
  color: #b8c2cc;
  font-size: 13px;
}

.status {
  font-weight: bold;
  margin: 10px;
}

.ok {
  color: #2ecc71;
}

.warning {
  color: #f1c40f;
}

.alert {
  color: #ff5555;
}

.progress {
  background: #59616b;
  height: 10px;
  border-radius: 10px;
  overflow: hidden;
  margin: 12px 0;
}

.progress-bar {
  background: #e5a33b;
  height: 10px;
  width: 0%;
}

button {
  padding: 14px 20px;
  font-size: 16px;
  border: 0;
  border-radius: 7px;
  background: #16a085;
  color: white;
  cursor: pointer;
  width: 100%;
}

button:disabled {
  background: #555;
  cursor: not-allowed;
}

.small {
  color: #b8c2cc;
  font-size: 12px;
}
</style>
</head>

<body>

<h1>FUMOIR FROID</h1>

<div class="card">
  <div class="label">ETAT DU SYSTEME</div>
  <div class="value" id="state">...</div>
  <div id="wifi" class="small">Hotspot : ...</div>
</div>

<div class="card">
  <div class="label">TEMPERATURE</div>
  <div class="value" id="temperature">-- °C</div>
  <div id="temperatureStatus"></div>
</div>

<div class="card">
  <div class="label">NIVEAU DE FUMEE - MQ-2</div>
  <div class="value" id="mq2">---</div>

  <p>Seuil minimal : <b id="threshold">---</b></p>
  <p>Ecart au seuil : <b id="variation">---</b> %</p>

  <div id="smokeStatus" class="status">---</div>
</div>

<div class="card">
  <div class="label">CONDITIONNEMENT INITIAL</div>
  <div class="value" id="remaining">---</div>

  <div class="progress">
    <div class="progress-bar" id="progress"></div>
  </div>

  <div class="small" id="elapsed">---</div>
</div>

<div class="card">
  <div class="label">CALIBRATION DU SEUIL MINIMAL</div>

  <p id="calibrationInfo">
    Demarrer le fumoir et attendre une fumee satisfaisante.
  </p>

  <button id="calibrate" onclick="calibrate()">
    Definir le seuil minimal
  </button>

  <p class="small">
    La calibration comprend 5 minutes de stabilisation
    puis 100 mesures.
  </p>
</div>

<div class="card">
  <div class="label">ETAT DES CAPTEURS</div>
  <p>DS18B20 : <b id="dsStatus">---</b></p>
  <p>MQ-2 : <b id="mqStatus">---</b></p>
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
      d.temperatureOK
        ? d.temperature.toFixed(2) + ' °C'
        : 'ERREUR';

    document.getElementById('temperatureStatus').textContent =
      d.temperatureOK ? 'Capteur OK' : 'Capteur non detecte';

    document.getElementById('mq2').textContent = d.mq2;

    document.getElementById('threshold').textContent =
      d.threshold > 0 ? d.threshold : 'Non defini';

    document.getElementById('variation').textContent =
      d.variation.toFixed(2);

    let smokeStatus = document.getElementById('smokeStatus');

    if (d.stateCode !== 4) {

      smokeStatus.textContent = 'Surveillance inactive';
      smokeStatus.className = 'status warning';

    } else if (d.smokeAlert) {

      smokeStatus.textContent = 'MANQUE DE FUMEE';
      smokeStatus.className = 'status alert';

    } else {

      smokeStatus.textContent = 'FUMEE SUFFISANTE';
      smokeStatus.className = 'status ok';

    }

    document.getElementById('remaining').textContent =
      formatDuration(d.preheatRemaining);

    document.getElementById('elapsed').textContent =
      'Ecoule : ' + formatDuration(d.preheatElapsed);

    let percent =
      Math.min(100, d.preheatElapsed / d.preheatTotal * 100);

    document.getElementById('progress').style.width =
      percent + '%';

    document.getElementById('dsStatus').textContent =
      d.temperatureOK ? 'OK' : 'DEFAUT';

    document.getElementById('mqStatus').textContent =
      d.mqFault ? 'DEFAUT' : 'OK';

    const canCalibrate =
      d.stateCode === 1 || d.stateCode === 4;

    document.getElementById('calibrate').disabled =
      !canCalibrate;

    let info = '';

    if (d.stateCode === 0) {
      info = 'Conditionnement initial en cours';
    } else if (d.stateCode === 1) {
      info = 'Pret : demarrer le fumoir avec une fumee satisfaisante';
    } else if (d.stateCode === 2) {
      info = 'Stabilisation du capteur en cours (5 minutes)';
    } else if (d.stateCode === 3) {
      info = 'Acquisition : ' + d.calibrationCount + '/100 mesures';
    } else {
      info = 'Seuil minimal enregistre : ' + d.threshold;
    }

    document.getElementById('calibrationInfo').textContent = info;

  } catch (e) {

    document.getElementById('state').textContent =
      'Connexion perdue';

  }

}

async function calibrate() {

  const confirmation = confirm(
    'Le fumoir produit-il actuellement une quantite de fumee satisfaisante ?'
  );

  if (!confirmation) {
    return;
  }

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

  json += "\"hotspot\":";
  json += hotspotOK ? "true," : "false,";

  json += "\"temperatureOK\":";
  json += temperatureOK ? "true," : "false,";

  json += "\"temperature\":";
  json += String(temperatureOK ? temperatureC : 0.0, 2) + ",";

  json += "\"mq2\":" + String(mq2Raw) + ",";
  json += "\"threshold\":" + String(smokeThreshold) + ",";
  json += "\"variation\":" + String(smokeVariation, 2) + ",";

  json += "\"smokeAlert\":";
  json += smokeAlert ? "true," : "false,";

  json += "\"mqFault\":";
  json += mq2SensorFault ? "true," : "false,";

  json += "\"calibrationCount\":" + String(calibrationCount) + ",";

  json += "\"preheatElapsed\":";
  json += String((unsigned long)preheatElapsed) + ",";

  json += "\"preheatRemaining\":";
  json += String((unsigned long)remaining) + ",";

  json += "\"preheatTotal\":";
  json += String((unsigned long)INITIAL_PREHEAT_MS);

  json += "}";

  server.send(200, "application/json", json);

}

// =====================================================
// ROUTE CALIBRATION
// =====================================================

void handleCalibrate() {

  if (systemState == WAIT_CALIBRATION ||
      systemState == MONITORING) {

    startCalibration();

    server.send(200, "text/plain", "Calibration demarree");

  } else {

    server.send(409, "text/plain", "Calibration impossible");

  }

}

// =====================================================
// SETUP
// =====================================================

void setup() {

  Serial.begin(115200);
  delay(200);

  Serial.println();
  Serial.println("========================================");
  Serial.println("          DEMARRAGE DU FUMOIR");
  Serial.println("========================================");

  // Initialisation des LED
  pinMode(LED_SYSTEM_STATUS, OUTPUT);
  pinMode(LED_CALIBRATION_STATUS, OUTPUT);
  pinMode(LED_SMOKE_ALERT, OUTPUT);
  pinMode(LED_SENSOR_FAULT, OUTPUT);

  digitalWrite(LED_SYSTEM_STATUS, LED_OFF);
  digitalWrite(LED_CALIBRATION_STATUS, LED_OFF);
  digitalWrite(LED_SMOKE_ALERT, LED_OFF);
  digitalWrite(LED_SENSOR_FAULT, LED_OFF);

  // Initialisation EEPROM
  EEPROM.begin(EEPROM_SIZE);
  loadEEPROM();

  // Initialisation DS18B20
  ds18b20.begin();
  ds18b20.setResolution(12);
  ds18b20.setWaitForConversion(false);

  // Demarrage hotspot
  WiFi.mode(WIFI_AP);
  hotspotOK = WiFi.softAP(AP_SSID, AP_PASSWORD);

  // Routes serveur web
  server.on("/", HTTP_GET, []() {
    server.send_P(200, "text/html", MAIN_PAGE);
  });

  server.on("/data", HTTP_GET, handleData);
  server.on("/calibrate", HTTP_POST, handleCalibrate);

  server.begin();

  // Initialisation des temporisations
  lastPreheatUpdate = millis();
  lastEEPROMsave = millis();

  lastTempRefresh = millis() - TEMP_REFRESH_MS;
  lastMQ2Refresh = 0;

  Serial.print("Hotspot : ");
  Serial.println(hotspotOK ? "ACTIF" : "ERREUR");

  Serial.print("Adresse IP : ");
  Serial.println(WiFi.softAPIP());

  Serial.print("SSID : ");
  Serial.println(AP_SSID);

  Serial.print("Etat initial : ");
  Serial.println(getStateName());

  if (smokeThreshold > 0) {
    Serial.print("Seuil minimal sauvegarde : ");
    Serial.println(smokeThreshold);
  }

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
