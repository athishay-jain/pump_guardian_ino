#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <PZEM004Tv30.h>

// Pin definitions
const int PZEM_RX_PIN = 26;
const int PZEM_TX_PIN = 25;
const int RELAY_PIN = 13;
const int BTN_START_PIN = 32;
const int BTN_STOP_PIN = 33;
const int BUZZER_PIN = 14;

// LCD and PZEM objects
LiquidCrystal_I2C lcd(0x27, 16, 2);
PZEM004Tv30 pzem(Serial2, PZEM_RX_PIN, PZEM_TX_PIN);

// --- Advanced Dry Run Settings ---
// Based on analysis, a dry run shows a significant drop in BOTH Power and Power Factor.
// We will set the threshold for a 10% drop in Power and a 5% drop in Power Factor from the baseline.
const float POWER_DROP_THRESHOLD = 0.96;  // Trigger if Power drops below 90% of normal
const float PF_DROP_THRESHOLD = 0.95;     // Trigger if Power Factor drops below 95% of normal

const unsigned long STABILIZATION_DELAY = 7000;   // Wait 7s after start to establish a stable baseline
const unsigned long DRY_RUN_CONFIRM_TIME = 3000;  // Confirm if low power/pf persists for 3s
const int MAX_DRY_RUN_ATTEMPTS = 30;

// --- Sanity Check Thresholds (NEW) ---
// If the pump starts and the baseline power is already below this value,
// it's likely starting in a dry state. Prevents setting a bad baseline.
const float MIN_NORMAL_POWER = 135; // Based on user data (Normal: ~223W, Dry: ~191W) 140//

// Variables
bool pumpRunning = false;
bool permanentLockout = false;
int dryRunAttempts = 0;
unsigned long pumpStartTime = 0;
unsigned long dryRunDetectTime = 0; // Timer for confirming a dry run condition
unsigned long lastUpdate = 0;
const unsigned long UPDATE_INTERVAL = 2000;

// Adaptive Baseline Variables (NEW)
float refPower = 0.0;     // Reference Active Power (W) established after stabilization
float refPF = 0.0;        // Reference Power Factor established after stabilization
float lastPowerRatio = 1.0;
float lastPfRatio = 1.0;

// Debouncing
unsigned long lastStartPress = 0;
unsigned long lastStopPress = 0;
const unsigned long DEBOUNCE_DELAY = 200;

void setup() {
  Serial.begin(115200);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BTN_START_PIN, INPUT_PULLUP);
  pinMode(BTN_STOP_PIN, INPUT_PULLUP);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(RELAY_PIN, HIGH);   // Pump OFF
  digitalWrite(BUZZER_PIN, LOW);

  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Smart Pump Ctrl");
  lcd.setCursor(0, 1);
  lcd.print("v5.0 Dual Param"); // Updated version
  delay(2000);

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Dual Detection");
  lcd.setCursor(0, 1);
  lcd.print("Power + PF");
  delay(2000);
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ready...");
  lcd.setCursor(0, 1);
  lcd.print("Press START");

  Serial.println("══════════════════════════════════════");
  Serial.println("SMART PUMP CONTROLLER v5.0 (Dual-Parameter)");
  Serial.println("✓ Dry run detection using Power & Power Factor");
  Serial.println("✓ Max Attempts: 30");
  Serial.println("══════════════════════════════════════\n");
}

void loop() {
  handleButtons();

  if (pumpRunning && !permanentLockout) {
    checkDryRun();
  }

  if (millis() - lastUpdate >= UPDATE_INTERVAL) {
    lastUpdate = millis();
    updateDisplay();
  }
}

//----------------------------------------------------//
// BUTTON HANDLING (Unchanged from original)
//----------------------------------------------------//
void handleButtons() {
  bool startPressed = (digitalRead(BTN_START_PIN) == LOW);
  bool stopPressed = (digitalRead(BTN_STOP_PIN) == LOW);

  // START
  if (startPressed && (millis() - lastStartPress > DEBOUNCE_DELAY)) {
    lastStartPress = millis();
    if (permanentLockout) showLockoutMessage();
    else startPump();
  }

  // STOP and RESET (Hold for 3s to reset)
  if (stopPressed && (millis() - lastStopPress > DEBOUNCE_DELAY)) {
    unsigned long pressStart = millis();
    lastStopPress = millis();

    while (digitalRead(BTN_STOP_PIN) == LOW) {
      if (millis() - pressStart > 3000) {
        resetSystem();
        while (digitalRead(BTN_STOP_PIN) == LOW) delay(10);
        return;
      }
      delay(10);
    }

    stopPump(false);
  }
}


//----------------------------------------------------//
// START / STOP CONTROL
//----------------------------------------------------//
void startPump() {
  if (!pumpRunning && !permanentLockout) {
    digitalWrite(RELAY_PIN, LOW); // Turn relay ON
    pumpRunning = true;
    pumpStartTime = millis();
    
    // Reset adaptive parameters for this new run
    refPower = 0.0;
    refPF = 0.0;
    dryRunDetectTime = 0;
    lastPowerRatio = 1.0;
    lastPfRatio = 1.0;

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUMP STARTED");
    lcd.setCursor(0, 1);
    lcd.print("Stabilizing...");
    Serial.println("\n▶ PUMP STARTED - Waiting for system to stabilize...");
    delay(1500);
  }
}

void stopPump(bool isDryRun) {
  if (pumpRunning) {
    digitalWrite(RELAY_PIN, HIGH); // Turn relay OFF
    pumpRunning = false;

    if (isDryRun) {
      Serial.println("⚠ Pump stopped: Dry run detected!");
    } else {
      Serial.println("■ Pump stopped: Manual");
    }

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("PUMP STOPPED");
    lcd.setCursor(0, 1);
    lcd.print(isDryRun ? "Dry Run!" : "Manual Stop");
    delay(1500);
  }
}

//----------------------------------------------------//
// DUAL-PARAMETER DRY RUN DETECTION (Power + PF)
//----------------------------------------------------//
void checkDryRun() {
  unsigned long runDuration = millis() - pumpStartTime;
  
  // Get fresh readings from PZEM sensor
  float voltage = pzem.voltage();
  float current = pzem.current();
  float power = pzem.power();
  float pf = pzem.pf();

  // Check for invalid readings from the sensor
  if (isnan(voltage) || isnan(current) || isnan(power) || isnan(pf) || power < 10.0) {
    return;
  }

  // STEP 1: Establish a baseline after the initial stabilization period
  if (runDuration >= STABILIZATION_DELAY && refPower == 0.0) {
    // NEW: Sanity check before setting the baseline.
    // If initial power is below our known minimum for a healthy run, trigger immediately.
    if (power < MIN_NORMAL_POWER) {
        Serial.println("------------------------------------------");
        Serial.println("! ERROR: Initial power is too low!");
        Serial.print("  > Power: "); Serial.print(power, 1); Serial.print("W is below minimum of "); Serial.println(MIN_NORMAL_POWER, 1);
        Serial.println("  > Assuming start in a DRY RUN condition.");
        Serial.println("------------------------------------------");
        handleDryRunEvent(power, pf); // Trigger dry run immediately
        return; // Exit the function to prevent setting a bad baseline
    }

    // If the power is normal, proceed to set the baseline
    refPower = power;
    refPF = pf;
    Serial.println("------------------------------------------");
    Serial.println("✓ System stabilized. Baseline captured.");
    Serial.print("  > Reference Power: "); Serial.print(refPower, 1); Serial.println(" W");
    Serial.print("  > Reference PF: "); Serial.println(refPF, 3);
    Serial.println("------------------------------------------");

    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Baseline Set");
    lcd.setCursor(0, 1);
    lcd.print("Monitoring...");
    delay(1500);
  }

  // STEP 2: Once a baseline is set, start monitoring for a dry run condition
  if (refPower > 0.0) {
    // Calculate current ratios for display/logging
    lastPowerRatio = power / refPower;
    lastPfRatio = pf / refPF;

    // Check if BOTH power and power factor have dropped below their thresholds
    bool isPowerLow = (power < refPower * POWER_DROP_THRESHOLD);
    bool isPfLow = (pf < refPF * PF_DROP_THRESHOLD);

    if (isPowerLow && isPfLow) {
      // If a potential dry run is detected, start a confirmation timer
      if (dryRunDetectTime == 0) {
        dryRunDetectTime = millis();
        Serial.println("! Potential dry run detected. Starting confirmation timer...");
      }
      
      // If the condition persists for the confirmation duration, trigger a dry run event
      if (millis() - dryRunDetectTime >= DRY_RUN_CONFIRM_TIME) {
        handleDryRunEvent(power, pf);
      }
    } else {
      // If conditions return to normal, reset the confirmation timer
      dryRunDetectTime = 0;
    }
  }
}


//----------------------------------------------------//
// HANDLE DRY RUN EVENT
//----------------------------------------------------//
void handleDryRunEvent(float currentPower, float currentPF) {
  stopPump(true);
  dryRunAttempts++;

  Serial.println("══════════════════════════════════════");
  Serial.println("‼ DRY RUN CONFIRMED (Dual-Parameter) ‼");
  Serial.print("  > Power dropped to "); Serial.print(currentPower, 1); Serial.print("W ("); Serial.print(lastPowerRatio * 100, 0); Serial.println("%)");
  Serial.print("  > PF dropped to "); Serial.print(currentPF, 2); Serial.print(" ("); Serial.print(lastPfRatio * 100, 0); Serial.println("%)");
  Serial.print("  > Attempt "); Serial.print(dryRunAttempts); Serial.print("/"); Serial.println(MAX_DRY_RUN_ATTEMPTS);
  Serial.println("══════════════════════════════════════");

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("DRY RUN ALERT!");
  lcd.setCursor(0, 1);
  lcd.print("Power & PF Low");
  soundAlarm(4);
  delay(2000);

  if (dryRunAttempts >= MAX_DRY_RUN_ATTEMPTS) {
    permanentLockout = true;
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("LOCKED OUT!");
    lcd.setCursor(0, 1);
    lcd.print("Hold STOP 3s");
    Serial.println("🔒 PERMANENT LOCKOUT! Manual reset required.");
    soundAlarm(6);
  } else {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Check Water!");
    lcd.setCursor(0, 1);
    lcd.print("Press START");
  }

  // Reset timers and references for the next run
  dryRunDetectTime = 0;
  refPower = 0.0;
  refPF = 0.0;
}


//----------------------------------------------------//
// SYSTEM RESET
//----------------------------------------------------//
void resetSystem() {
  dryRunAttempts = 0;
  permanentLockout = false;
  refPower = 0.0;
  refPF = 0.0;
  
  if (pumpRunning) {
    stopPump(false);
  }
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("SYSTEM RESET");
  lcd.setCursor(0, 1);
  lcd.print("Counters Cleared");
  soundAlarm(2);

  Serial.println("\n✓ SYSTEM RESET COMPLETE");
  delay(2000);
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ready!");
  lcd.setCursor(0, 1);
  lcd.print("Press START");
}

//----------------------------------------------------//
// DISPLAY + ALARM
//----------------------------------------------------//
void updateDisplay() {
  float voltage = pzem.voltage();
  float current = pzem.current();

  lcd.clear();
  lcd.setCursor(0, 0);

  if (permanentLockout) {
    lcd.print("LOCKED! HoldSTOP");
  } else if (pumpRunning) {
    lcd.print("ON ");
    unsigned long runtime = (millis() - pumpStartTime) / 1000;
    int minutes = runtime / 60;
    int seconds = runtime % 60;
    char timeStr[6];
    sprintf(timeStr, "%02d:%02d", minutes, seconds);
    lcd.print(timeStr);
    
    // Show power ratio as a health indicator
    lcd.print(" P:");
    lcd.print((int)(lastPowerRatio * 100));
    lcd.print("%");
    
  } else {
    lcd.print("OFF Try:");
    lcd.print(dryRunAttempts);
    lcd.print("/");
    lcd.print(MAX_DRY_RUN_ATTEMPTS);
  }

  lcd.setCursor(0, 1);
  if (isnan(voltage) || isnan(current)){
     lcd.print("V:--- I:--.--");
  } else {
    lcd.print("V:");
    lcd.print(voltage, 0);
    lcd.print(" I:");
    lcd.print(current, 2);
  }
  
  // More detailed Serial log (only log if not locked out to avoid spam)
  if(!permanentLockout && pumpRunning) {
    float power = pzem.power();
    float pf = pzem.pf();
    Serial.println("──────────────────────────────────────");
    Serial.print("Status: "); Serial.println("🟢 RUNNING");
    Serial.print("Voltage: "); Serial.print(voltage, 1); Serial.println(" V");
    Serial.print("Current: "); Serial.print(current, 3); Serial.println(" A");
    Serial.print("Power: "); Serial.print(power, 1); Serial.println(" W");
    Serial.print("Power Factor: "); Serial.println(pf, 3);
    if(refPower > 0) {
       Serial.print("Power Ratio: "); Serial.print(lastPowerRatio * 100, 1); Serial.println("%");
       Serial.print("PF Ratio: "); Serial.print(lastPfRatio * 100, 1); Serial.println("%");
    }
    Serial.print("Attempts: "); Serial.print(dryRunAttempts); Serial.print("/"); Serial.println(MAX_DRY_RUN_ATTEMPTS);
    Serial.println("──────────────────────────────────────\n");
  }
}

void showLockoutMessage() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("LOCKED OUT!");
  lcd.setCursor(0, 1);
  lcd.print("Hold STOP 3s");
  soundAlarm(2);
  delay(2000);
}

void soundAlarm(int beeps) {
  for (int i = 0; i < beeps; i++) {
    digitalWrite(BUZZER_PIN, HIGH);
    delay(150);
    digitalWrite(BUZZER_PIN, LOW);
    delay(150);
  }
}

