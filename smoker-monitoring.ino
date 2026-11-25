
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <EEPROM.h>
#include <math.h>

// ============================================================
// BROCHAGE - NODEMCU ESP8266
// ============================================================

#define PIN_DS18B20       D0
#define PIN_LED_SYSTEM    D1
#define PIN_LED_CALIB     D2
#define PIN_LED_SMOKE     D5
#define PIN_LED_FAULT     D6
#define PIN_MQ2           A0

// LEDs actives a HIGH.
// Inverser ces deux valeurs si les LEDs sont actives a LOW.
#define LED_ON  HIGH
#define LED_OFF LOW

// ============================================================
// WIFI
// ============================================================

const char* WIFI_SSID = "FUMOIR_COLD";
const char* WIFI_PASSWORD = "Fumage2026";

ESP8266WebServer server(80);

// ============================================================
// CAPTEURS
// ============================================================

OneWire oneWire(PIN_DS18B20);
DallasTemperature ds18b20(&oneWire);

// ============================================================
// PARAMETRES DU SYSTEME
// ============================================================

// Conditionnement initial : 24 heures
const uint64_t INITIAL_PREHEAT_MS =
    24ULL * 60ULL * 60ULL * 1000ULL;

// Stabilisation avant calibration : 5 minutes
const unsigned long CALIBRATION_STABILIZATION_MS = 300000UL;

// Nombre de mesures pour la calibration
const int CALIBRATION_SAMPLES = 100;

// Intervalle entre les mesures de calibration
const unsigned long CALIBRATION_INTERVAL_MS = 200UL;

// Marge de securite : seuil minimum = moyenne - 15 %
const float MIN_SMOKE_MARGIN_PERCENT = 15.0;

// Limites de detection d'un defaut ADC
const int MQ2_ADC_MIN = 5;
const int MQ2_ADC_MAX = 1018;

// Sauvegarde EEPROM toutes les 5 minutes
const unsigned long EEPROM_SAVE_INTERVAL_MS = 300000UL;

// Rafraichissement des capteurs
const unsigned long DS18B20_INTERVAL_MS = 2000UL;
const unsigned long DS18B20_CONVERSION_MS = 750UL;
const unsigned long MQ2_INTERVAL_MS = 500UL;

// Moniteur serie
const unsigned long SERIAL_INTERVAL_MS = 1000UL;

// LEDs
const unsigned long LED_BLINK_INTERVAL_MS = 500UL;

// Verification du hotspot
const unsigned long WIFI_CHECK_INTERVAL_MS = 10000UL;

// Timeout d'acquisition de calibration
const unsigned long CALIBRATION_TIMEOUT_MS = 60000UL;

// ============================================================
// EEPROM
// ============================================================

const uint32_t EEPROM_MAGIC = 0xF0A01235;
const int EEPROM_SIZE = 128;

struct PersistentData {
  uint32_t magic;
  uint64_t preheatElapsed;
  int32_t smokeThreshold;
  uint8_t conditioned;
  uint8_t reserved[3];
};

PersistentData persistentData;

// ============================================================
// ETATS DU SYSTEME
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
// VARIABLES
// ============================================================

// Temperature
float temperature = NAN;
bool temperatureOK = false;

// MQ-2
int mq2Raw = 0;
int smokeThreshold = 0;
float smokeVariation = 0.0;
bool mq2SensorFault = false;

// Etat de la fumee
bool smokeAlert = false;

// Calibration
int calibrationCount = 0;
long calibrationSum = 0;
float calibrationAverage = 0.0;
unsigned long calibrationStartTime = 0;
unsigned long lastCalibrationSample = 0;

// Conditionnement
uint64_t preheatElapsed = 0;
unsigned long lastPreheatUpdate = 0;
unsigned long lastEEPROMSave = 0;

// Temporisations
unsigned long lastTemperatureRequest = 0;
unsigned long temperatureRequestTime = 0;
unsigned long lastMQ2Read = 0;
unsigned long lastSerialPrint = 0;
unsigned long lastLEDUpdate = 0;
unsigned long lastWiFiCheck = 0;

// Etat des capteurs
bool dsConversionPending = false;

// Wi-Fi
bool hotspotActive = false;

// ============================================================
// FONCTIONS GENERALES
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
// EEPROM
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

  Serial.println("[EEPROM] Sauvegarde effectuee");
}

void loadEEPROM() {

  EEPROM.begin(EEPROM_SIZE);
  EEPROM.get(0, persistentData);

  if (persistentData.magic != EEPROM_MAGIC) {

    Serial.println("[EEPROM] Donnees absentes ou invalides");
    Serial.println("[EEPROM] Initialisation du systeme");

    preheatElapsed = 0;
    smokeThreshold = 0;

    saveEEPROM();

    systemState = INITIAL_PREHEAT;
    return;
  }

  preheatElapsed = persistentData.preheatElapsed;
  smokeThreshold = persistentData.smokeThreshold;

  if (preheatElapsed > INITIAL_PREHEAT_MS) {
    preheatElapsed = INITIAL_PREHEAT_MS;
  }

  Serial.println("[EEPROM] Donnees restaurees");
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
// WIFI / HOTSPOT
// ============================================================

void startHotspot() {

  WiFi.mode(WIFI_AP);

  bool result = WiFi.softAP(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  hotspotActive = result;

  if (result) {

    Serial.println("[WIFI] Hotspot actif");

    Serial.print("[WIFI] SSID : ");
    Serial.println(WIFI_SSID);

    Serial.print("[WIFI] Adresse IP : ");
    Serial.println(WiFi.softAPIP());

  } else {

    Serial.println("[WIFI] Echec du demarrage du hotspot");
  }
}

void updateHotspot() {

  unsigned long now = millis();

  if (now - lastWiFiCheck < WIFI_CHECK_INTERVAL_MS) {
    return;
  }

  lastWiFiCheck = now;

  IPAddress ip = WiFi.softAPIP();

  hotspotActive =
    (WiFi.getMode() == WIFI_AP ||
     WiFi.getMode() == WIFI_AP_STA) &&
    ip != IPAddress(0, 0, 0, 0);

  if (!hotspotActive) {

    Serial.println("[WIFI] Hotspot indisponible, tentative de redemarrage");

    startHotspot();
  }
}

// ============================================================
// DS18B20
// ============================================================

void updateTemperature() {

  unsigned long now = millis();

  // Recuperation du resultat de conversion
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

  // Demande d'une nouvelle conversion
  if (!dsConversionPending &&
      now - lastTemperatureRequest >= DS18B20_INTERVAL_MS) {

    ds18b20.requestTemperatures();

    lastTemperatureRequest = now;
    temperatureRequestTime = now;
    dsConversionPending = true;
  }
}

// ============================================================
// MQ-2
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

  // IMPORTANT :
  // Le fonctionnement suppose que la valeur analogique
  // augmente lorsque la quantite de fumee augmente.
  //
  // Il n'existe PAS de seuil maximum.
  // Une alerte est declenchee uniquement lorsque
  // la valeur passe sous le seuil minimum.

  smokeAlert =
    (systemState == MONITORING &&
     !mq2SensorFault &&
     smokeThreshold > 0 &&
     mq2Raw < smokeThreshold);
}

// ============================================================
// CONDITIONNEMENT INITIAL
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
// CALIBRATION
// ============================================================

void startCalibration() {

  if (!isCalibrationPossible()) {
    Serial.println("[CALIBRATION] Impossible dans cet etat");
    return;
  }

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
  Serial.println("[CALIBRATION] Stabilisation de 5 minutes");
  Serial.println("[CALIBRATION] Le fumoir doit produire une fumee satisfaisante");
  Serial.println("================================");
}

void updateCalibration() {

  unsigned long now = millis();

  // ----------------------------------------------------------
  // ETAPE 1 : STABILISATION
  // ----------------------------------------------------------

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

  // ----------------------------------------------------------
  // ETAPE 2 : ACQUISITION
  // ----------------------------------------------------------

  if (systemState != CALIBRATION) {
    return;
  }

  // Timeout de l'acquisition
  if (now - calibrationStartTime >
      CALIBRATION_TIMEOUT_MS) {

    Serial.println("[CALIBRATION] ERREUR : timeout");
    Serial.println("[CALIBRATION] Calibration abandonnee");

    systemState = WAIT_CALIBRATION;

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

    Serial.println("[CALIBRATION] Mesure invalide, ignoree");
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

  // ----------------------------------------------------------
  // FIN DE CALIBRATION
  // ----------------------------------------------------------

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
// LEDS
// ============================================================

void updateLEDs() {

  unsigned long now = millis();

  bool blinkState =
    ((now / LED_BLINK_INTERVAL_MS) % 2) == 0;

  // ----------------------------------------------------------
  // LED VERTE : SYSTEME OPERATIONNEL
  // ----------------------------------------------------------

  bool systemOperational =
    hotspotActive &&
    systemState == MONITORING &&
    !hasSensorFault();

  if (systemOperational) {

    digitalWrite(PIN_LED_SYSTEM, LED_ON);

  } else {

    digitalWrite(
      PIN_LED_SYSTEM,
      blinkState ? LED_ON : LED_OFF
    );
  }

  // ----------------------------------------------------------
  // LED JAUNE : CONDITIONNEMENT / CALIBRATION
  // ----------------------------------------------------------

  bool calibrationActive =
    (systemState == INITIAL_PREHEAT ||
     systemState == STABILIZATION ||
     systemState == CALIBRATION);

  digitalWrite(
    PIN_LED_CALIB,
    calibrationActive ? LED_ON : LED_OFF
  );

  // ----------------------------------------------------------
  // LED ROUGE : MANQUE DE FUMEE
  // ----------------------------------------------------------

  digitalWrite(
    PIN_LED_SMOKE,
    smokeAlert ? LED_ON : LED_OFF
  );

  // ----------------------------------------------------------
  // LED BLEUE : DEFAUT CAPTEUR
  // ----------------------------------------------------------

  digitalWrite(
    PIN_LED_FAULT,
    hasSensorFault() ? LED_ON : LED_OFF
  );
}

// ============================================================
// MONITEUR SERIE
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

  Serial.print("Hotspot WiFi : ");
  Serial.println(hotspotActive ? "ACTIF" : "INACTIF");

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
// PAGE WEB
// ============================================================

const char MAIN_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="fr">

<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">

<title>Fumoir - Surveillance</title>

<style>

:root {
  --background: #f3f5f8;
  --card: #ffffff;
  --text: #202936;
  --muted: #788493;
  --green: #15965a;
  --green-bg: #e5f6ed;
  --red: #d94343;
  --red-bg: #fdeaea;
  --orange: #d58b19;
  --orange-bg: #fff3d9;
  --blue: #337fc4;
  --border: #e6eaf0;
}

* {
  box-sizing: border-box;
}

body {
  margin: 0;
  padding: 16px;
  background: var(--background);
  color: var(--text);
  font-family: Arial, Helvetica, sans-serif;
}

.container {
  max-width: 650px;
  margin: auto;
}

.header {
  display: flex;
  justify-content: space-between;
  align-items: center;
  margin-bottom: 18px;
}

.logo {
  font-size: 22px;
  font-weight: 800;
  letter-spacing: 1px;
}

.logo span {
  color: #e87932;
}

.wifi {
  font-size: 12px;
  font-weight: bold;
  padding: 7px 11px;
  border-radius: 20px;
  background: var(--green-bg);
  color: var(--green);
}

.wifi.off {
  background: var(--red-bg);
  color: var(--red);
}

.card {
  background: var(--card);
  border-radius: 16px;
  padding: 18px;
  margin-bottom: 13px;
  box-shadow: 0 2px 8px rgba(0,0,0,0.035);
}

.main-status {
  border-left: 5px solid var(--green);
}

.main-status.alert {
  border-left-color: var(--red);
}

.main-status.wait {
  border-left-color: var(--orange);
}

.status-title {
  font-size: 19px;
  font-weight: 800;
  margin-bottom: 6px;
}

.status-description {
  font-size: 13px;
  color: var(--muted);
  line-height: 1.5;
}

.grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 12px;
}

.metric-label {
  color: var(--muted);
  font-size: 11px;
  font-weight: bold;
  letter-spacing: 0.7px;
  margin-bottom: 12px;
}

.metric-value {
  font-size: 29px;
  font-weight: 800;
  margin-bottom: 8px;
}

.metric-unit {
  font-size: 14px;
  font-weight: normal;
  color: var(--muted);
}

.tag {
  display: inline-block;
  padding: 5px 9px;
  border-radius: 20px;
  font-size: 11px;
  font-weight: bold;
}

.tag.green {
  background: var(--green-bg);
  color: var(--green);
}

.tag.red {
  background: var(--red-bg);
  color: var(--red);
}

.tag.orange {
  background: var(--orange-bg);
  color: var(--orange);
}

.tag.blue {
  background: #e8f1fc;
  color: var(--blue);
}

.section-title {
  font-size: 15px;
  font-weight: 800;
  margin-bottom: 15px;
}

.smoke-value {
  display: flex;
  justify-content: space-between;
  align-items: baseline;
}

.smoke-number {
  font-size: 30px;
  font-weight: 800;
}

.progress {
  width: 100%;
  height: 11px;
  background: #e9edf1;
  border-radius: 20px;
  overflow: hidden;
  margin: 12px 0;
}

.progress-bar {
  height: 100%;
  width: 0%;
  background: var(--green);
  border-radius: 20px;
  transition: width 0.3s;
}

.progress-bar.red {
  background: var(--red);
}

.progress-bar.orange {
  background: var(--orange);
}

.small-text {
  color: var(--muted);
  font-size: 12px;
  line-height: 1.5;
}

.sensor-row {
  display: flex;
  justify-content: space-between;
  align-items: center;
  padding: 12px 0;
  border-bottom: 1px solid var(--border);
  font-size: 13px;
}

.sensor-row:last-child {
  border-bottom: none;
  padding-bottom: 0;
}

button {
  width: 100%;
  border: none;
  border-radius: 12px;
  padding: 15px;
  font-size: 14px;
  font-weight: bold;
  cursor: pointer;
  margin-top: 12px;
}

.button-primary {
  background: #253447;
  color: white;
}

.button-primary:disabled {
  opacity: 0.4;
  cursor: not-allowed;
}

.details {
  margin-top: 10px;
}

details summary {
  cursor: pointer;
  font-size: 14px;
  font-weight: bold;
  padding: 5px 0;
}

.detail-row {
  display: flex;
  justify-content: space-between;
  font-size: 12px;
  padding: 8px 0;
  border-bottom: 1px solid var(--border);
}

.footer {
  text-align: center;
  font-size: 11px;
  color: var(--muted);
  padding: 10px;
}

.hidden {
  display: none;
}

@media(max-width: 390px) {

  body {
    padding: 10px;
  }

  .metric-value {
    font-size: 25px;
  }

  .card {
    padding: 15px;
  }
}

</style>
</head>

<body>

<div class="container">

  <div class="header">

    <div class="logo">
      <span>🔥</span> FUMOIR
    </div>

    <div id="wifi" class="wifi">
      ● Connecté
    </div>

  </div>

  <!-- ETAT GENERAL -->

  <div id="mainStatus" class="card main-status">

    <div id="statusTitle" class="status-title">
      Initialisation...
    </div>

    <div id="statusDescription" class="status-description">
      Connexion au système de surveillance.
    </div>

  </div>

  <!-- TEMPERATURE ET FUMEE -->

  <div class="grid">

    <div class="card">

      <div class="metric-label">
        🌡 TEMPERATURE
      </div>

      <div class="metric-value">
        <span id="temperature">--.-</span>
        <span class="metric-unit">°C</span>
      </div>

      <span id="temperatureStatus" class="tag orange">
        En attente
      </span>

    </div>

    <div class="card">

      <div class="metric-label">
        ☁ NIVEAU DE FUMEE
      </div>

      <div id="smokeText" class="metric-value" style="font-size:21px">
        En attente
      </div>

      <span id="smokeStatus" class="tag orange">
        En attente
      </span>

    </div>

  </div>

  <!-- SURVEILLANCE FUMEE -->

  <div class="card">

    <div class="section-title">
      Surveillance de la fumée
    </div>

    <div class="smoke-value">

      <div>
        <div class="small-text">Valeur du capteur MQ-2</div>
        <div id="mq2" class="smoke-number">---</div>
      </div>

      <div style="text-align:right">
        <div class="small-text">Seuil minimum</div>
        <div id="threshold" style="font-size:20px;font-weight:bold">
          ---
        </div>
      </div>

    </div>

    <div class="progress">
      <div id="smokeBar" class="progress-bar"></div>
    </div>

    <div class="smoke-value">

      <span class="small-text">
        <span id="variation">--</span> % par rapport au seuil
      </span>

      <span id="smokeConclusion" class="small-text">
        En attente
      </span>

    </div>

    <div class="small-text" style="margin-top:12px">
      Le système vérifie uniquement que le niveau de fumée
      reste supérieur au seuil minimum défini lors de la calibration.
    </div>

  </div>

  <!-- CONDITIONNEMENT -->

  <div id="preheatCard" class="card hidden">

    <div class="section-title">
      Conditionnement initial
    </div>

    <div class="small-text">
      Le capteur MQ-2 effectue son conditionnement initial.
      Cette opération se poursuit automatiquement.
    </div>

    <div class="smoke-value" style="margin-top:15px">

      <strong id="preheatProgress">0 %</strong>

      <span id="preheatRemaining" class="small-text">
        Temps restant : --
      </span>

    </div>

    <div class="progress">
      <div id="preheatBar" class="progress-bar orange"></div>
    </div>

  </div>

  <!-- CALIBRATION -->

  <div id="calibrationCard" class="card hidden">

    <div class="section-title">
      Calibration du détecteur
    </div>

    <div id="calibrationDescription" class="small-text">
      Place le fumoir dans ses conditions normales de fonctionnement,
      avec une fumée satisfaisante.
    </div>

    <div id="calibrationProgressContainer" class="hidden">

      <div class="smoke-value" style="margin-top:15px">

        <strong id="calibrationProgress">0 / 100</strong>

        <span id="calibrationPercent" class="small-text">
          0 %
        </span>

      </div>

      <div class="progress">
        <div id="calibrationBar" class="progress-bar orange"></div>
      </div>

    </div>

    <button
      id="calibrationButton"
      class="button-primary"
      onclick="startCalibration()">

      Lancer la calibration

    </button>

  </div>

  <!-- CAPTEURS -->

  <div class="card">

    <div class="section-title">
      Etat des équipements
    </div>

    <div class="sensor-row">
      <span>🌡 Sonde de température</span>
      <span id="dsStatus" class="tag orange">---</span>
    </div>

    <div class="sensor-row">
      <span>☁ Détecteur MQ-2</span>
      <span id="mqStatus" class="tag orange">---</span>
    </div>

    <div class="sensor-row">
      <span>📶 Point d'accès Wi-Fi</span>
      <span id="wifiStatus" class="tag orange">---</span>
    </div>

  </div>

  <!-- DETAILS TECHNIQUES -->

  <div class="card details">

    <details>

      <summary>⚙ Détails techniques</summary>

      <div style="margin-top:15px">

        <div class="detail-row">
          <span>Etat du système</span>
          <strong id="technicalState">---</strong>
        </div>

        <div class="detail-row">
          <span>Valeur ADC MQ-2</span>
          <strong id="technicalMQ2">---</strong>
        </div>

        <div class="detail-row">
          <span>Seuil minimum</span>
          <strong id="technicalThreshold">---</strong>
        </div>

        <div class="detail-row">
          <span>Moyenne calibration</span>
          <strong id="technicalAverage">---</strong>
        </div>

        <div class="detail-row">
          <span>Mesures calibration</span>
          <strong id="technicalSamples">---</strong>
        </div>

      </div>

    </details>

  </div>

  <div class="footer">
    FUMOIR COLD · Mise à jour automatique
    <br>
    <span id="lastUpdate">Connexion...</span>
  </div>

</div>

<script>

function setTag(id, text, color) {

  const element = document.getElementById(id);

  element.textContent = text;
  element.className = "tag " + color;
}

function setText(id, text) {

  document.getElementById(id).textContent = text;
}

function formatDuration(milliseconds) {

  let seconds = Math.floor(milliseconds / 1000);

  let hours = Math.floor(seconds / 3600);
  let minutes = Math.floor((seconds % 3600) / 60);

  return hours + " h " + minutes + " min";
}

function updateDashboard(data) {

  // WIFI

  const wifi = document.getElementById("wifi");

  if (data.hotspot) {

    wifi.textContent = "● Connecté";
    wifi.className = "wifi";

    setTag("wifiStatus", "Actif", "green");

  } else {

    wifi.textContent = "● Déconnecté";
    wifi.className = "wifi off";

    setTag("wifiStatus", "Inactif", "red");
  }

  // TEMPERATURE

  if (data.temperatureOK) {

    setText("temperature", data.temperature.toFixed(1));

    setTag("temperatureStatus", "Capteur OK", "green");
    setTag("dsStatus", "OK", "green");

  } else {

    setText("temperature", "--.-");

    setTag("temperatureStatus", "Défaut", "red");
    setTag("dsStatus", "Défaut", "red");
  }

  // MQ2

  setText("mq2", data.mq2);
  setText("threshold", data.threshold > 0 ? data.threshold : "---");

  if (data.mqFault) {

    setText("smokeText", "Indisponible");

    setTag("smokeStatus", "Défaut capteur", "red");
    setTag("mqStatus", "Défaut", "red");

  } else {

    setTag("mqStatus", "OK", "green");

    if (data.stateCode === 4 && data.threshold > 0) {

      if (data.smokeAlert) {

        setText("smokeText", "Insuffisante");

        setTag("smokeStatus", "Manque de fumée", "red");

      } else {

        setText("smokeText", "Suffisante");

        setTag("smokeStatus", "Niveau correct", "green");
      }

    } else {

      setText("smokeText", "En attente");

      setTag("smokeStatus", "Non surveillée", "orange");
    }
  }

  // JAUGE DE FUMEE

  setText("variation", data.variation.toFixed(1));

  let bar = document.getElementById("smokeBar");

  let percentage = 0;

  if (data.threshold > 0) {

    percentage = Math.min(
      100,
      Math.max(0, (data.mq2 / (data.threshold * 1.5)) * 100)
    );
  }

  bar.style.width = percentage + "%";

  if (data.smokeAlert) {

    bar.className = "progress-bar red";
    setText("smokeConclusion", "Sous le seuil");

  } else if (data.stateCode === 4 && !data.mqFault) {

    bar.className = "progress-bar";
    setText("smokeConclusion", "Au-dessus du seuil");

  } else {

    bar.className = "progress-bar orange";
    setText("smokeConclusion", "En attente");
  }

  // ETAT GENERAL

  const main = document.getElementById("mainStatus");

  main.className = "card main-status";

  let title = "";
  let description = "";

  if (data.stateCode === 0) {

    title = "Conditionnement initial";
    description = "Le capteur effectue son conditionnement. La surveillance sera disponible à son terme.";
    main.classList.add("wait");

  } else if (data.stateCode === 1) {

    title = "Calibration nécessaire";
    description = "Le conditionnement est terminé. Lance une calibration avec une fumée satisfaisante.";
    main.classList.add("wait");

  } else if (data.stateCode === 2) {

    title = "Stabilisation du capteur";
    description = "Le capteur se stabilise avant l'acquisition des mesures.";
    main.classList.add("wait");

  } else if (data.stateCode === 3) {

    title = "Calibration en cours";
    description = "Acquisition des mesures du capteur MQ-2.";
    main.classList.add("wait");

  } else if (data.stateCode === 4) {

    if (data.sensorFault) {

      title = "Défaut détecté";
      description = "Un capteur nécessite une vérification.";
      main.classList.add("alert");

    } else if (data.smokeAlert) {

      title = "Manque de fumée";
      description = "Le niveau de fumée est passé sous le seuil minimum.";
      main.classList.add("alert");

    } else {

      title = "Surveillance active";
      description = "Le système fonctionne normalement. Le niveau de fumée est suffisant.";
    }
  }

  if (!data.hotspot) {

    description += " Attention : le point d'accès Wi-Fi est indisponible.";
    main.classList.add("alert");
  }

  if (data.sensorFault && data.stateCode !== 4) {

    description += " Un défaut capteur est également détecté.";
  }

  setText("statusTitle", title);
  setText("statusDescription", description);

  // CONDITIONNEMENT

  const preheatCard = document.getElementById("preheatCard");

  if (data.stateCode === 0) {

    preheatCard.classList.remove("hidden");

    let progress =
      (data.preheatElapsed / data.preheatTotal) * 100;

    progress = Math.min(100, progress);

    setText("preheatProgress", progress.toFixed(1) + " %");

    setText(
      "preheatRemaining",
      "Temps restant : " + formatDuration(data.preheatRemaining)
    );

    document.getElementById("preheatBar").style.width =
      progress + "%";

  } else {

    preheatCard.classList.add("hidden");
  }

  // CALIBRATION

  const calibrationCard =
    document.getElementById("calibrationCard");

  const calibrationButton =
    document.getElementById("calibrationButton");

  const calibrationDescription =
    document.getElementById("calibrationDescription");

  const calibrationProgressContainer =
    document.getElementById("calibrationProgressContainer");

  if (data.stateCode === 1 ||
      data.stateCode === 2 ||
      data.stateCode === 3 ||
      data.stateCode === 4) {

    calibrationCard.classList.remove("hidden");

  } else {

    calibrationCard.classList.add("hidden");
  }

  if (data.stateCode === 1) {

    calibrationButton.disabled = false;
    calibrationButton.textContent = "Lancer la calibration";

    calibrationDescription.textContent =
      "Place le fumoir dans ses conditions normales de fonctionnement, avec une fumée satisfaisante.";

    calibrationProgressContainer.classList.add("hidden");

  } else if (data.stateCode === 2) {

    calibrationButton.disabled = true;
    calibrationButton.textContent = "Stabilisation en cours...";

    calibrationDescription.textContent =
      "Le capteur se stabilise pendant 5 minutes avant les mesures.";

    calibrationProgressContainer.classList.add("hidden");

  } else if (data.stateCode === 3) {

    calibrationButton.disabled = true;
    calibrationButton.textContent = "Calibration en cours...";

    calibrationDescription.textContent =
      "Acquisition automatique des mesures.";

    calibrationProgressContainer.classList.remove("hidden");

    let progress =
      (data.calibrationCount / 100) * 100;

    setText(
      "calibrationProgress",
      data.calibrationCount + " / 100"
    );

    setText(
      "calibrationPercent",
      progress.toFixed(0) + " %"
    );

    document.getElementById("calibrationBar").style.width =
      progress + "%";

  } else if (data.stateCode === 4) {

    calibrationButton.disabled = false;
    calibrationButton.textContent = "Recalibrer le capteur";

    calibrationDescription.textContent =
      "Tu peux lancer une nouvelle calibration si les conditions de fumage ont changé.";

    calibrationProgressContainer.classList.add("hidden");

  } else {

    calibrationCard.classList.add("hidden");
  }

  // DETAILS TECHNIQUES

  setText("technicalState", data.state);
  setText("technicalMQ2", data.mq2);
  setText("technicalThreshold", data.threshold);

  setText(
    "technicalAverage",
    data.calibrationAverage.toFixed(1)
  );

  setText(
    "technicalSamples",
    data.calibrationCount + " / 100"
  );

  setText("lastUpdate", "Dernière actualisation : maintenant");
}

async function refreshData() {

  try {

    const response = await fetch("/data", {
      cache: "no-store"
    });

    if (!response.ok) {
      throw new Error("Erreur serveur");
    }

    const data = await response.json();

    updateDashboard(data);

  } catch (error) {

    setText("statusTitle", "Connexion interrompue");

    setText(
      "statusDescription",
      "Impossible de communiquer avec le fumoir."
    );

    document.getElementById("mainStatus").className =
      "card main-status alert";

    document.getElementById("wifi").textContent =
      "● Hors ligne";
  }
}

async function startCalibration() {

  if (!confirm(
    "Le fumoir produit-il actuellement une fumée satisfaisante ?"
  )) {
    return;
  }

  try {

    const response = await fetch("/calibrate", {
      method: "POST"
    });

    const result = await response.text();

    alert(result);

    refreshData();

  } catch (error) {

    alert("Impossible de lancer la calibration.");
  }
}

refreshData();

setInterval(refreshData, 1000);

</script>

</body>
</html>
)rawliteral";

// ============================================================
// SERVEUR WEB
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
  json += hotspotActive ? "true" : "false";
  json += ",";

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

  server.send(
    200,
    "application/json",
    json
  );
}

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

void configureWebServer() {

  server.on("/", HTTP_GET, handleRoot);

  server.on("/data", HTTP_GET, handleData);

  server.on(
    "/calibrate",
    HTTP_POST,
    handleCalibration
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
// SETUP
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

  // Conversion asynchrone
  ds18b20.setWaitForConversion(false);

  // EEPROM
  loadEEPROM();

  // Initialisation des temporisations
  unsigned long now = millis();

  lastPreheatUpdate = now;
  lastEEPROMSave = now;

  lastTemperatureRequest = now - DS18B20_INTERVAL_MS;
  lastMQ2Read = now - MQ2_INTERVAL_MS;

  // Wi-Fi
  startHotspot();

  // Serveur web
  configureWebServer();

  // Premiere demande de temperature
  ds18b20.requestTemperatures();

  lastTemperatureRequest = millis();
  temperatureRequestTime = millis();
  dsConversionPending = true;

  Serial.println();
  Serial.println("[SYSTEME] Initialisation terminee");
  Serial.print("[SYSTEME] Etat : ");
  Serial.println(getStateName());

  Serial.print("[SYSTEME] Interface web : http://");
  Serial.println(WiFi.softAPIP());

  Serial.println("================================");
}

// ============================================================
// LOOP
// ============================================================

void loop() {

  // Serveur web
  server.handleClient();

  // Connexion Wi-Fi
  updateHotspot();

  // Lecture des capteurs
  updateTemperature();
  updateMQ2();

  // Gestion du conditionnement
  updatePreheat();

  // Gestion de la calibration
  updateCalibration();

  // Gestion des LEDs
  updateLEDs();

  // Moniteur serie
  printSerialStatus();

  // Sauvegarde periodique EEPROM
  if (systemState == INITIAL_PREHEAT &&
      millis() - lastEEPROMSave >= EEPROM_SAVE_INTERVAL_MS) {

    saveEEPROM();
  }

  yield();
}
