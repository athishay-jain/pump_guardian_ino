/******************************************************************************
 * PUMPGUARDIAN - ESP32 Smart Pump Controller (FIRESTORE VERSION)
 *
 * Author: Athishay Jain
 * Date: August 23, 2025
 *
 * Full-featured code for the Pumguard project, adapted to use
 * Google Cloud Firestore for data storage and commands.
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

// ## 3. FIREBASE CONFIGURATION (FIRESTORE) ##
//-----------------------------------------------------------------------------
#define FIREBASE_API_KEY "YAIzaSyB3Kdp7Fk-xrzC4mkm1WcYm31epXi2459M"
#define FIREBASE_PROJECT_ID "pump-guardian" // Your Project ID

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ## 4. GLOBAL VARIABLES ##
//-----------------------------------------------------------------------------
Preferences preferences;

// Pump state
bool pumpStatus = false;
String faultReason = "None";
bool overload_alert = false;
bool dry_run_alert = false;

// Sensor readings
float voltage = 0.0, current = 0.0, power = 0.0, powerFactor = 0.0;

// Protection thresholds
float overloadCurrent = 10.0, dryRunCurrent = 0.5, dryRunPowerFactor = 0.6, minVoltage = 180.0, maxVoltage = 250.0;

// Timers for non-blocking operations
unsigned long lastPzemRead = 0, lastFirebaseUpdate = 0, lastLcdUpdate = 0, lastCommandCheck = 0;
uint8_t lcdScreen = 0;

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
void loadSettingsFromPreferences();
void saveSettingToPreferences(String key, float value);
void readPzemData();
void controlPump(bool turnOn, String reason);
void checkProtections();
void handleManualControls();
void updateLcdDisplay();
void syncTimeToNTP();
void setupOTA();
void updateFirebaseData();
void checkFirestoreCommands(); // New function for polling commands

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

  unsigned long currentMillis = millis();

  if (currentMillis - lastPzemRead > 2000) {
    readPzemData();
    checkProtections();
    lastPzemRead = currentMillis;
  }

  // Check for commands from Firestore every 5 seconds
  if (currentMillis - lastCommandCheck > 5000) {
    if (Firebase.ready()) checkFirestoreCommands();
    lastCommandCheck = currentMillis;
  }

  // Update Firestore with data every 10 seconds
  if (currentMillis - lastFirebaseUpdate > 10000) {
    if (Firebase.ready()) updateFirebaseData();
    lastFirebaseUpdate = currentMillis;
  }

  if (currentMillis - lastLcdUpdate > 2000) {
    updateLcdDisplay();
    lastLcdUpdate = currentMillis;
  }
}

// ## FUNCTION DEFINITIONS ##
//-----------------------------------------------------------------------------

void connectToWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // 3-minute timeout
  if (!wm.autoConnect("Pumguard-Setup")) {
    Serial.println("Failed to connect and hit timeout");
    ESP.restart();
  }
}

void initFirebase() {
  config.api_key = FIREBASE_API_KEY;
  config.project_id = FIREBASE_PROJECT_ID;

  // Use empty auth for test mode rules. For production, consider other auth methods.
  auth.user.email = "";
  auth.user.password = "";

  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  // Set initial online status in Firestore
  Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", "pump_data/live_status", "{\"fields\":{\"is_online\":{\"booleanValue\":true}}}");
}

void checkFirestoreCommands() {
  String documentPath = "pump_commands/remote_control";
  Serial.println("Checking for commands...");

  // Get the document from Firestore
  if (Firebase.Firestore.getDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str())) {
    FirebaseJson &json = fbdo.to<FirebaseJson>();
    // The data is nested under a 'fields' object
    if (json.toString().indexOf("pump_on") != -1) {
      FirebaseJsonData result;
      // Path into the JSON to get the boolean value
      json.get(result, "fields/pump_on/booleanValue");
      if(result.success) {
        bool remoteCommand = result.to<bool>();
        if (remoteCommand != pumpStatus) {
            controlPump(remoteCommand, "Remote");
        }
      }
    }
  } else {
    Serial.println("Failed to get command document: " + fbdo.errorReason());
  }
}


void loadSettingsFromPreferences() {
  preferences.begin("pumguard", true);
  overloadCurrent = preferences.getFloat("ovl_curr", 10.0);
  dryRunCurrent = preferences.getFloat("dry_curr", 0.5);
  dryRunPowerFactor = preferences.getFloat("dry_pf", 0.6);
  minVoltage = preferences.getFloat("min_volt", 180.0);
  maxVoltage = preferences.getFloat("max_volt", 250.0);
  preferences.end();
  Serial.println("Loaded settings from memory.");
}

void saveSettingToPreferences(String key, float value) {
  preferences.begin("pumguard", false);
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
  if (pumpStatus == turnOn) return;

  pumpStatus = turnOn;
  digitalWrite(RELAY_PIN, pumpStatus);
  Serial.printf("Pump turned %s. Reason: %s\n", turnOn ? "ON" : "OFF", reason.c_str());

  if (!turnOn) {
    faultReason = reason;
  } else {
    faultReason = "None";
    // Clear local alert flags when pump is turned on
    dry_run_alert = false;
    overload_alert = false;
  }
  updateFirebaseData(); // Send immediate update on state change
}

void checkProtections() {
  if (!pumpStatus) return;

  if (voltage > 0 && (voltage < minVoltage || voltage > maxVoltage)) {
    controlPump(false, "Trip: Voltage");
  } else if (current > overloadCurrent) {
    overload_alert = true;
    controlPump(false, "Trip: Overload");
  } else if (current < dryRunCurrent && powerFactor < dryRunPowerFactor && power > 5) {
    dry_run_alert = true;
    controlPump(false, "Trip: Dry-Run");
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
      lcdScreen = !lcdScreen;
  }
}

void syncTimeToNTP(){
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  Serial.println("Time synchronized");
}

void updateFirebaseData() {
  String documentPath = "pump_data/live_status";

  FirebaseJson content;
  content.set("pump_status", pumpStatus);
  content.set("voltage", String(voltage, 1));
  content.set("current", String(current, 2));
  content.set("power", String(power, 0));
  content.set("power_factor", String(powerFactor, 2));
  content.set("last_operation_reason", faultReason);
  content.set("is_online", true);
  content.set("overload_alert", overload_alert);
  content.set("dry_run_alert", dry_run_alert);
  content.set("last_update", ".sv", "timestamp"); // Server-side timestamp

  // Field mask specifies which fields to update
  String mask = "pump_status,voltage,current,power,power_factor,last_operation_reason,is_online,overload_alert,dry_run_alert,last_update";

  if (Firebase.Firestore.patchDocument(&fbdo, FIREBASE_PROJECT_ID, "", documentPath.c_str(), content.raw(), mask.c_str())) {
    Serial.println("Firestore update SUCCESS");
  } else {
    Serial.println("Firestore update FAILED: " + fbdo.errorReason());
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
