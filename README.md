# Pumpguardian: ESP32 Smart Pump Controller 💧⚡

**Pumpguardian** is a DIY, IoT-enabled smart controller designed to protect your water pump from common electrical and mechanical faults. Built using an ESP32, it offers robust protection against dry-running, overloads, and voltage fluctuations, all while providing remote monitoring and control through a Firebase backend.

This project was developed by **Athishay Jain**.

-----

## \#\# Key Features

  * **Smart Protection**: Automatically shuts down the pump during:
      * **Dry-Run**: Detects no-load conditions using a combination of **current** and **power factor** for high accuracy.
      * **Overload**: Prevents motor burnout from high current draw.
      * **Voltage Faults**: Protects against under-voltage and over-voltage conditions.
  * **IoT Connected**: Connects to your Wi-Fi and syncs with a Firebase Realtime Database.
  * **Remote Control & Monitoring**: Turn the pump on/off and monitor real-time voltage, current, and power from anywhere via a custom app.
  * **Offline Capability**: Protection thresholds are saved to the ESP32's memory, ensuring the pump remains protected even if Wi-Fi is disconnected.
  * **User-Friendly Setup**: Uses **WiFiManager** to create a captive portal for easy Wi-Fi configuration without hardcoding credentials.
  * **Manual Override**: Physical push buttons for manual start and stop control.
  * **LCD Status Display**: An onboard I2C LCD shows real-time stats, pump status, and fault reasons.
  * **OTA Updates**: Update the device firmware wirelessly over the air.

-----

## \#\# Hardware Required

  * **Microcontroller**: ESP32 DevKitC V4 or similar board.
  * **Sensor**: PZEM-004T v3.0 AC Energy Meter with its coil.
  * **Display**: 16x2 I2C LCD Display.
  * **Switching**: 5V Single Channel Relay Module.
  * **Input**: Push Buttons (x2).
  * **Power Supply**: 5V power supply for the ESP32 and relay.
  * **Miscellaneous**: Breadboard, jumper wires, and a suitable enclosure.

-----

## \#\# Software & Libraries

This project is built using the Arduino framework.

  * **IDE**: [Arduino IDE](https://www.arduino.cc/en/software) or [VS Code with PlatformIO](https://platformio.org/).
  * **Libraries**:
      * `WiFiManager` by tzapu
      * `Firebase ESP32 Client` by Mobizt
      * `PZEM-004T v3.0` by olehs
      * `LiquidCrystal_I2C` by Frank de Brabander
      * `Preferences.h` (built-in)
      * `ArduinoOTA` (built-in)

-----

## \#\# Wiring Diagram

Connect the components according to the pin definitions in the code.

  * **Relay IN**: `GPIO 23`
  * **Start Button**: `GPIO 18`
  * **Stop Button**: `GPIO 19`
  * **PZEM-004T**:
      * **TX** -\> ESP32 **RX2** (`GPIO 16`)
      * **RX** -\> ESP32 **TX2** (`GPIO 17`)
  * **I2C LCD**:
      * **SDA** -\> ESP32 **SDA** (`GPIO 21`)
      * **SCL** -\> ESP32 **SCL** (`GPIO 22`)

**⚠️ HIGH VOLTAGE WARNING**: The PZEM-004T and relay are connected to mains voltage. Ensure all power is disconnected before wiring and take all necessary safety precautions.

-----

## \#\# Setup Instructions

#### \#\#\# 1. Firebase Configuration

1.  Create a new project in the [Firebase Console](https://console.firebase.google.com/).
2.  Set up the **Realtime Database**. In the rules tab, set `.read` and `.write` to `true` for initial testing.
3.  Set up **Authentication**. Enable the "Email/Password" provider and create a user.
4.  In your Project Settings, find your **Web API Key**, **Project ID**, and **Database URL**.

#### \#\#\# 2. Code Configuration

1.  Clone this repository.
2.  Open the `.ino` file in your IDE.
3.  Fill in your Firebase credentials in the **"FIREBASE CONFIGURATION"** section of the code.

<!-- end list -->

```cpp
#define FIREBASE_API_KEY "YOUR_FIREBASE_WEB_API_KEY"
#define FIREBASE_PROJECT_ID "YOUR_FIREBASE_PROJECT_ID"
#define FIREBASE_DATABASE_URL "YOUR_FIREBASE_DATABASE_URL"
#define FIREBASE_USER_EMAIL "YOUR_FIREBASE_AUTH_EMAIL"
#define FIREBASE_USER_PASSWORD "YOUR_FIREBASE_AUTH_PASSWORD"
```

4.  Install all the required libraries using the IDE's Library Manager.

#### \#\#\# 3. First Boot & Wi-Fi Setup

1.  Upload the code to your ESP32.
2.  On the first boot, the device will create a Wi-Fi hotspot named **"Pumguard-Setup"**.
3.  Connect to this hotspot with your phone or computer. A captive portal will open automatically.
4.  Select your home Wi-Fi network, enter the password, and save. The ESP32 will then connect to your network and the Pumguard will be online.

-----

## \#\# Firebase Database Structure

Your Realtime Database should be structured as follows for the code to work correctly.

```json
{
  "pump_data": {
    "pump_status": false,
    "current": 0,
    "voltage": 0,
    "power": 0,
    "power_factor": 0,
    "last_operation": "None",
    "is_online": false,
    "dry_run_alert": false,
    "overload_alert": false
  },
  "commands": {
    "pump_control": false
  },
  "settings": {
    "dry_run_sensitivity": 0.5,
    "overload_current": 10.0
  }
}
```

-----

## \#\# License

This project is licensed under the MIT License. See the `LICENSE` file for details.
