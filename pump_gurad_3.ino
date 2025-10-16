/*
  Smart Pump Guardian — ESP32 Full IoT Firmware (Dual Push Buttons)
  =================================================================
  Hardware
    • ESP32 DevKitC
    • PZEM-004T v3.0 (AC Energy Meter) on Serial2
    • 16x2 I2C LCD (0x27 default)
    • 5V Single-Channel Relay (active LOW by default)
    • Two Manual Push Buttons: START (ON) + STOP (OFF)
    • DS3231 RTC (I2C)
    • SPIFFS for offline logs + config persistence (Preferences)

  Features
    • Dry-run & Overload protection (current + PF + voltage)
    • Over/Undervoltage protection
    • Two-button manual override (works offline)
    • Daily schedule using DS3231 (works offline)
      - Seasonal/Yearly gate via start/end month (e.g., monsoon season)
    • Cooldown lockout after a FAULT (prevents rapid restarts)
    • Min off-time anti-chatter (prevents relay chattering)
    • Watchdog auto-restart
    • Offline logging to SPIFFS (JSONL) & background sync to Firestore when online
    • LCD shows live stats & last fault reason
    • Full Firestore REST integration (Email/Password auth)
      - Creates /devices/{DEVICE_ID} (config, control, status)
      - Uploads telemetry to /devices/{DEVICE_ID}/telemetry
      - Fetches control + config and applies them (including schedule + thresholds)
      - Patches device status (online/relay/fault/lastUpdate)

  Fill the CONFIG section below before flashing.
*/

/********************* LIBRARIES *********************/
#include <Arduino.h>
#include <Wire.h>
#include <SPIFFS.h>
#include <LiquidCrystal_I2C.h>
#include <RTClib.h>
#include <PZEM004Tv30.h>
#include <Preferences.h>
#include <Bounce2.h>
#include "esp_task_wdt.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>


/********************* CONFIG (EDIT ME) *********************/
// --- WiFi ---
const char* WIFI_SSID = "Ramettu";
const char* WIFI_PASSWORD = "12345678";

// --- Firebase / Firestore ---
const char* FIREBASE_API_KEY = "AIzaSyB3Kdp7Fk-xrzC4mkm1WcYm31epXi2459M";  // Web API Key
const char* FIREBASE_PROJECT_ID = "pump-guardian";                         // e.g. my-project-id
const char* FIREBASE_EMAIL = "admin@gmail.com";                            // Auth user email
const char* FIREBASE_PASSWORD = "admin123";                                // Auth user password
const char* DEVICE_ID = "pump-guardian-001";                               // Unique device id

// --- Time (IST) ---
const char* NTP_SERVER = "pool.ntp.org";
const long GMT_OFFSET = 19800;  // +05:30 IST in seconds
const int DST_OFFSET = 0;

// --- Pins ---
#define RELAY_PIN 28           // GPIO to drive relay IN
#define RELAY_ACTIVE_LOW true  // set false if your relay board is active HIGH
#define START_BTN_PIN 32       // Manual ON button (to GND, uses pull-up)
#define STOP_BTN_PIN 33        // Manual OFF button (to GND, uses pull-up)
#define PZEM_RX_PIN 26         // ESP32 RX2 (to PZEM TX)
#define PZEM_TX_PIN 25         // ESP32 TX2 (to PZEM RX)

// --- LCD ---
#define LCD_I2C_ADDR 0x27
LiquidCrystal_I2C lcd(LCD_I2C_ADDR, 16, 2);

// --- PZEM on Serial2 ---
HardwareSerial PZEMSerial(2);
PZEM004Tv30 pzem(PZEMSerial, PZEM_RX_PIN, PZEM_TX_PIN);

// --- RTC ---
RTC_DS3231 rtc;

// --- Watchdog ---
#define WDT_TIMEOUT_SEC 8

// --- Logging ---
const char* LOG_PATH = "/logs.jsonl";             // live buffer
const char* LOG_PENDING = "/logs_pending.jsonl";  // for sync cycles

// --- Safety timings ---
const unsigned long MIN_OFF_MS = 5000;      // anti-chatter: require 5s OFF before ON
const unsigned long FAULT_LOCK_MS = 30000;  // lockout after a fault for 30s

/********************* STATE & SETTINGS *********************/
Preferences prefs;  // namespace: "spg"

// Pump safety thresholds (defaults – can be updated from Firestore)
struct Thresholds {
  float minVoltage = 180.0;    // undervolt limit
  float maxVoltage = 260.0;    // overvolt limit
  float maxCurrent_A = 6.0;    // overload limit (amps)
  float minCurrent_A = 0.3;    // below this + low PF = dry run
  float minPF = 0.5;           // dry run detection threshold
};

//PumpThresholds thresholds; // global instance

Thresholds thresholds;
/********************* PROTECTION *********************/
enum FaultCode { FAULT_NONE,
                 FAULT_DRYRUN,
                 FAULT_OVERLOAD,
                 FAULT_UNDERVOLT,
                 FAULT_OVERVOLT };

FaultCode evaluateFaults(float V, float I, float PF) {
  if (V < thresholds.minVoltage) return FAULT_UNDERVOLT;
  if (V > thresholds.maxVoltage) return FAULT_OVERVOLT;
  if (I > thresholds.maxCurrent_A) return FAULT_OVERLOAD;
  if (I <= thresholds.minCurrent_A && PF <= thresholds.minPF) return FAULT_DRYRUN;
  return FAULT_NONE;
}

String faultToString(FaultCode f) {
  switch (f) {
    case FAULT_NONE: return "OK";
    case FAULT_DRYRUN: return "DRY RUN";
    case FAULT_OVERLOAD: return "OVERLOAD";
    case FAULT_UNDERVOLT: return "UNDER VOLT";
    case FAULT_OVERVOLT: return "OVER VOLT";
  }
  return "UNKNOWN";
}


struct DailySchedule {
  int onHour = 6;
  int onMinute = 0;
  int offHour = 6;
  int offMinute = 30;
  bool enabled = false;
  // Yearly/seasonal window (inclusive months 1..12)
  int seasonStartMonth = 1;
  int seasonEndMonth = 12;
};

struct State {
  bool relayCommand = false;    // desired by logic/schedule/app
  bool relayPhysical = false;   // actual pin state
  bool manualPriority = false;  // manual override latched
  String lastFault = "OK";      // human-readable fault
  float hp = 1.0;               // motor horsepower
};

DailySchedule scheduleCfg;
State state;
Bounce debStart, debStop;

/********************* FIREBASE AUTH RUNTIME *********************/
String g_idToken, g_refreshToken, g_localId;
unsigned long g_tokenExpiryMs = 0;  // millis timestamp

/********************* UTILS *********************/
inline void setRelay(bool on) {
  state.relayCommand = on;
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? !on : on);
  state.relayPhysical = on;
}

String two(int v) {
  char b[3];
  snprintf(b, sizeof(b), "%02d", v);
  return String(b);
}
String rfc3339_utc_from_epoch(time_t t) {
  struct tm tmUTC;
  gmtime_r(&t, &tmUTC);
  char buf[30];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02dZ",
           1900 + tmUTC.tm_year, 1 + tmUTC.tm_mon, tmUTC.tm_mday,
           tmUTC.tm_hour, tmUTC.tm_min, tmUTC.tm_sec);
  return String(buf);
}

String nowRFC3339UTC() {
  time_t nowSec = time(nullptr);
  if (nowSec > 1600000000) return rfc3339_utc_from_epoch(nowSec);
  // fallback: use RTC (assumed local IST); convert to UTC by subtracting GMT_OFFSET
  DateTime n = rtc.now();
  struct tm ti = { 0 };
  ti.tm_year = n.year() - 1900;
  ti.tm_mon = n.month() - 1;
  ti.tm_mday = n.day();
  ti.tm_hour = n.hour();
  ti.tm_min = n.minute();
  ti.tm_sec = n.second();
  time_t localEpoch = mktime(&ti);
  time_t utcEpoch = localEpoch - GMT_OFFSET;  // crude but OK if RTC set to IST
  return rfc3339_utc_from_epoch(utcEpoch);
}

DateTime getNow() {
  if (rtc.begin()) return rtc.now();
  time_t raw = time(nullptr);
  struct tm* ti = localtime(&raw);
  if (!ti) return DateTime(F(__DATE__), F(__TIME__));
  return DateTime(1900 + ti->tm_year, 1 + ti->tm_mon, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
}


void initWatchdog() {
  esp_task_wdt_config_t wdt_cfg = {
    .timeout_ms = WDT_TIMEOUT_SEC * 1000,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&wdt_cfg);
  esp_task_wdt_add(NULL);  // add current thread
}

/********************* PERSISTENCE *********************/
void loadPrefs() {
  prefs.begin("spg", true);
  thresholds.maxCurrent_A = prefs.getFloat("maxI", thresholds.maxCurrent_A);
  thresholds.minPF = prefs.getFloat("minPF", thresholds.minPF);
  thresholds.minCurrent_A = prefs.getFloat("minI", thresholds.minCurrent_A);
  thresholds.minVoltage = prefs.getFloat("minV", thresholds.minVoltage);
  thresholds.maxVoltage = prefs.getFloat("maxV", thresholds.maxVoltage);
  scheduleCfg.onHour = prefs.getInt("onH", scheduleCfg.onHour);
  scheduleCfg.onMinute = prefs.getInt("onM", scheduleCfg.onMinute);
  scheduleCfg.offHour = prefs.getInt("offH", scheduleCfg.offHour);
  scheduleCfg.offMinute = prefs.getInt("offM", scheduleCfg.offMinute);
  scheduleCfg.enabled = prefs.getBool("sched", scheduleCfg.enabled);
  scheduleCfg.seasonStartMonth = prefs.getInt("sMonS", scheduleCfg.seasonStartMonth);
  scheduleCfg.seasonEndMonth = prefs.getInt("sMonE", scheduleCfg.seasonEndMonth);
  state.hp = prefs.getFloat("hp", state.hp);
  prefs.end();
}

void savePrefs() {
  prefs.begin("spg", false);
  prefs.putFloat("maxI", thresholds.maxCurrent_A);
  prefs.putFloat("minPF", thresholds.minPF);
  prefs.putFloat("minI", thresholds.minCurrent_A);
  prefs.putFloat("minV", thresholds.minVoltage);
  prefs.putFloat("maxV", thresholds.maxVoltage);
  prefs.putInt("onH", scheduleCfg.onHour);
  prefs.putInt("onM", scheduleCfg.onMinute);
  prefs.putInt("offH", scheduleCfg.offHour);
  prefs.putInt("offM", scheduleCfg.offMinute);
  prefs.putBool("sched", scheduleCfg.enabled);
  prefs.putInt("sMonS", scheduleCfg.seasonStartMonth);
  prefs.putInt("sMonE", scheduleCfg.seasonEndMonth);
  prefs.putFloat("hp", state.hp);
  prefs.end();
}

/********************* LOGGING (SPIFFS JSONL) *********************/
void appendLogLine(File& f, const String& type, const String& msg) {
  String line = String("{\"ts\":\"") + nowRFC3339UTC() + "\",\"type\":\"" + type + "\",\"msg\":\"" + msg + "\"}\n";
  f.print(line);
}

void appendLog(const String& type, const String& msg) {
  File f = SPIFFS.open(LOG_PATH, FILE_APPEND);
  if (!f) return;
  appendLogLine(f, type, msg);
  f.close();
}

void rotateLogsIfTooBig(size_t maxBytes = 200 * 1024) {
  File f = SPIFFS.open(LOG_PATH, FILE_READ);
  if (!f) return;
  size_t sz = f.size();
  f.close();
  if (sz < maxBytes) return;
  SPIFFS.remove(LOG_PENDING);
  SPIFFS.rename(LOG_PATH, LOG_PENDING);
}

/********************* WIFI + NTP *********************/
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(200); }
  if (WiFi.status() == WL_CONNECTED) {
    configTime(GMT_OFFSET, DST_OFFSET, NTP_SERVER);
    time_t nowSec = time(nullptr);
    if (nowSec > 1600000000) {
      struct tm ti;
      localtime_r(&nowSec, &ti);
      rtc.adjust(DateTime(1900 + ti.tm_year, 1 + ti.tm_mon, ti.tm_mday, ti.tm_hour, ti.tm_min, ti.tm_sec));
    }
    appendLog("net", "WiFi up");
  } else appendLog("net", "WiFi fail");
}
/********************* SCHEDULER *********************/
bool seasonAllows(const DateTime& now) {
  int m = now.month();
  if (scheduleCfg.seasonStartMonth <= scheduleCfg.seasonEndMonth) {
    return (m >= scheduleCfg.seasonStartMonth && m <= scheduleCfg.seasonEndMonth);
  } else {
    return (m >= scheduleCfg.seasonStartMonth || m <= scheduleCfg.seasonEndMonth);  // wrapped year
  }
}

bool isWithinOnWindow(const DateTime& now) {
  if (!scheduleCfg.enabled) return false;
  if (!seasonAllows(now)) return false;
  int curM = now.hour() * 60 + now.minute();
  int onM = scheduleCfg.onHour * 60 + scheduleCfg.onMinute;
  int offM = scheduleCfg.offHour * 60 + scheduleCfg.offMinute;
  if (onM == offM) return false;
  if (onM < offM) return (curM >= onM && curM < offM);
  return (curM >= onM || curM < offM);  // across midnight
}

/********************* HEALTH (rough) *********************/
float estimateEfficiency(float measuredW) {
  float ratedW = state.hp * 746.0f;
  if (ratedW <= 1.0f) return 0.0f;
  return measuredW / ratedW;
}

/********************* LCD HELPERS *********************/
void lcdSplash() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Pump Gdn");
  lcd.setCursor(0, 1);
  lcd.print("Booting...");
}
void lcdStatus(float V, float I, float P, bool relay, const String& fault) {
  char l1[17];
  char l2[17];
  snprintf(l1, sizeof(l1), "V%4.0f I%3.1f ", V, I);
  snprintf(l2, sizeof(l2), "%s %s", relay ? "ON " : "OFF", fault.c_str());
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(l1);
  lcd.setCursor(0, 1);
  lcd.print(l2);
}

/********************* FIREBASE REST HELPERS *********************/
String urlBase() {
  return String("https://firestore.googleapis.com/v1/projects/") + FIREBASE_PROJECT_ID + "/databases/(default)/documents";
}

bool firebaseSignIn() {
  if (!g_idToken.isEmpty() && millis() < g_tokenExpiryMs) return true;
  HTTPClient http;
  http.begin(String("https://identitytoolkit.googleapis.com/v1/accounts:signInWithPassword?key=") + FIREBASE_API_KEY);
  http.addHeader("Content-Type", "application/json");
  StaticJsonDocument<256> doc;
  doc["email"] = FIREBASE_EMAIL;
  doc["password"] = FIREBASE_PASSWORD;
  doc["returnSecureToken"] = true;
  String body;
  serializeJson(doc, body);
  int code = http.POST(body);
  if (code == 200) {
    DynamicJsonDocument res(1024);
    deserializeJson(res, http.getString());
    g_idToken = (const char*)res["idToken"];
    g_refreshToken = (const char*)res["refreshToken"];
    g_localId = (const char*)res["localId"];
    unsigned long expires = strtoul(res["expiresIn"], nullptr, 10);  // seconds
    g_tokenExpiryMs = millis() + (expires - 60) * 1000UL;            // refresh a minute early
    http.end();
    return true;
  }
  http.end();
  return false;
}

bool httpGET(const String& url, String& out) {
  HTTPClient http;
  http.begin(url);
  if (!g_idToken.isEmpty()) http.addHeader("Authorization", String("Bearer ") + g_idToken);
  int code = http.GET();
  if (code == 200) {
    out = http.getString();
    http.end();
    return true;
  }
  out = http.getString();
  http.end();
  return false;
}

bool httpPOST(const String& url, const String& body, String& out) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  if (!g_idToken.isEmpty()) http.addHeader("Authorization", String("Bearer ") + g_idToken);
  int code = http.POST(body);
  out = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

bool httpPATCH(const String& url, const String& body, String& out) {
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  if (!g_idToken.isEmpty()) http.addHeader("Authorization", String("Bearer ") + g_idToken);
  int code = http.sendRequest("PATCH", body);
  out = http.getString();
  http.end();
  return (code >= 200 && code < 300);
}

String devicePath() {
  return String("/devices/") + DEVICE_ID;
}
String deviceDocURL() {
  return urlBase() + devicePath();
}
String telemetryColURL() {
  return urlBase() + devicePath() + "/telemetry";
}

bool ensureDeviceDoc() {
  if (!firebaseSignIn()) return false;
  String resp;
  if (httpGET(deviceDocURL(), resp)) return true;  // exists
  // Create document with defaults
  StaticJsonDocument<1024> doc;
  JsonObject fields = doc.createNestedObject("fields");
  JsonObject config = fields.createNestedObject("config").createNestedObject("mapValue").createNestedObject("fields");
  JsonObject thresholdsMap = config.createNestedObject("thresholds").createNestedObject("mapValue").createNestedObject("fields");
  thresholdsMap["maxCurrent_A"]["doubleValue"] = thresholds.maxCurrent_A;
  thresholdsMap["minPF"]["doubleValue"] = thresholds.minPF;
  thresholdsMap["minCurrent_A"]["doubleValue"] = thresholds.minCurrent_A;
  thresholdsMap["minVoltage"]["doubleValue"] = thresholds.minVoltage;
  thresholdsMap["maxVoltage"]["doubleValue"] = thresholds.maxVoltage;
  JsonObject scheduleMap = config.createNestedObject("schedule").createNestedObject("mapValue").createNestedObject("fields");
  scheduleMap["enabled"]["booleanValue"] = scheduleCfg.enabled;
  scheduleMap["onHour"]["integerValue"] = scheduleCfg.onHour;
  scheduleMap["onMinute"]["integerValue"] = scheduleCfg.onMinute;
  scheduleMap["offHour"]["integerValue"] = scheduleCfg.offHour;
  scheduleMap["offMinute"]["integerValue"] = scheduleCfg.offMinute;
  scheduleMap["seasonStartMonth"]["integerValue"] = scheduleCfg.seasonStartMonth;
  scheduleMap["seasonEndMonth"]["integerValue"] = scheduleCfg.seasonEndMonth;
  config["hp"]["doubleValue"] = state.hp;
  JsonObject control = fields.createNestedObject("control").createNestedObject("mapValue").createNestedObject("fields");
  control["forceOn"]["booleanValue"] = false;
  control["forceOff"]["booleanValue"] = false;
  control["clearManual"]["booleanValue"] = false;
  JsonObject status = fields.createNestedObject("status").createNestedObject("mapValue").createNestedObject("fields");
  status["online"]["booleanValue"] = true;
  status["relay"]["booleanValue"] = false;
  status["lastFault"]["stringValue"] = "BOOT";
  status["lastUpdate"]["timestampValue"] = nowRFC3339UTC();
  String body;
  serializeJson(doc, body);
  String out;
  String url = urlBase() + "/devices?documentId=" + DEVICE_ID;  // create with ID
  return httpPOST(url, body, out);
}

void firestoreUpdateStatus(bool online) {
  if (!firebaseSignIn()) return;
  StaticJsonDocument<768> doc;
  JsonObject fields = doc.createNestedObject("fields");
  JsonObject status = fields.createNestedObject("status").createNestedObject("mapValue").createNestedObject("fields");
  status["online"]["booleanValue"] = online;
  status["relay"]["booleanValue"] = state.relayPhysical;
  status["lastFault"]["stringValue"] = state.lastFault;
  status["lastUpdate"]["timestampValue"] = nowRFC3339UTC();
  String body;
  serializeJson(doc, body);
  String out;
  String url = deviceDocURL() + "?updateMask.fieldPaths=status";
  httpPATCH(url, body, out);
}

void firestoreUploadTelemetry(float V, float I, float P, float PF, float E) {
  if (!firebaseSignIn()) return;
  StaticJsonDocument<768> doc;
  JsonObject fields = doc.createNestedObject("fields");
  fields["timestamp"]["timestampValue"] = nowRFC3339UTC();
  fields["voltage"]["doubleValue"] = V;
  fields["current"]["doubleValue"] = I;
  fields["power"]["doubleValue"] = P;
  fields["pf"]["doubleValue"] = PF;
  fields["energy"]["doubleValue"] = E;
  fields["relay"]["booleanValue"] = state.relayPhysical;
  fields["fault"]["stringValue"] = state.lastFault;
  String body;
  serializeJson(doc, body);
  String out;
  httpPOST(telemetryColURL(), body, out);
}

// Helpers to parse nested Firestore maps
bool getFieldBool(JsonVariant v, const char* a, const char* b, const char* c, bool& out) {
  if (!v.is<JsonObject>()) return false;
  JsonVariant f = v["fields"];
  if (f.isNull()) return false;
  JsonVariant m1 = f[a]["mapValue"]["fields"];
  if (m1.isNull()) return false;
  JsonVariant m2 = m1[b]["mapValue"]["fields"];
  if (m2.isNull()) return false;
  JsonVariant val = m2[c]["booleanValue"];
  if (val.isNull()) return false;
  out = val.as<bool>();
  return true;
}

bool getFieldInt(JsonVariant v, const char* a, const char* b, const char* c, int& out) {
  JsonVariant f = v["fields"];
  if (f.isNull()) return false;
  JsonVariant m1 = f[a]["mapValue"]["fields"];
  if (m1.isNull()) return false;
  JsonVariant m2 = m1[b]["mapValue"]["fields"];
  if (m2.isNull()) return false;
  JsonVariant val = m2[c]["integerValue"];
  if (val.isNull()) return false;
  out = atoi(val.as<const char*>());
  return true;
}

bool getFieldDouble(JsonVariant v, const char* a, const char* b, const char* c, float& out) {
  JsonVariant f = v["fields"];
  if (f.isNull()) return false;
  JsonVariant m1 = f[a]["mapValue"]["fields"];
  if (m1.isNull()) return false;
  JsonVariant m2 = m1[b]["mapValue"]["fields"];
  if (m2.isNull()) return false;
  JsonVariant val = m2[c]["doubleValue"];
  if (val.isNull()) return false;
  out = val.as<float>();
  return true;
}

bool getFieldDouble1(JsonVariant v, const char* a, const char* c, float& out) {
  JsonVariant f = v["fields"];
  if (f.isNull()) return false;
  JsonVariant m1 = f[a]["mapValue"]["fields"];
  if (m1.isNull()) return false;
  JsonVariant val = m1[c]["doubleValue"];
  if (val.isNull()) return false;
  out = val.as<float>();
  return true;
}

// Fetch config + control from device doc
void firestoreFetchAndApply() {
  if (!firebaseSignIn()) return;
  String resp;
  if (!httpGET(deviceDocURL(), resp)) return;
  DynamicJsonDocument doc(6144);  // device doc can be large
  DeserializationError e = deserializeJson(doc, resp);
  if (e) return;
  JsonVariant root = doc;
  // thresholds
  float v;
  if (getFieldDouble(root, "config", "thresholds", "maxCurrent_A", v)) thresholds.maxCurrent_A = v;
  if (getFieldDouble(root, "config", "thresholds", "minPF", v)) thresholds.minPF = v;
  if (getFieldDouble(root, "config", "thresholds", "minCurrent_A", v)) thresholds.minCurrent_A = v;
  if (getFieldDouble(root, "config", "thresholds", "minVoltage", v)) thresholds.minVoltage = v;
  if (getFieldDouble(root, "config", "thresholds", "maxVoltage", v)) thresholds.maxVoltage = v;
  // schedule
  int iv;
  bool bv;
  float fv;
  if (getFieldBool(root, "config", "schedule", "enabled", bv)) scheduleCfg.enabled = bv;
  if (getFieldInt(root, "config", "schedule", "onHour", iv)) scheduleCfg.onHour = iv;
  if (getFieldInt(root, "config", "schedule", "onMinute", iv)) scheduleCfg.onMinute = iv;
  if (getFieldInt(root, "config", "schedule", "offHour", iv)) scheduleCfg.offHour = iv;
  if (getFieldInt(root, "config", "schedule", "offMinute", iv)) scheduleCfg.offMinute = iv;
  if (getFieldInt(root, "config", "schedule", "seasonStartMonth", iv)) scheduleCfg.seasonStartMonth = iv;
  if (getFieldInt(root, "config", "schedule", "seasonEndMonth", iv)) scheduleCfg.seasonEndMonth = iv;
  // hp
  if (getFieldDouble1(root, "config", "hp", v)) state.hp = v;
  savePrefs();
  // control
  bool forceOn = false, forceOff = false, clearManual = false;
  JsonVariant fields = root["fields"];
  if (!fields.isNull()) {
    JsonVariant control = fields["control"]["mapValue"]["fields"];
    if (!control.isNull()) {
      if (!control["forceOn"].isNull()) forceOn = control["forceOn"]["booleanValue"].as<bool>();
      if (!control["forceOff"].isNull()) forceOff = control["forceOff"]["booleanValue"].as<bool>();
      if (!control["clearManual"].isNull()) clearManual = control["clearManual"]["booleanValue"].as<bool>();
    }
  }
  if (clearManual) state.manualPriority = false;
  if (forceOn && !forceOff) {
    state.manualPriority = false;
    setRelay(true);
  }
  if (forceOff && !forceOn) {
    state.manualPriority = false;
    setRelay(false);
  }
  if (forceOn || forceOff || clearManual) {
    StaticJsonDocument<384> up;
    JsonObject fields = up.createNestedObject("fields");
    JsonObject control = fields.createNestedObject("control").createNestedObject("mapValue").createNestedObject("fields");
    control["forceOn"]["booleanValue"] = false;
    control["forceOff"]["booleanValue"] = false;
    control["clearManual"]["booleanValue"] = false;
    String body;
    serializeJson(up, body);
    String out;
    String url = deviceDocURL() + "?updateMask.fieldPaths=control";
    httpPATCH(url, body, out);
  }
}
// Put this after includes, before setup()


/********************* SETUP *********************/
unsigned long lastOffMs = 0;       // for anti-chatter
unsigned long faultLockUntil = 0;  // for fault lockout

void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  setRelay(false);
  lastOffMs = millis();
  pinMode(START_BTN_PIN, INPUT_PULLUP);
  pinMode(STOP_BTN_PIN, INPUT_PULLUP);
  debStart.attach(START_BTN_PIN);
  debStart.interval(25);
  debStop.attach(STOP_BTN_PIN);
  debStop.interval(25);

  Wire.begin();
  lcd.init();
  lcd.backlight();
  lcdSplash();
  if (!SPIFFS.begin(true)) { /* auto-format on first run */
  }
  loadPrefs();

  if (!rtc.begin()) appendLog("rtc", "DS3231 missing");
  if (rtc.lostPower()) rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));

  Serial.begin(115200);
  delay(200);
  PZEMSerial.begin(9600, SERIAL_8N1, PZEM_RX_PIN, PZEM_TX_PIN);

  connectWiFi();
  if (WiFi.status() == WL_CONNECTED) {
    firebaseSignIn();
    ensureDeviceDoc();
  }

  initWatchdog();

  appendLog("boot", "Firmware started");
}

/********************* LOOP *********************/
unsigned long lastSampleMs = 0, lastLcdMs = 0, lastNetMs = 0, lastSyncMs = 0, lastRotateMs = 0;
bool wasRunning = false;
unsigned long runStartMs = 0;

void loop() {
  esp_task_wdt_reset();
  debStart.update();
  debStop.update();

  // Manual ON / OFF buttons (falling edge)
  if (debStart.fell()) {
    state.manualPriority = true;
    if (millis() - lastOffMs >= MIN_OFF_MS && millis() >= faultLockUntil) {
      setRelay(true);
      appendLog("button", "Manual ON");
    } else {
      appendLog("button", "ON blocked (cooldown/lockout)");
    }
  }
  if (debStop.fell()) {
    state.manualPriority = true;
    setRelay(false);
    lastOffMs = millis();
    appendLog("button", "Manual OFF");
  }

  unsigned long nowMs = millis();

FaultCode fault = FAULT_NONE;

  // Sample PZEM @ 500ms
  static float V = 0, I = 0, P = 0, PF = 1.0, E = 0;
  if (nowMs - lastSampleMs >= 500) {
    lastSampleMs = nowMs;
    V = pzem.voltage();
    I = pzem.current();
    P = pzem.power();
    PF = pzem.pf();
    E = pzem.energy();
    if (isnan(V)) V = 0;
    if (isnan(I)) I = 0;
    if (isnan(P)) P = 0;
    if (isnan(PF)) PF = 1.0;
    if (isnan(E)) E = 0;

    // Desired command from schedule unless manualPriority latched
    if (!state.manualPriority) {
      bool shouldRun = isWithinOnWindow(getNow());
      state.relayCommand = shouldRun;
    }

    esp_task_wdt_reset();

    // Evaluate faults
    FaultCode fault = evaluateFaults(V, I, PF);
    if (fault != FAULT_NONE) {
      state.lastFault = faultToString(fault);
      if (state.relayPhysical) {
        setRelay(false);
        state.manualPriority = false;
        lastOffMs = nowMs;
        faultLockUntil = nowMs + FAULT_LOCK_MS;
        appendLog("fault", state.lastFault + String(" V=") + V + " I=" + I + " PF=" + PF);
      }
    } else {
      state.lastFault = (nowMs < faultLockUntil) ? String("LOCKOUT") : String("OK");
      if (!state.manualPriority) {
        if (state.relayCommand) {
          if (nowMs - lastOffMs >= MIN_OFF_MS && nowMs >= faultLockUntil) { setRelay(true); }
        } else {
          if (state.relayPhysical) {
            setRelay(false);
            lastOffMs = nowMs;
          }
        }
      }
    }

    // Health note
    float eff = estimateEfficiency(P);
    if (eff > 0 && eff < 0.4 && state.relayPhysical) appendLog("health", String("Low eff ") + eff + " P=" + P);

    // Runtime accounting
    if (state.relayPhysical && !wasRunning) {
      wasRunning = true;
      runStartMs = nowMs;
      appendLog("run", "start");
    } else if (!state.relayPhysical && wasRunning) {
      wasRunning = false;
      unsigned long dur = nowMs - runStartMs;
      appendLog("run", String("stop ") + (dur / 1000) + "s");
    }
  }

  // LCD @ 1s
  if (nowMs - lastLcdMs >= 1000) {
    lastLcdMs = nowMs;
    lcdStatus(V, I, P, state.relayPhysical, state.lastFault);
  }

  // Network cycle @ 5s
  if (nowMs - lastNetMs >= 5000) {
    lastNetMs = nowMs;
    if (WiFi.status() != WL_CONNECTED) connectWiFi();
    if (WiFi.status() == WL_CONNECTED) {
      if (firebaseSignIn()) {
        ensureDeviceDoc();
        firestoreUpdateStatus(true);
        firestoreFetchAndApply();
        firestoreUploadTelemetry(V, I, P, PF, E);
        // offline sync if any
        if (SPIFFS.exists(LOG_PENDING)) {
          File f = SPIFFS.open(LOG_PENDING, FILE_READ);
          if (f) {
            while (f.available()) {
              String line = f.readStringUntil('\n');
              if (line.length() < 5) continue;
              // Convert saved line (type+msg) into telemetry note (store as log event)
              StaticJsonDocument<768> doc;
              DeserializationError e = deserializeJson(doc, line);
              if (e) continue;
              StaticJsonDocument<768> out;
              JsonObject fields = out.createNestedObject("fields");
              fields["timestamp"]["timestampValue"] = (const char*)doc["ts"];
              fields["eventType"]["stringValue"] = (const char*)doc["type"];
              fields["eventMsg"]["stringValue"] = (const char*)doc["msg"];
              String body;
              serializeJson(out, body);
              String resp;
              httpPOST(telemetryColURL(), body, resp);
            }
            f.close();
            SPIFFS.remove(LOG_PENDING);
          }
        }
      }
    } else {
      // If WiFi down, move growing log to pending occasionally
      rotateLogsIfTooBig();
    }
  }

  // Periodically rotate logs even when online (every 30s)
  if (nowMs - lastRotateMs >= 30000) {
    lastRotateMs = nowMs;
    rotateLogsIfTooBig();
  }
}

/********************* REMOTE CONFIG SETTERS (OPTIONAL) *********************/
void setMaxCurrent(float A) {
  thresholds.maxCurrent_A = A;
  savePrefs();
  appendLog("cfg", String("maxI=") + A);
}
void setMinPF(float pf) {
  thresholds.minPF = pf;
  savePrefs();
  appendLog("cfg", String("minPF=") + pf);
}
void setMinCurrent(float A) {
  thresholds.minCurrent_A = A;
  savePrefs();
  appendLog("cfg", String("minI=") + A);
}
void setMinVoltage(float V) {
  thresholds.minVoltage = V;
  savePrefs();
  appendLog("cfg", String("minV=") + V);
}
void setMaxVoltage(float V) {
  thresholds.maxVoltage = V;
  savePrefs();
  appendLog("cfg", String("maxV=") + V);
}
void setHP(float hp) {
  state.hp = hp;
  savePrefs();
  appendLog("cfg", String("hp=") + hp);
}
void setSchedule(int onH, int onM, int offH, int offM, bool en, int sMon, int eMon) {
  scheduleCfg.onHour = onH;
  scheduleCfg.onMinute = onM;
  scheduleCfg.offHour = offH;
  scheduleCfg.offMinute = offM;
  scheduleCfg.enabled = en;
  scheduleCfg.seasonStartMonth = sMon;
  scheduleCfg.seasonEndMonth = eMon;
  savePrefs();
  appendLog("cfg", String("sched ") + onH + ":" + onM + "-" + offH + ":" + offM + (en ? " ON" : " OFF") + " mon " + sMon + ".." + eMon);
}
