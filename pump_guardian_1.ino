/******************************************************************************
 * PUMGUARD - ESP32 Smart Pump Controller
 *
 * Author: Athishay Jain (with Gemini AI)
 * Date: August 22, 2025
 *
 * Full-featured code for the Pumguard project. This code is designed to be
 * robust, with non-blocking logic and real-time cloud synchronization.
 ******************************************************************************/

// ## 1. LIBRARIES ##
//-----------------------------------------------------------------------------
#include <WiFi.h>
#include <WiFiManager.h>
#include <PZEM004Tv30.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <ArduinoOTA.h>
#include "time.h"
#include <Firebase_ESP_Client.h>

// ## 2. HARDWARE & PIN DEFINITIONS ##
//-----------------------------------------------------------------------------
#define RELAY_PIN 23
#define START_BUTTON_PIN 18
#define STOP_BUTTON_PIN 19
#define PZEM_RX_PIN 16
#define PZEM_TX_PIN 17

PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ## 3. FIREBASE CONFIGURATION ##
//-----------------------------------------------------------------------------
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT_ID" // "pumguard-1234"
#define FIREBASE_DATABASE_URL "YOUR_FIREBASE_DATABASE_URL" // "pumguard-1234.firebaseio.com" or "pumguard-1234-default-rtdb.firebaseio.com"
#define FIREBASE_USER_EMAIL "YOUR_FIREBASE_AUTH_EMAIL"
#define FIREBASE_USER_PASSWORD "YOUR_FIREBASE_AUTH_PASSWORD"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ## 4. GLOBAL VARIABLES ##
//-----------------------------------------------------------------------------
Preferences preferences;

// Pump state
bool pumpStatus = false;
String faultReason = "None";

// Sensor readings
float voltage = 0.0, current = 0.0, power = 0.0, powerFactor = 0.0;

// Protection thresholds
float overloadCurrent = 10.0, dryRunCurrent = 0.5, dryRunPowerFactor = 0.6, minVoltage = 180.0, maxVoltage = 250.0;

// Timers for non-blocking operations
unsigned long lastPzemRead = 0, lastFirebaseUpdate = 0, lastLcdUpdate = 0;
uint8_t lcdScreen = 0; // For cycling through LCD screens

// Button debouncing
unsigned long lastDebounceTime = 0;
#define DEBOUNCE_DELAY 50

// NTP & Time
const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 19800; // India Standard Time (UTC +5:30)
const int daylightOffset_sec = 0;

// ## 5. FUNCTION PROTOTYPES ##
//-----------------------------------------------------------------------------
void connectToWiFi();
void initFirebase();
void firebaseStreamCallback(FirebaseStream data);
void loadSettingsFromPreferences();
void saveSettingToPreferences(String key, float value);
void readPzemData();
void controlPump(bool turnOn, String reason);
void checkProtections();
void handleManualControls();
void updateLcdDisplay();
void syncTimeToNTP();
void handleSchedules(); // Placeholder for now
void setupOTA();
void updateFirebaseData();

// ## SETUP ##
//-----------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  
  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, LOW);
  pinMode(START_BUTTON_PIN, INPUT_PULLUP);
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Pumguard Init...");

  loadSettingsFromPreferences();
  connectToWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    lcd.setCursor(0, 1);
    lcd.print("WiFi Connected!");
    initFirebase();
    syncTimeToNTP();
    setupOTA();
  } else {
    lcd.setCursor(0, 1);
    lcd.print("Offline Mode");
  }
  
  delay(1500);
  lcd.clear();
}

// ## MAIN LOOP ##
//-----------------------------------------------------------------------------
void loop() {
  if (WiFi.status() == WL_CONNECTED) {
    ArduinoOTA.handle();
  }

  handleManualControls();

  if (millis() - lastPzemRead > 2000) {
    readPzemData();
    checkProtections();
    lastPzemRead = millis();
  }

  if (millis() - lastFirebaseUpdate > 10000) {
    if (Firebase.ready()) updateFirebaseData();
    lastFirebaseUpdate = millis();
  }

  if (millis() - lastLcdUpdate > 2000) { // Update LCD every 2 seconds
    updateLcdDisplay();
    lastLcdUpdate = millis();
  }
}

// ## FUNCTION DEFINITIONS ##
//-----------------------------------------------------------------------------

void connectToWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // 3-minute timeout for setup portal
  if (!wm.autoConnect("Pumguard-Setup")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }
}

void initFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.database_url = FIREBASE_DATABASE_URL;
  auth.user.email = FIREBASE_USER_EMAIL;
  auth.user.password = FIREBASE_USER_PASSWORD;
  
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Listen for changes in commands and settings
  if (!Firebase.beginStream(fbdo, "/")) {
    Serial.println("Stream setup failed: " + fbdo.errorReason());
  }
  Firebase.setStreamCallback(fbdo, firebaseStreamCallback, 1000);

  // Set online status
  Firebase.RTDB.setBool(&fbdo, "/pump_data/is_online", true);
  Firebase.RTDB.onDisconnectSetBool(&fbdo, "/pump_data/is_online", false);
}

void firebaseStreamCallback(FirebaseStream data) {
  // Handle remote pump control
  if (data.dataPath() == "/commands/pump_control" && data.dataTypeEnum() == fb_esp_data_type_boolean) {
    if (data.boolData() != pumpStatus) {
      controlPump(data.boolData(), "Remote");
    }
  }

  // Handle settings updates from Firebase
  if (data.dataPath() == "/settings/dry_run_sensitivity" && data.dataTypeEnum() == fb_esp_data_type_float) {
      dryRunCurrent = data.floatData();
      saveSettingToPreferences("dry_curr", dryRunCurrent);
      Serial.printf("Updated Dry Run Current to: %.2f\n", dryRunCurrent);
  }
  // Add similar 'if' blocks for your other settings like overload_current, etc.
}

void loadSettingsFromPreferences() {
  preferences.begin("pumguard", true); // Read-only mode
  overloadCurrent = preferences.getFloat("ovl_curr", 10.0);
  dryRunCurrent = preferences.getFloat("dry_curr", 0.5);
  dryRunPowerFactor = preferences.getFloat("dry_pf", 0.6);
  minVoltage = preferences.getFloat("min_volt", 180.0);
  maxVoltage = preferences.getFloat("max_volt", 250.0);
  preferences.end();
  Serial.println("Loaded settings from memory.");
}

void saveSettingToPreferences(String key, float value) {
  preferences.begin("pumguard", false); // Read-write mode
  preferences.putFloat(key.c_str(), value);
  preferences.end();
}

void readPzemData() {
  float v = pzem.voltage();
  if(!isnan(v)) voltage = v; else voltage = 0.0;
  
  float i = pzem.current();
  if(!isnan(i)) current = i; else current = 0.0;
  
  float p = pzem.power();
  if(!isnan(p)) power = p; else power = 0.0;
  
  float pf = pzem.pf();
  if(!isnan(pf)) powerFactor = pf; else powerFactor = 0.0;
}

void controlPump(bool turnOn, String reason) {
  if (pumpStatus == turnOn) return; // No change needed

  pumpStatus = turnOn;
  digitalWrite(RELAY_PIN, pumpStatus);
  Serial.printf("Pump turned %s. Reason: %s\n", turnOn ? "ON" : "OFF", reason.c_str());

  if (!turnOn) { // If turning off due to a fault
    faultReason = reason;
  } else { // If turning on, clear any previous fault
    faultReason = "None";
    // Also clear alert flags in Firebase
    if (Firebase.ready()) {
       Firebase.RTDB.setBool(&fbdo, "/pump_data/dry_run_alert", false);
       // Clear other alerts...
    }
  }
  updateFirebaseData(); // Send immediate update
}

void checkProtections() {
  if (!pumpStatus) return;

  if (voltage > 0 && (voltage < minVoltage || voltage > maxVoltage)) {
    controlPump(false, "Trip: Voltage");
  } else if (current > overloadCurrent) {
    controlPump(false, "Trip: Overload");
    if(Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/pump_data/overload_alert", true);
  } else if (current < dryRunCurrent && powerFactor < dryRunPowerFactor && power > 5) { // power > 5 to avoid false trip at start/stop
    controlPump(false, "Trip: Dry-Run");
    if(Firebase.ready()) Firebase.RTDB.setBool(&fbdo, "/pump_data/dry_run_alert", true);
  }
}

void handleManualControls() {
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (digitalRead(START_BUTTON_PIN) == LOW) {
      controlPump(true, "Manual ON");
      lastDebounceTime = millis();
    }
    if (digitalRead(STOP_BUTTON_PIN) == LOW) {
      controlPump(false, "Manual OFF");
      lastDebounceTime = millis();
    }
  }
}

void updateLcdDisplay() {
  lcd.clear();
  lcd.setCursor(0, 0);

  if (faultReason != "None" && !pumpStatus) {
    lcd.print("FAULT:");
    lcd.setCursor(0, 1);
    lcd.print(faultReason);
  } else {
      if (lcdScreen == 0) {
        lcd.printf("V:%.1fV I:%.2fA", voltage, current);
        lcd.setCursor(0, 1);
        lcd.printf("PUMP IS %s", pumpStatus ? "ON" : "OFF");
      } else if (lcdScreen == 1) {
        lcd.printf("Pwr:%.0fW", power);
        lcd.setCursor(0, 1);
        lcd.printf("PF: %.2f", powerFactor);
      }
      lcdScreen = !lcdScreen; // Toggle between screens
  }
}

void syncTimeToNTP(){
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Time synchronized");
}

void updateFirebaseData() {
  FirebaseJson jsonData;
  jsonData.set("pump_status", pumpStatus);
  jsonData.set("current", String(current, 2));
  jsonData.set("voltage", String(voltage, 1));
  jsonData.set("power", String(power, 0));
  jsonData.set("power_factor", String(powerFactor, 2));
  jsonData.set("last_operation", faultReason == "None" ? "Normal" : faultReason);
  
  String path = "/pump_data";
  if (Firebase.RTDB.setJSON(&fbdo, path.c_str(), &jsonData)) {
    //Serial.println("Firebase updated.");
  } else {
    Serial.println("Firebase update failed: " + fbdo.errorReason());
  }
}


void setupOTA() {
  ArduinoOTA.setHostname("Pumguard-ESP32");
  ArduinoOTA
    .onStart([]() {
      String type;
      if (ArduinoOTA.getCommand() == U_FLASH) type = "sketch";
      else type = "filesystem";
      Serial.println("Start updating " + type);
      lcd.clear();
      lcd.print("OTA Update...");
    })
    .onEnd([]() {
      Serial.println("\nEnd");
      lcd.clear();
      lcd.print("Update Complete!");
      delay(1000);
    })
    .onProgress([](unsigned int progress, unsigned int total) {
      Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
      lcd.setCursor(0,1);
      lcd.printf("Progress: %u%%", (progress / (total / 100)));
    })
    .onError([](ota_error_t error) {
      Serial.printf("Error[%u]: ", error);
      if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
      else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
      else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
      else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
      else if (error == OTA_END_ERROR) Serial.println("End Failed");
      ESP.restart();
    });
  ArduinoOTA.begin();
  Serial.println("OTA Ready");
}