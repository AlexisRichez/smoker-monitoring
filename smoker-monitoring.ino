
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <math.h>

// ============================================================
// 1. CONFIGURATION WIFI
// ============================================================

// Hotspot permanent de configuration
const char* AP_SSID = "HOTSPOT_SMOKER";
const char* AP_PASSWORD = "Smoker2026";

// Adresse mDNS
const char* MDNS_HOSTNAME = "smoker";

// Temporisations Wi-Fi
const unsigned long WIFI_CONNECT_TIMEOUT_MS = 20000UL;
const unsigned long WIFI_RETRY_INTERVAL_MS = 15000UL;

// ============================================================
// 2. BROCHAGE NODEMCU ESP8266
// ============================================================

#define PIN_DS18B20       D0
#define PIN_LED_SYSTEM    D1
#define PIN_LED_CALIB     D2
#define PIN_LED_SMOKE     D5
#define PIN_LED_FAULT     D6
#define PIN_MQ2           A0

#define LED_ON  HIGH
#define LED_OFF LOW

// ============================================================
// 3. OBJETS
// ============================================================

ESP8266WebServer server(80);

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

// ============================================================
// 4. PARAMETRES DU SYSTEME
// ============================================================

const uint64_t INITIAL_PREHEAT_MS =
    24ULL * 60ULL * 60ULL * 1000ULL;

const unsigned long CALIBRATION_STABILIZATION_MS = 300000UL;

const int CALIBRATION_SAMPLES = 100;
const unsigned long CALIBRATION_INTERVAL_MS = 200UL;
const unsigned long CALIBRATION_TIMEOUT_MS = 60000UL;

const float MIN_SMOKE_MARGIN_PERCENT = 15.0;

const int MQ2_ADC_MIN = 5;
const int MQ2_ADC_MAX = 1018;

const unsigned long EEPROM_SAVE_INTERVAL_MS = 300000UL;

const unsigned long DS18B20_INTERVAL_MS = 2000UL;
const unsigned long DS18B20_CONVERSION_MS = 750UL;
const unsigned long MQ2_INTERVAL_MS = 500UL;

const unsigned long SERIAL_INTERVAL_MS = 1000UL;
const unsigned long LED_BLINK_INTERVAL_MS = 500UL;

// ============================================================
// 5. EEPROM
// ============================================================

const uint32_t EEPROM_MAGIC = 0xF0A01235;
const uint32_t WIFI_MAGIC = 0x57494649;

const int EEPROM_SIZE = 512;
const int WIFI_CONFIG_OFFSET = 64;

struct PersistentData {
  uint32_t magic;
  uint64_t preheatElapsed;
  int32_t smokeThreshold;
  uint8_t conditioned;
  uint8_t reserved[3];
};

struct WiFiConfig {
  uint32_t magic;
  char ssid[33];
  char password[65];
};

PersistentData persistentData;
WiFiConfig wifiConfig;

// ============================================================
// 6. ETATS DU SYSTEME
// ============================================================

enum SystemState {
  INITIAL_PREHEAT,
  WAIT_CALIBRATION,
  STABILIZATION,
  CALIBRATION,
  MONITORING
};

SystemState systemState = INITIAL_PREHEAT;

// ============================================================
// 7. VARIABLES
// ============================================================

// Température
float temperature = NAN;
bool temperatureOK = false;

// MQ-2
int mq2Raw = 0;
int smokeThreshold = 0;
float smokeVariation = 0.0;
bool mq2SensorFault = false;

// Fumée
bool smokeAlert = false;

// Calibration
int calibrationCount = 0;
long calibrationSum = 0;
float calibrationAverage = 0.0;

unsigned long calibrationStartTime = 0;
unsigned long lastCalibrationSample = 0;

bool previousCalibrationValid = false;

// Conditionnement
uint64_t preheatElapsed = 0;
unsigned long lastPreheatUpdate = 0;
unsigned long lastEEPROMSave = 0;

// DS18B20
bool dsConversionPending = false;
unsigned long lastTemperatureRequest = 0;
unsigned long temperatureRequestTime = 0;

// Temporisations
unsigned long lastMQ2Read = 0;
unsigned long lastSerialPrint = 0;

// Wi-Fi
bool wifiConnected = false;
bool fallbackAPActive = false;
bool mdnsStarted = false;

bool wifiConnectionAttempted = false;

unsigned long wifiConnectStart = 0;
unsigned long lastWiFiRetry = 0;

// ============================================================
// 8. FONCTIONS GENERALES
// ============================================================

const char* getStateName() {

  switch (systemState) {

    case INITIAL_PREHEAT:
      return "Conditionnement initial";

    case WAIT_CALIBRATION:
      return "Calibration necessaire";

    case STABILIZATION:
      return "Stabilisation";

    case CALIBRATION:
      return "Calibration en cours";

    case MONITORING:
      return "Surveillance active";
  }

  return "Etat inconnu";
}

int getStateCode() {
  return (int)systemState;
}

bool isCalibrationPossible() {

  return (
    systemState == WAIT_CALIBRATION ||
    systemState == MONITORING
  );
}

bool networkAvailable() {
  return wifiConnected || fallbackAPActive;
}

bool hasSensorFault() {
  return (!temperatureOK || mq2SensorFault);
}

uint64_t getPreheatRemaining() {

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {
    return 0;
  }

  return INITIAL_PREHEAT_MS - preheatElapsed;
}

// ============================================================
// 9. EEPROM : DONNEES DU SYSTEME
// ============================================================

void saveEEPROM() {

  persistentData.magic = EEPROM_MAGIC;
  persistentData.preheatElapsed = preheatElapsed;
  persistentData.smokeThreshold = smokeThreshold;

  persistentData.conditioned =
    (preheatElapsed >= INITIAL_PREHEAT_MS) ? 1 : 0;

  EEPROM.put(0, persistentData);
  EEPROM.commit();

  lastEEPROMSave = millis();

  Serial.println("[EEPROM] Donnees systeme sauvegardees");
}

void loadEEPROM() {

  EEPROM.begin(EEPROM_SIZE);

  EEPROM.get(0, persistentData);

  if (persistentData.magic != EEPROM_MAGIC) {

    Serial.println("[EEPROM] Donnees systeme absentes");

    preheatElapsed = 0;
    smokeThreshold = 0;

    systemState = INITIAL_PREHEAT;

    saveEEPROM();

    return;
  }

  preheatElapsed = persistentData.preheatElapsed;
  smokeThreshold = persistentData.smokeThreshold;

  if (preheatElapsed > INITIAL_PREHEAT_MS) {
    preheatElapsed = INITIAL_PREHEAT_MS;
  }

  Serial.println("[EEPROM] Donnees systeme restaurees");

  Serial.print("[EEPROM] Conditionnement : ");
  Serial.print((unsigned long)(preheatElapsed / 3600000ULL));
  Serial.println(" heures");

  Serial.print("[EEPROM] Seuil minimum : ");
  Serial.println(smokeThreshold);

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {

    if (smokeThreshold > 0) {
      systemState = MONITORING;
    } else {
      systemState = WAIT_CALIBRATION;
    }

  } else {

    systemState = INITIAL_PREHEAT;
  }
}

// ============================================================
// 10. EEPROM : CONFIGURATION WIFI
// ============================================================

bool loadWiFiConfig() {

  EEPROM.get(WIFI_CONFIG_OFFSET, wifiConfig);

  if (wifiConfig.magic != WIFI_MAGIC) {

    memset(&wifiConfig, 0, sizeof(wifiConfig));

    Serial.println("[WIFI] Aucun identifiant Wi-Fi en memoire");

    return false;
  }

  wifiConfig.ssid[32] = '\0';
  wifiConfig.password[64] = '\0';

  if (strlen(wifiConfig.ssid) == 0) {
    return false;
  }

  Serial.println("[WIFI] Configuration Wi-Fi restauree");

  Serial.print("[WIFI] SSID memorise : ");
  Serial.println(wifiConfig.ssid);

  return true;
}

bool saveWiFiConfig(const String& ssid, const String& password) {

  if (ssid.length() == 0 || ssid.length() > 32) {
    return false;
  }

  if (password.length() > 63) {
    return false;
  }

  memset(&wifiConfig, 0, sizeof(wifiConfig));

  wifiConfig.magic = WIFI_MAGIC;

  ssid.toCharArray(wifiConfig.ssid, sizeof(wifiConfig.ssid));
  password.toCharArray(wifiConfig.password, sizeof(wifiConfig.password));

  EEPROM.put(WIFI_CONFIG_OFFSET, wifiConfig);

  if (!EEPROM.commit()) {

    Serial.println("[EEPROM] Erreur sauvegarde Wi-Fi");

    return false;
  }

  Serial.println("[EEPROM] Identifiants Wi-Fi sauvegardes");

  return true;
}

// ============================================================
// 11. WIFI : HOTSPOT PERMANENT
// ============================================================

void startFallbackAP() {

  if (fallbackAPActive) {
    return;
  }

  Serial.println();
  Serial.println("[WIFI] Activation du hotspot permanent");

  WiFi.mode(WIFI_AP_STA);

  bool result = WiFi.softAP(
    AP_SSID,
    AP_PASSWORD
  );

  fallbackAPActive = result;

  if (result) {

    Serial.println("[WIFI] Hotspot actif");

    Serial.print("[WIFI] SSID : ");
    Serial.println(AP_SSID);

    Serial.print("[WIFI] IP : ");
    Serial.println(WiFi.softAPIP());

  } else {

    Serial.println("[WIFI] Echec activation hotspot");
  }
}

// ============================================================
// 12. WIFI : CONNEXION AU RESEAU DOMESTIQUE
// ============================================================

void connectToHomeWiFi() {

  if (strlen(wifiConfig.ssid) == 0) {

    Serial.println("[WIFI] Aucun reseau configure");

    wifiConnectionAttempted = false;

    return;
  }

  Serial.println();
  Serial.println("[WIFI] Tentative de connexion au reseau domestique");

  Serial.print("[WIFI] SSID : ");
  Serial.println(wifiConfig.ssid);

  WiFi.mode(WIFI_AP_STA);
  WiFi.hostname(MDNS_HOSTNAME);
  WiFi.setAutoReconnect(true);

  WiFi.begin(
    wifiConfig.ssid,
    wifiConfig.password
  );

  wifiConnectionAttempted = true;

  wifiConnectStart = millis();
  lastWiFiRetry = millis();
}

void updateWiFi() {

  unsigned long now = millis();

  bool connected = (WiFi.status() == WL_CONNECTED);

  // ----------------------------------------------------------
  // CONNEXION REUSSIE
  // ----------------------------------------------------------

  if (connected) {

    if (!wifiConnected) {

      wifiConnected = true;

      Serial.println();
      Serial.println("[WIFI] Connexion reussie");

      Serial.print("[WIFI] Adresse IP : ");
      Serial.println(WiFi.localIP());

      if (!mdnsStarted) {

        mdnsStarted = MDNS.begin(MDNS_HOSTNAME);

        if (mdnsStarted) {

          MDNS.addService("http", "tcp", 80);

          Serial.print("[MDNS] Adresse : http://");
          Serial.print(MDNS_HOSTNAME);
          Serial.println(".local");
        }
      }
    }

    return;
  }

  // ----------------------------------------------------------
  // PERTE DE CONNEXION
  // ----------------------------------------------------------

  if (wifiConnected) {

    Serial.println();
    Serial.println("[WIFI] Connexion perdue");

    wifiConnected = false;

    if (mdnsStarted) {
      MDNS.end();
      mdnsStarted = false;
    }

    lastWiFiRetry = now;

    return;
  }

  // ----------------------------------------------------------
  // PAS DE CONFIGURATION
  // ----------------------------------------------------------

  if (strlen(wifiConfig.ssid) == 0) {
    return;
  }

  // ----------------------------------------------------------
  // TIMEOUT DE CONNEXION
  // ----------------------------------------------------------

  if (wifiConnectionAttempted &&
      now - wifiConnectStart >= WIFI_CONNECT_TIMEOUT_MS) {

    Serial.println("[WIFI] Delai de connexion depasse");

    wifiConnectionAttempted = false;
    lastWiFiRetry = now;

    return;
  }

  // ----------------------------------------------------------
  // NOUVELLE TENTATIVE
  // ----------------------------------------------------------

  if (!wifiConnectionAttempted &&
      now - lastWiFiRetry >= WIFI_RETRY_INTERVAL_MS) {

    Serial.println("[WIFI] Nouvelle tentative de connexion");

    connectToHomeWiFi();
  }
}

// ============================================================
// 13. DS18B20
// ============================================================

void updateTemperature() {

  unsigned long now = millis();

  if (dsConversionPending &&
      now - temperatureRequestTime >= DS18B20_CONVERSION_MS) {

    float value = ds18b20.getTempCByIndex(0);

    if (value != DEVICE_DISCONNECTED_C &&
        value > -55.0 &&
        value < 125.0 &&
        value != 85.0) {

      temperature = value;
      temperatureOK = true;

    } else {

      temperatureOK = false;
    }

    dsConversionPending = false;
  }

  if (!dsConversionPending &&
      now - lastTemperatureRequest >= DS18B20_INTERVAL_MS) {

    ds18b20.requestTemperatures();

    lastTemperatureRequest = now;
    temperatureRequestTime = now;

    dsConversionPending = true;
  }
}

// ============================================================
// 14. MQ-2
// ============================================================

void updateMQ2() {

  unsigned long now = millis();

  if (now - lastMQ2Read < MQ2_INTERVAL_MS) {
    return;
  }

  lastMQ2Read = now;

  mq2Raw = analogRead(PIN_MQ2);

  mq2SensorFault =
    (mq2Raw <= MQ2_ADC_MIN ||
     mq2Raw >= MQ2_ADC_MAX);

  if (smokeThreshold > 0) {

    smokeVariation =
      ((float)(mq2Raw - smokeThreshold) /
       (float)smokeThreshold) * 100.0;

  } else {

    smokeVariation = 0.0;
  }

  smokeAlert =
    (systemState == MONITORING &&
     !mq2SensorFault &&
     smokeThreshold > 0 &&
     mq2Raw < smokeThreshold);
}

// ============================================================
// 15. CONDITIONNEMENT INITIAL
// ============================================================

void updatePreheat() {

  if (systemState != INITIAL_PREHEAT) {
    return;
  }

  unsigned long now = millis();

  unsigned long elapsed = now - lastPreheatUpdate;

  lastPreheatUpdate = now;

  preheatElapsed += elapsed;

  if (preheatElapsed >= INITIAL_PREHEAT_MS) {

    preheatElapsed = INITIAL_PREHEAT_MS;

    systemState = WAIT_CALIBRATION;

    Serial.println();
    Serial.println("================================");
    Serial.println("[SYSTEME] CONDITIONNEMENT TERMINE");
    Serial.println("[SYSTEME] Calibration necessaire");
    Serial.println("================================");

    saveEEPROM();

    return;
  }

  if (now - lastEEPROMSave >= EEPROM_SAVE_INTERVAL_MS) {
    saveEEPROM();
  }
}

// ============================================================
// 16. CALIBRATION
// ============================================================

void startCalibration() {

  if (!isCalibrationPossible()) {

    Serial.println("[CALIBRATION] Impossible dans cet etat");

    return;
  }

  previousCalibrationValid =
    (systemState == MONITORING && smokeThreshold > 0);

  calibrationCount = 0;
  calibrationSum = 0;
  calibrationAverage = 0;

  calibrationStartTime = millis();
  lastCalibrationSample = 0;

  smokeAlert = false;

  systemState = STABILIZATION;

  Serial.println();
  Serial.println("================================");
  Serial.println("[CALIBRATION] Demarrage");
  Serial.println("[CALIBRATION] Stabilisation 5 minutes");
  Serial.println("[CALIBRATION] Fumee satisfaisante requise");
  Serial.println("================================");
}

void abortCalibration(const char* reason) {

  Serial.print("[CALIBRATION] Echec : ");
  Serial.println(reason);

  if (previousCalibrationValid && smokeThreshold > 0) {

    systemState = MONITORING;

    Serial.println("[CALIBRATION] Ancien seuil conserve");
    Serial.println("[CALIBRATION] Reprise de la surveillance");

  } else {

    systemState = WAIT_CALIBRATION;
  }
}

void updateCalibration() {

  unsigned long now = millis();

  if (systemState == STABILIZATION) {

    if (now - calibrationStartTime >=
        CALIBRATION_STABILIZATION_MS) {

      systemState = CALIBRATION;

      calibrationStartTime = now;
      lastCalibrationSample = 0;

      Serial.println("[CALIBRATION] Stabilisation terminee");
      Serial.println("[CALIBRATION] Acquisition des mesures");
    }

    return;
  }

  if (systemState != CALIBRATION) {
    return;
  }

  if (now - calibrationStartTime >
      CALIBRATION_TIMEOUT_MS) {

    abortCalibration("Timeout");

    return;
  }

  if (now - lastCalibrationSample <
      CALIBRATION_INTERVAL_MS) {
    return;
  }

  lastCalibrationSample = now;

  int sample = analogRead(PIN_MQ2);

  if (sample <= MQ2_ADC_MIN ||
      sample >= MQ2_ADC_MAX) {

    Serial.println("[CALIBRATION] Mesure invalide");

    return;
  }

  calibrationSum += sample;
  calibrationCount++;

  Serial.print("[CALIBRATION] Mesure ");
  Serial.print(calibrationCount);
  Serial.print("/");
  Serial.print(CALIBRATION_SAMPLES);
  Serial.print(" : ");
  Serial.println(sample);

  if (calibrationCount >= CALIBRATION_SAMPLES) {

    calibrationAverage =
      (float)calibrationSum /
      (float)CALIBRATION_SAMPLES;

    smokeThreshold =
      (int)round(
        calibrationAverage *
        (1.0 - MIN_SMOKE_MARGIN_PERCENT / 100.0)
      );

    if (smokeThreshold < 1) {
      smokeThreshold = 1;
    }

    systemState = MONITORING;

    saveEEPROM();

    Serial.println();
    Serial.println("================================");
    Serial.println("[CALIBRATION] TERMINEE");

    Serial.print("[CALIBRATION] Moyenne : ");
    Serial.println(calibrationAverage);

    Serial.print("[CALIBRATION] Seuil minimum : ");
    Serial.println(smokeThreshold);

    Serial.println("[CALIBRATION] Surveillance active");
    Serial.println("================================");
  }
}

// ============================================================
// 17. LEDS
// ============================================================

void updateLEDs() {

  unsigned long now = millis();

  bool blinkState =
    ((now / LED_BLINK_INTERVAL_MS) % 2) == 0;

  bool systemOperational =
    networkAvailable() &&
    systemState == MONITORING &&
    !hasSensorFault();

  digitalWrite(
    PIN_LED_SYSTEM,
    systemOperational ?
      LED_ON :
      (blinkState ? LED_ON : LED_OFF)
  );

  bool calibrationActive =
    (systemState == INITIAL_PREHEAT ||
     systemState == STABILIZATION ||
     systemState == CALIBRATION);

  digitalWrite(
    PIN_LED_CALIB,
    calibrationActive ? LED_ON : LED_OFF
  );

  digitalWrite(
    PIN_LED_SMOKE,
    smokeAlert ? LED_ON : LED_OFF
  );

  digitalWrite(
    PIN_LED_FAULT,
    hasSensorFault() ? LED_ON : LED_OFF
  );
}

// ============================================================
// 18. MONITEUR SERIE
// ============================================================

void printSerialStatus() {

  unsigned long now = millis();

  if (now - lastSerialPrint < SERIAL_INTERVAL_MS) {
    return;
  }

  lastSerialPrint = now;

  Serial.println();
  Serial.println("--------------------------------");

  Serial.print("Etat systeme : ");
  Serial.println(getStateName());

  Serial.print("WiFi maison : ");
  Serial.println(wifiConnected ? "CONNECTE" : "DECONNECTE");

  Serial.print("Hotspot configuration : ");
  Serial.println(fallbackAPActive ? "ACTIF" : "INACTIF");

  Serial.print("Temperature : ");

  if (temperatureOK) {
    Serial.print(temperature, 1);
    Serial.println(" C");
  } else {
    Serial.println("DEFAUT");
  }

  Serial.print("MQ-2 brut : ");
  Serial.println(mq2Raw);

  Serial.print("MQ-2 defaut : ");
  Serial.println(mq2SensorFault ? "OUI" : "NON");

  Serial.print("Seuil minimum fumee : ");
  Serial.println(smokeThreshold);

  Serial.print("Variation seuil : ");
  Serial.print(smokeVariation, 1);
  Serial.println(" %");

  Serial.print("Alerte fumee : ");
  Serial.println(smokeAlert ? "OUI - MANQUE DE FUMEE" : "NON");

  Serial.print("Defaut capteur : ");
  Serial.println(hasSensorFault() ? "OUI" : "NON");

  if (systemState == INITIAL_PREHEAT) {

    Serial.print("Conditionnement : ");

    Serial.print(
      (float)preheatElapsed / 3600000.0,
      2
    );

    Serial.println(" / 24 heures");
  }

  if (systemState == STABILIZATION) {

    unsigned long elapsed =
      now - calibrationStartTime;

    Serial.print("Stabilisation restante : ");

    Serial.print(
      (CALIBRATION_STABILIZATION_MS - elapsed) / 1000
    );

    Serial.println(" secondes");
  }

  if (systemState == CALIBRATION) {

    Serial.print("Calibration : ");
    Serial.print(calibrationCount);
    Serial.print("/");
    Serial.println(CALIBRATION_SAMPLES);
  }

  Serial.println("--------------------------------");
}

// ============================================================
// 19. PAGE PRINCIPALE
// ============================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Fumoir - Surveillance</title>
<style>
body {
  margin:0;
  padding:16px;
  background:#f3f5f8;
  color:#202936;
  font-family:Arial,sans-serif;
}
.container {max-width:650px;margin:auto;}
.card {
  background:white;
  border-radius:14px;
  padding:18px;
  margin-bottom:13px;
  box-shadow:0 2px 8px rgba(0,0,0,.04);
}
.header {
  display:flex;
  justify-content:space-between;
  align-items:center;
  margin-bottom:16px;
}
.logo {font-size:22px;font-weight:800;}
.logo span {color:#e87932;}
.grid {display:grid;grid-template-columns:1fr 1fr;gap:12px;}
.label {font-size:11px;color:#788493;font-weight:bold;margin-bottom:10px;}
.value {font-size:28px;font-weight:800;margin-bottom:8px;}
.muted {font-size:12px;color:#788493;line-height:1.5;}
.tag {
  display:inline-block;padding:5px 9px;border-radius:20px;
  font-size:11px;font-weight:bold;
}
.green {background:#e5f6ed;color:#15965a;}
.red {background:#fdeaea;color:#d94343;}
.orange {background:#fff3d9;color:#d58b19;}
.status {border-left:5px solid #15965a;}
.status.alert {border-left-color:#d94343;}
.status.wait {border-left-color:#d58b19;}
.status-title {font-size:19px;font-weight:800;margin-bottom:7px;}
.progress {height:11px;background:#e9edf1;border-radius:20px;overflow:hidden;margin:12px 0;}
.bar {height:100%;width:0;background:#15965a;}
.bar.red {background:#d94343;}
.bar.orange {background:#d58b19;}
.row {
  display:flex;justify-content:space-between;
  padding:11px 0;border-bottom:1px solid #e6eaf0;font-size:13px;
}
button {
  width:100%;border:0;border-radius:10px;padding:14px;
  background:#253447;color:white;font-weight:bold;margin-top:12px;
}
button:disabled {opacity:.4;}
a {color:#2467a6;}
.hidden {display:none;}
.footer {text-align:center;font-size:11px;color:#788493;padding:10px;}
@media(max-width:390px) {
  body {padding:10px;}
  .card {padding:14px;}
  .value {font-size:24px;}
}
</style>
</head>
<body>
<div class="container">

<div class="header">
  <div class="logo"><span>🔥</span> FUMOIR</div>
  <span id="wifi" class="tag orange">Connexion...</span>
</div>

<div id="mainStatus" class="card status">
  <div id="statusTitle" class="status-title">Initialisation...</div>
  <div id="statusDescription" class="muted">Connexion au système.</div>
</div>

<div class="grid">
  <div class="card">
    <div class="label">🌡 TEMPERATURE</div>
    <div class="value"><span id="temperature">--.-</span> °C</div>
    <span id="temperatureStatus" class="tag orange">En attente</span>
  </div>
  <div class="card">
    <div class="label">☁ NIVEAU DE FUMEE</div>
    <div id="smokeText" class="value" style="font-size:21px">En attente</div>
    <span id="smokeStatus" class="tag orange">En attente</span>
  </div>
</div>

<div class="card">
  <strong>Surveillance de la fumée</strong>
  <div class="row"><span>Valeur MQ-2</span><strong id="mq2">---</strong></div>
  <div class="row"><span>Seuil minimum</span><strong id="threshold">---</strong></div>
  <div class="progress"><div id="smokeBar" class="bar"></div></div>
  <div class="muted">
    <span id="variation">--</span> % par rapport au seuil.
    <span id="smokeConclusion">En attente</span>
  </div>
  <p class="muted">
    Le système vérifie uniquement que le niveau de fumée reste
    supérieur au seuil minimum défini lors de la calibration.
  </p>
</div>

<div id="preheatCard" class="card hidden">
  <strong>Conditionnement initial</strong>
  <p class="muted">Le capteur MQ-2 effectue son conditionnement initial.</p>
  <div class="row">
    <strong id="preheatProgress">0 %</strong>
    <span id="preheatRemaining">Temps restant : --</span>
  </div>
  <div class="progress"><div id="preheatBar" class="bar orange"></div></div>
</div>

<div id="calibrationCard" class="card hidden">
  <strong>Calibration du détecteur</strong>
  <p id="calibrationDescription" class="muted">
    Place le fumoir dans ses conditions normales de fonctionnement,
    avec une fumée satisfaisante.
  </p>
  <div id="calibrationProgressContainer" class="hidden">
    <div class="row">
      <strong id="calibrationProgress">0 / 100</strong>
      <span id="calibrationPercent">0 %</span>
    </div>
    <div class="progress"><div id="calibrationBar" class="bar orange"></div></div>
  </div>
  <button id="calibrationButton" onclick="startCalibration()">
    Lancer la calibration
  </button>
</div>

<div class="card">
  <strong>Etat des équipements</strong>
  <div class="row"><span>Sonde de température</span><span id="dsStatus" class="tag orange">---</span></div>
  <div class="row"><span>Détecteur MQ-2</span><span id="mqStatus" class="tag orange">---</span></div>
  <div class="row"><span>Réseau Wi-Fi</span><span id="wifiStatus" class="tag orange">---</span></div>
</div>

<div class="card">
  <details>
    <summary>⚙ Détails techniques</summary>
    <div class="row"><span>Etat du système</span><strong id="technicalState">---</strong></div>
    <div class="row"><span>Valeur ADC MQ-2</span><strong id="technicalMQ2">---</strong></div>
    <div class="row"><span>Seuil minimum</span><strong id="technicalThreshold">---</strong></div>
    <div class="row"><span>Moyenne calibration</span><strong id="technicalAverage">---</strong></div>
    <div class="row"><span>Mesures calibration</span><strong id="technicalSamples">---</strong></div>
    <div class="row"><span>Mode réseau</span><strong id="technicalNetwork">---</strong></div>
    <div class="row"><span>Adresse IP</span><strong id="technicalIP">---</strong></div>
    <p><a href="/wifi">Configurer le réseau Wi-Fi</a></p>
  </details>
</div>

<div class="card">
  <a href="/wifi">⚙ Modifier les identifiants Wi-Fi</a>
</div>

<div class="footer">
  FUMOIR COLD · Mise à jour automatique
  <br><span id="lastUpdate">Connexion...</span>
</div>

</div>

<script>
function setText(id,text) {
  document.getElementById(id).textContent=text;
}
function setTag(id,text,color) {
  const e=document.getElementById(id);
  e.textContent=text;
  e.className="tag "+color;
}
function formatDuration(ms) {
  let s=Math.floor(ms/1000);
  return Math.floor(s/3600)+" h "+Math.floor((s%3600)/60)+" min";
}

function updateDashboard(d) {

  if(d.wifiConnected) {
    setText("wifi","● Wi-Fi maison");
    setTag("wifiStatus","Connecté","green");
  } else {
    setText("wifi","● Hotspot de configuration");
    setTag("wifiStatus","Hotspot","orange");
  }

  if(d.temperatureOK) {
    setText("temperature",d.temperature.toFixed(1));
    setTag("temperatureStatus","Capteur OK","green");
    setTag("dsStatus","OK","green");
  } else {
    setText("temperature","--.-");
    setTag("temperatureStatus","Défaut","red");
    setTag("dsStatus","Défaut","red");
  }

  setText("mq2",d.mq2);
  setText("threshold",d.threshold>0?d.threshold:"---");

  if(d.mqFault) {
    setText("smokeText","Indisponible");
    setTag("smokeStatus","Défaut capteur","red");
    setTag("mqStatus","Défaut","red");
  } else {
    setTag("mqStatus","OK","green");
    if(d.stateCode===4 && d.threshold>0) {
      if(d.smokeAlert) {
        setText("smokeText","Insuffisante");
        setTag("smokeStatus","Manque de fumée","red");
      } else {
        setText("smokeText","Suffisante");
        setTag("smokeStatus","Niveau correct","green");
      }
    } else {
      setText("smokeText","En attente");
      setTag("smokeStatus","Non surveillée","orange");
    }
  }

  setText("variation",d.variation.toFixed(1));

  let percentage=0;
  if(d.threshold>0) {
    percentage=Math.min(100,Math.max(0,(d.mq2/(d.threshold*1.5))*100));
  }

  const bar=document.getElementById("smokeBar");
  bar.style.width=percentage+"%";

  if(d.smokeAlert) {
    bar.className="bar red";
    setText("smokeConclusion","Sous le seuil");
  } else if(d.stateCode===4 && !d.mqFault) {
    bar.className="bar";
    setText("smokeConclusion","Au-dessus du seuil");
  } else {
    bar.className="bar orange";
    setText("smokeConclusion","En attente");
  }

  const main=document.getElementById("mainStatus");
  main.className="card status";

  let title="",description="";

  if(d.stateCode===0) {
    title="Conditionnement initial";
    description="Le capteur effectue son conditionnement. La surveillance sera disponible à son terme.";
    main.classList.add("wait");
  } else if(d.stateCode===1) {
    title="Calibration nécessaire";
    description="Le conditionnement est terminé. Lance une calibration avec une fumée satisfaisante.";
    main.classList.add("wait");
  } else if(d.stateCode===2) {
    title="Stabilisation du capteur";
    description="Le capteur se stabilise avant l'acquisition des mesures.";
    main.classList.add("wait");
  } else if(d.stateCode===3) {
    title="Calibration en cours";
    description="Acquisition des mesures du capteur MQ-2.";
    main.classList.add("wait");
  } else if(d.stateCode===4) {
    if(d.sensorFault) {
      title="Défaut détecté";
      description="Un capteur nécessite une vérification.";
      main.classList.add("alert");
    } else if(d.smokeAlert) {
      title="Manque de fumée";
      description="Le niveau de fumée est passé sous le seuil minimum.";
      main.classList.add("alert");
    } else {
      title="Surveillance active";
      description="Le système fonctionne normalement. Le niveau de fumée est suffisant.";
    }
  }

  setText("statusTitle",title);
  setText("statusDescription",description);

  const preheat=document.getElementById("preheatCard");
  if(d.stateCode===0) {
    preheat.classList.remove("hidden");
    let p=Math.min(100,(d.preheatElapsed/d.preheatTotal)*100);
    setText("preheatProgress",p.toFixed(1)+" %");
    setText("preheatRemaining","Temps restant : "+formatDuration(d.preheatRemaining));
    document.getElementById("preheatBar").style.width=p+"%";
  } else {
    preheat.classList.add("hidden");
  }

  const cal=document.getElementById("calibrationCard");
  const btn=document.getElementById("calibrationButton");
  const desc=document.getElementById("calibrationDescription");
  const progress=document.getElementById("calibrationProgressContainer");

  if(d.stateCode>=1) cal.classList.remove("hidden");
  else cal.classList.add("hidden");

  if(d.stateCode===1) {
    btn.disabled=false;
    btn.textContent="Lancer la calibration";
    desc.textContent="Place le fumoir dans ses conditions normales de fonctionnement, avec une fumée satisfaisante.";
    progress.classList.add("hidden");
  } else if(d.stateCode===2) {
    btn.disabled=true;
    btn.textContent="Stabilisation en cours...";
    desc.textContent="Le capteur se stabilise pendant 5 minutes avant les mesures.";
    progress.classList.add("hidden");
  } else if(d.stateCode===3) {
    btn.disabled=true;
    btn.textContent="Calibration en cours...";
    desc.textContent="Acquisition automatique des mesures.";
    progress.classList.remove("hidden");
    setText("calibrationProgress",d.calibrationCount+" / 100");
    setText("calibrationPercent",d.calibrationCount+" %");
    document.getElementById("calibrationBar").style.width=d.calibrationCount+"%";
  } else if(d.stateCode===4) {
    btn.disabled=false;
    btn.textContent="Recalibrer le capteur";
    desc.textContent="Tu peux lancer une nouvelle calibration si les conditions de fumage ont changé.";
    progress.classList.add("hidden");
  }

  setText("technicalState",d.state);
  setText("technicalMQ2",d.mq2);
  setText("technicalThreshold",d.threshold);
  setText("technicalAverage",d.calibrationAverage.toFixed(1));
  setText("technicalSamples",d.calibrationCount+" / 100");
  setText("technicalNetwork",d.wifiConnected?"Wi-Fi maison":"Hotspot de configuration");
  setText("technicalIP",d.ip);
  setText("lastUpdate","Dernière actualisation : maintenant");
}

let requestInProgress=false;

async function refreshData() {
  if(requestInProgress) return;
  requestInProgress=true;

  try {
    const response=await fetch("/data",{cache:"no-store"});
    if(!response.ok) throw new Error("Erreur serveur");
    updateDashboard(await response.json());
  } catch(e) {
    setText("statusTitle","Connexion interrompue");
    setText("statusDescription","Impossible de communiquer avec le fumoir.");
    document.getElementById("mainStatus").className="card status alert";
  } finally {
    requestInProgress=false;
  }
}

async function startCalibration() {
  if(!confirm("Le fumoir produit-il actuellement une fumée satisfaisante ?")) return;
  try {
    const response=await fetch("/calibrate",{method:"POST"});
    const result=await response.text();
    if(!response.ok) {
      alert(result);
      return;
    }
    alert(result);
    refreshData();
  } catch(e) {
    alert("Impossible de lancer la calibration.");
  }
}

refreshData();
setInterval(refreshData,1000);
</script>
</body>
</html>
)rawliteral";

// ============================================================
// 20. PAGE DE CONFIGURATION WIFI
// ============================================================

const char WIFI_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Configuration Wi-Fi - Fumoir</title>
<style>
body {
  margin:0;
  padding:20px;
  background:#f3f5f8;
  color:#202936;
  font-family:Arial,sans-serif;
}
.container {max-width:450px;margin:35px auto;}
.card {
  background:white;
  border-radius:16px;
  padding:22px;
  box-shadow:0 2px 10px rgba(0,0,0,.05);
}
h2 {margin-top:0;}
p {font-size:13px;color:#788493;line-height:1.5;}
label {display:block;font-size:13px;font-weight:bold;margin-top:18px;}
input {
  width:100%;
  box-sizing:border-box;
  padding:13px;
  margin-top:7px;
  border:1px solid #dce2e9;
  border-radius:9px;
  font-size:15px;
}
button {
  width:100%;
  padding:14px;
  margin-top:22px;
  border:0;
  border-radius:10px;
  background:#253447;
  color:white;
  font-weight:bold;
  font-size:15px;
}
a {color:#2467a6;font-size:13px;}
.info {
  padding:12px;
  border-radius:9px;
  background:#e5f6ed;
  color:#15965a;
  font-size:12px;
  line-height:1.5;
}
</style>
</head>
<body>
<div class="container">
<div class="card">
  <h2>📶 Configuration Wi-Fi</h2>

  <p>
    Renseigne les identifiants de ton réseau domestique.
    Ils seront enregistrés dans le fumoir et conservés après
    une coupure de courant.
  </p>

  <div class="info">
    Le hotspot de configuration restera actif même après
    la connexion au réseau domestique.
  </div>

  <form method="POST" action="/savewifi">

    <label for="ssid">Nom du réseau Wi-Fi (SSID)</label>
    <input
      id="ssid"
      name="ssid"
      type="text"
      maxlength="32"
      required
      autocomplete="off"
      value="%SSID%"
    >

    <label for="password">Mot de passe Wi-Fi</label>
    <input
      id="password"
      name="password"
      type="password"
      maxlength="63"
      autocomplete="new-password"
      placeholder="Nouveau mot de passe"
    >

    <p>
      Si ton réseau est ouvert, laisse le mot de passe vide.
      Pour conserver le mot de passe actuel, laisse également
      ce champ vide.
    </p>

    <button type="submit">Enregistrer et connecter</button>

  </form>

  <p><a href="/">Retour au tableau de bord</a></p>
</div>
</div>
</body>
</html>
)rawliteral";

// ============================================================
// 21. SERVEUR WEB : TABLEAU DE BORD
// ============================================================

void handleRoot() {

  server.send_P(
    200,
    "text/html; charset=utf-8",
    MAIN_PAGE
  );
}

void handleData() {

  String json = "{";

  json += "\"state\":\"";
  json += getStateName();
  json += "\",";

  json += "\"stateCode\":";
  json += String(getStateCode());
  json += ",";

  json += "\"hotspot\":";
  json += networkAvailable() ? "true" : "false";
  json += ",";

  json += "\"wifiConnected\":";
  json += wifiConnected ? "true" : "false";
  json += ",";

  json += "\"fallbackAP\":";
  json += fallbackAPActive ? "true" : "false";
  json += ",";

  json += "\"ip\":\"";

  if (wifiConnected) {
    json += WiFi.localIP().toString();
  } else if (fallbackAPActive) {
    json += WiFi.softAPIP().toString();
  } else {
    json += "Indisponible";
  }

  json += "\",";

  json += "\"temperatureOK\":";
  json += temperatureOK ? "true" : "false";
  json += ",";

  json += "\"temperature\":";
  json += temperatureOK ? String(temperature, 2) : "0";
  json += ",";

  json += "\"mq2\":";
  json += String(mq2Raw);
  json += ",";

  json += "\"threshold\":";
  json += String(smokeThreshold);
  json += ",";

  json += "\"variation\":";
  json += String(smokeVariation, 2);
  json += ",";

  json += "\"smokeAlert\":";
  json += smokeAlert ? "true" : "false";
  json += ",";

  json += "\"mqFault\":";
  json += mq2SensorFault ? "true" : "false";
  json += ",";

  json += "\"sensorFault\":";
  json += hasSensorFault() ? "true" : "false";
  json += ",";

  json += "\"calibrationCount\":";
  json += String(calibrationCount);
  json += ",";

  json += "\"calibrationAverage\":";
  json += String(calibrationAverage, 2);
  json += ",";

  json += "\"preheatElapsed\":";
  json += String((unsigned long)preheatElapsed);
  json += ",";

  json += "\"preheatRemaining\":";
  json += String((unsigned long)getPreheatRemaining());
  json += ",";

  json += "\"preheatTotal\":";
  json += String((unsigned long)INITIAL_PREHEAT_MS);

  json += "}";

  server.send(200, "application/json", json);
}

// ============================================================
// 22. SERVEUR WEB : CALIBRATION
// ============================================================

void handleCalibration() {

  if (!isCalibrationPossible()) {

    server.send(
      409,
      "text/plain",
      "Calibration impossible dans cet etat."
    );

    return;
  }

  startCalibration();

  server.send(
    200,
    "text/plain",
    "Calibration demarree. Stabilisation de 5 minutes."
  );
}

// ============================================================
// 23. SERVEUR WEB : CONFIGURATION WIFI
// ============================================================

void handleWiFiPage() {

  String page = FPSTR(WIFI_PAGE);

  String currentSSID = wifiConfig.ssid;

  page.replace("%SSID%", currentSSID);

  server.send(
    200,
    "text/html; charset=utf-8",
    page
  );
}

void handleSaveWiFi() {

  if (!server.hasArg("ssid") || !server.hasArg("password")) {

    server.send(
      400,
      "text/plain",
      "Parametres manquants."
    );

    return;
  }

  String newSSID = server.arg("ssid");
  String newPassword = server.arg("password");

  newSSID.trim();

  if (newSSID.length() == 0 || newSSID.length() > 32) {

    server.send(
      400,
      "text/plain",
      "Le nom du reseau doit contenir entre 1 et 32 caracteres."
    );

    return;
  }

  // Mot de passe vide :
  // si un réseau était déjà enregistré, on conserve son mot de passe.
  if (newPassword.length() == 0 &&
      strlen(wifiConfig.ssid) > 0 &&
      newSSID.equals(wifiConfig.ssid)) {

    newPassword = String(wifiConfig.password);
  }

  if (newPassword.length() > 63) {

    server.send(
      400,
      "text/plain",
      "Le mot de passe est trop long."
    );

    return;
  }

  if (!saveWiFiConfig(newSSID, newPassword)) {

    server.send(
      500,
      "text/plain",
      "Erreur lors de la sauvegarde des identifiants."
    );

    return;
  }

  String response = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Wi-Fi enregistré</title>
<style>
body{font-family:Arial;background:#f3f5f8;padding:20px;color:#202936}
.card{max-width:430px;margin:50px auto;background:white;padding:24px;border-radius:14px}
p{color:#788493;line-height:1.5;font-size:14px}
</style>
</head>
<body>
<div class="card">
<h2>Identifiants enregistrés</h2>
<p>Le fumoir va maintenant tenter de se connecter au réseau domestique.</p>
<p>Tu peux consulter le tableau de bord pendant cette opération.</p>
<p><a href="/">Retour au tableau de bord</a></p>
</div>
</body>
</html>
)rawliteral";

  server.send(
    200,
    "text/html; charset=utf-8",
    response
  );

  Serial.println();
  Serial.println("[WIFI] Nouvelle configuration recue");

  Serial.print("[WIFI] Nouveau SSID : ");
  Serial.println(wifiConfig.ssid);

  // Laisse le temps au navigateur de recevoir la réponse.
  // La connexion sera lancée dans loop().
  wifiConnectionAttempted = false;
  lastWiFiRetry = millis() - WIFI_RETRY_INTERVAL_MS;
}

// ============================================================
// 24. CONFIGURATION DU SERVEUR WEB
// ============================================================

void configureWebServer() {

  server.on("/", HTTP_GET, handleRoot);

  server.on("/data", HTTP_GET, handleData);

  server.on(
    "/calibrate",
    HTTP_POST,
    handleCalibration
  );

  server.on(
    "/wifi",
    HTTP_GET,
    handleWiFiPage
  );

  server.on(
    "/savewifi",
    HTTP_POST,
    handleSaveWiFi
  );

  server.onNotFound([]() {

    server.send(
      404,
      "text/plain",
      "Page introuvable"
    );
  });

  server.begin();

  Serial.println("[WEB] Serveur HTTP demarre");
}

// ============================================================
// 25. SETUP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(100);

  Serial.println();
  Serial.println("================================");
  Serial.println("       FUMOIR COLD");
  Serial.println("       NODEMCU ESP8266");
  Serial.println("================================");

  // LEDs
  pinMode(PIN_LED_SYSTEM, OUTPUT);
  pinMode(PIN_LED_CALIB, OUTPUT);
  pinMode(PIN_LED_SMOKE, OUTPUT);
  pinMode(PIN_LED_FAULT, OUTPUT);

  digitalWrite(PIN_LED_SYSTEM, LED_OFF);
  digitalWrite(PIN_LED_CALIB, LED_OFF);
  digitalWrite(PIN_LED_SMOKE, LED_OFF);
  digitalWrite(PIN_LED_FAULT, LED_OFF);

  // DS18B20
  ds18b20.begin();
  ds18b20.setWaitForConversion(false);

  // EEPROM : données système et configuration Wi-Fi
  loadEEPROM();
  loadWiFiConfig();

  // Temporisations
  unsigned long now = millis();

  lastPreheatUpdate = now;
  lastEEPROMSave = now;

  lastTemperatureRequest = now - DS18B20_INTERVAL_MS;
  lastMQ2Read = now - MQ2_INTERVAL_MS;

  // ----------------------------------------------------------
  // HOTSPOT PERMANENT
  // ----------------------------------------------------------

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);

  startFallbackAP();

  // ----------------------------------------------------------
  // CONNEXION WIFI DOMESTIQUE SI CONFIGUREE
  // ----------------------------------------------------------

  if (strlen(wifiConfig.ssid) > 0) {

    connectToHomeWiFi();

  } else {

    Serial.println("[WIFI] Configuration necessaire");
    Serial.println("[WIFI] Connecte-toi au hotspot pour renseigner le Wi-Fi");
  }

  // Serveur web
  configureWebServer();

  // Premiere conversion DS18B20
  ds18b20.requestTemperatures();

  lastTemperatureRequest = millis();
  temperatureRequestTime = millis();
  dsConversionPending = true;

  Serial.println();
  Serial.println("[SYSTEME] Initialisation terminee");

  Serial.print("[SYSTEME] Etat : ");
  Serial.println(getStateName());

  Serial.print("[WIFI] Hotspot : ");
  Serial.println(AP_SSID);

  Serial.println("[WIFI] Configuration : http://192.168.4.1/wifi");
  Serial.println("[WIFI] Tableau de bord : http://192.168.4.1");
  Serial.println("[WIFI] Adresse domestique : http://fumoir.local");

  Serial.println("================================");
}

// ============================================================
// 26. LOOP
// ============================================================

void loop() {

  // Serveur web
  server.handleClient();

  // Gestion Wi-Fi et mDNS
  updateWiFi();

  if (mdnsStarted) {
    MDNS.update();
  }

  // Capteurs
  updateTemperature();
  updateMQ2();

  // Conditionnement
  updatePreheat();

  // Calibration
  updateCalibration();

  // LEDs
  updateLEDs();

  // Moniteur serie
  printSerialStatus();

  // Sauvegarde periodique du conditionnement
  if (systemState == INITIAL_PREHEAT &&
      millis() - lastEEPROMSave >= EEPROM_SAVE_INTERVAL_MS) {

    saveEEPROM();
  }

  yield();
}
