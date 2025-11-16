# Pump Guardian: ESP32 Smart Pump Controller 💧⚡

**Pump Guardian** is a standalone, intelligent controller designed to protect your water pump from dry-run damage. This project, developed by **Athishay Jain** and exhibited at **Elixer 2025**, uses an ESP32 and an AC energy meter to perform sophisticated, real-time analysis of the pump's electrical signature.

Unlike simple current-based protectors, Pump Guardian uses a **dual-parameter adaptive baseline** logic. It learns the pump's normal operating **Active Power (W)** and **Power Factor (PF)** and triggers a shutdown only when *both* values drop simultaneously, indicating a true dry-run condition with high accuracy.

-----

## Key Features

  * **Adaptive Dry-Run Protection**: Establishes a unique baseline for Power and Power Factor after the pump stabilizes. This adapts to normal voltage fluctuations and ensures high accuracy.
  * **Dual-Parameter Detection**: A fault is only triggered if *both* Active Power and Power Factor drop below their respective thresholds, preventing false trips.
  * **Startup Sanity Check**: Immediately detects if the pump is started in an already-dry state (power below `MIN_NORMAL_POWER`) to prevent setting a bad baseline.
  * **Attempt Counter & Lockout**: Tracks the number of dry-run attempts. After a set number of faults (`MAX_DRY_RUN_ATTEMPTS`), the device enters a `permanentLockout` state to prevent damage, requiring a manual reset.
  * **Manual Controls & Reset**: Features physical buttons for Start and Stop. Holding the **Stop button for 3 seconds** resets the fault counter and clears the permanent lockout.
  * **Status Display**: A 16x2 I2C LCD shows real-time status (Voltage, Current), pump run-time, and fault messages.
  * **Audible Alerts**: An onboard buzzer provides clear audio feedback for dry-run events, lockouts, and system resets.
  * **Standalone Operation**: All logic is self-contained. No Wi-Fi or internet connection is required, making it robust and reliable.

-----

## Hardware Required

  * **Microcontroller**: ESP32 DevKitC V4 or similar board.
  * **Sensor**: PZEM-004T v3.0 AC Energy Meter with its coil.
  * **Display**: 16x2 I2C LCD Display (Address `0x27`).
  * **Switching**: 5V Single Channel Relay Module.
  * **Input**: Push Buttons (x2).
  * **Indicator**: 5V Active Buzzer.
  * **Power Supply**: 5V power supply for the ESP32 and relay.
  * **Miscellaneous**: Perfboard, jumper wires, and a suitable enclosure.

-----

## Software & Libraries

This project is built using the Arduino framework.

  * **IDE**: [Arduino IDE](https://www.arduino.cc/en/software) or [VS Code with PlatformIO](https://platformio.org/).
  * **Libraries**:
      * `Wire.h` (Built-in for I2C)
      * `LiquidCrystal_I2C` by Frank de Brabander
      * `PZEM004Tv30` by olehs

-----

## Wiring Diagram

Connect the components according to the pin definitions in the code.

  * **Relay IN**: `GPIO 13`
  * **Start Button**: `GPIO 32` (connected to GND)
  * **Stop Button**: `GPIO 33` (connected to GND)
  * **Buzzer**: `GPIO 14`
  * **PZEM-004T**: (Uses Hardware Serial 2)
      * **TX** -\> ESP32 **RX2** (`GPIO 26`)
      * **RX** -\> ESP32 **TX2** (`GPIO 25`)
  * **I2C LCD**: (Address `0x27`)
      * **SDA** -\> ESP32 **SDA** (`GPIO 21`)
      * **SCL** -\> ESP32 **SCL** (`GPIO 22`)

**⚠️ HIGH VOLTAGE WARNING**: The PZEM-004T and relay are connected to mains voltage. Ensure all power is disconnected before wiring and take all necessary safety precautions.

-----

## How It Works: The Protection Logic

1.  **Start-Up**: The user presses the **START** button. The relay turns on, and the pump starts. The LCD displays "Stabilizing...".
2.  **Stabilization Phase**: The controller waits for `STABILIZATION_DELAY` (7 seconds) to let the pump reach a stable running state.
3.  **Sanity Check & Baseline**:
      * After 7 seconds, it takes a reading. If the power is *already* below `MIN_NORMAL_POWER` (135W), it assumes the pump started dry, triggers a dry run event, and skips setting a baseline.
      * If the power is normal, the controller "learns" the healthy state by saving the current `refPower` and `refPF`.
4.  **Monitoring Phase**: The controller continuously reads power and PF.
5.  **Fault Detection**: A "potential dry run" is flagged if **BOTH** conditions are met:
      * `current_power < refPower * 0.96` (a 4% drop in power)
      * `current_pf < refPF * 0.95` (a 5% drop in power factor)
6.  **Fault Confirmation**: If this dual-drop condition persists for `DRY_RUN_CONFIRM_TIME` (3 seconds), the controller confirms a true dry run.
7.  **Shutdown**: The device triggers `handleDryRunEvent()`, which:
      * Stops the pump via the relay.
      * Sounds the alarm.
      * Displays "DRY RUN ALERT\!" on the LCD.
      * Increments the `dryRunAttempts` counter.
8.  **Lockout**: If `dryRunAttempts` reaches `MAX_DRY_RUN_ATTEMPTS` (30), the device enters `permanentLockout` and will not start again until reset.
9.  **Manual Reset**: Holding the **STOP** button for 3 seconds calls `resetSystem()`, which clears the `dryRunAttempts` counter and the `permanentLockout` flag.

-----

## Configuration

The core logic can be fine-tuned by adjusting the `const` values at the top of the `.ino` file:

```cpp
// --- Advanced Dry Run Settings ---
const float POWER_DROP_THRESHOLD = 0.96;  // Trigger if Power drops below 96% of normal
const float PF_DROP_THRESHOLD = 0.95;     // Trigger if PF drops below 95% of normal

const unsigned long STABILIZATION_DELAY = 7000;  // 7s to wait for a stable baseline
const unsigned long DRY_RUN_CONFIRM_TIME = 3000; // 3s to confirm the fault
const int MAX_DRY_RUN_ATTEMPTS = 30;             // Max faults before lockout

// --- Sanity Check Thresholds ---
const float MIN_NORMAL_POWER = 135; // If power is below this at startup, it's a dry run
```

-----

## License

This project is licensed under the MIT License. See the `LICENSE` file for details.
