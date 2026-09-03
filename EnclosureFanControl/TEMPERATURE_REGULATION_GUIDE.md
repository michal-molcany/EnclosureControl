# Temperature Regulation System - Implementation Guide

## Overview

This system automatically controls the enclosure fan and louver based on printer filament type and temperature conditions. It uses OctoPrint data to detect material type, nozzle temperature, and chamber temperature, then applies material-specific cooling strategies.

## System Architecture

### Components

1. **TemperatureRegulation Class** - Main controller
   - Manages state machine for different materials
   - Coordinates fan and louver control
   - Reads data from OctoPrinter

2. **PIDController Class** - Temperature regulation
   - Proportional-Integral-Derivative controller
   - Smoothly adjusts fan speed based on chamber temperature
   - Prevents overshooting and oscillation

3. **Integration Points**
   - OctoPrinter: Provides temperature and filament data
   - ServoLouver: Controls physical louvre opening/closing
   - Main loop: Gets called every 2 seconds for updates

## Material-Specific Behavior

### PLA Mode

**Triggers:**

- PLA filament is printing, OR
- Nozzle temperature reaches 215±10°C (205-225°C)

**Actions:**

- Opens louvre
- Starts fan on PIN 1 with PID regulation
- Maintains chamber at 30°C
- Continues for 60 seconds after print ends

**Stops when:**

- Bed temperature drops to 30°C
- Returns to IDLE state

---

### PETG Mode

**Triggers:**

- PETG filament printing, OR
- Within 30 seconds after PETG print ends

**Actions:**

- Opens louvre
- Starts cooling only if chamber ≥ 35°C
- Targets chamber at 30°C

**Stops when:**

- Chamber cools to 30°C target, OR
- Printing continues and chamber stays below threshold

**Benefits:** Faster cooling after PETG print for improved part quality

---

### ASA/ABS Mode

**Triggers:**

- ASA, ABS, or other high-temperature materials printing

**Actions:**

- Opens louvre ONLY if chamber reaches 40°C
- Applies very slow cooling (30% fan speed)
- Targets chamber at 45°C (not aggressive)

**Stops when:**

- Chamber reaches 45°C target, OR
- Chamber drops below 40°C

**Important:** Never opens louvre for other reasons to prevent warping of high-temperature materials

---

## Technical Details

### Fan Control

- **Pin:** GPIO 1
- **PWM Channel:** Channel 0
- **Frequency:** 5 kHz
- **Resolution:** 8-bit (0-255 speed levels)
- **Control:** PID-based smooth regulation

### PID Parameters

Located in `const.h`:

- **KP (Proportional):** 1.5 - Quick response to temperature changes
- **KI (Integral):** 0.1 - Eliminates steady-state error
- **KD (Derivative):** 0.5 - Reduces overshoot
- **Update Interval:** 1000ms (1 second)

### Temperature Thresholds

**PLA:**

- Nozzle target: 215°C ± 10°C tolerance
- Chamber target: 30°C
- Bed cooldown trigger: 30°C

**PETG:**

- Chamber threshold: 35°C (must reach to trigger cooling)
- Chamber target: 30°C

**ASA/ABS/Others:**

- Enclosure threshold: 40°C (critical)
- Chamber target: 45°C (gentle)
- Fan speed: 30% of normal (very slow cooling)

## State Machine

The system operates with 4 states:

```
IDLE → PLA_COOLING → IDLE
IDLE → PETG_COOLING → IDLE
IDLE → ASA_ABS_COOLING → IDLE
```

Each state runs material-specific logic and transitions based on conditions.

## Usage

### Initialization

```cpp
// In main.cpp
TemperatureRegulation tempRegulation(&prusa, &louver);

void setup() {
    tempRegulation.begin();  // Initialize PWM pins
}

void loop() {
    prusa.update();
    tempRegulation.update();  // Called every 2 seconds
    delay(2000);
}
```

### Status Checking

```cpp
if (tempRegulation.isFanActive()) {
    Serial.println("Fan is running");
}

if (tempRegulation.isLouvreOpen()) {
    Serial.println("Louvre is open");
}

String state = tempRegulation.getCurrentState();
Serial.println(state);  // Prints: IDLE, PLA_COOLING, PETG_COOLING, ASA_ABS_COOLING
```

## Serial Output Examples

### PLA Print Starting

```
State transition: PLA_COOLING
Louvre opened
[PLA] Chamber: 25.3°C, Bed: 60.5°C, Fan speed: 120
[PLA] Chamber: 28.9°C, Bed: 58.2°C, Fan speed: 45
[PLA] Chamber: 30.1°C, Bed: 55.0°C, Fan speed: 12
```

### PETG Print Cooling

```
State transition: PETG_COOLING
[PETG] Cooling from 38.5°C to target 30°C, Fan speed: 85
[PETG] Cooling from 32.1°C to target 30°C, Fan speed: 15
[PETG] Chamber cooled to target - returning to IDLE
Louvre closed
```

### ASA Print Overheating

```
State transition: ASA_ABS_COOLING
[ASA/ABS] Enclosure temperature critical - opening louvre
[ASA/ABS] Slow cooling from 42.0°C, Fan speed (slow): 25
[ASA/ABS] Slow cooling from 44.5°C, Fan speed (slow): 3
[ASA/ABS] Target temperature reached - returning to IDLE
Louvre closed
```

## Configuration Guide

### Adjusting Temperature Thresholds

Edit `include/const.h`:

```cpp
// PLA thresholds
#define TEMP_PLA_NOZZLE_TARGET 215
#define TEMP_PLA_NOZZLE_TOLERANCE 10
#define TEMP_PLA_CHAMBER_TARGET 30
#define TEMP_PLA_BED_COOLDOWN 30

// PETG thresholds
#define TEMP_PETG_CHAMBER_THRESHOLD 35

// ASA/ABS thresholds
#define TEMP_ASA_ENCLOSURE_THRESHOLD 40
#define TEMP_ASA_CHAMBER_TARGET 45
```

### Tuning PID Controller

If cooling is too slow or too aggressive, adjust PID gains:

```cpp
#define PID_KP 1.5    // Increase for faster response
#define PID_KI 0.1    // Increase to eliminate steady-state error
#define PID_KD 0.5    // Increase to reduce overshoot
```

### Changing Fan Pin

```cpp
#define FAN_PIN 1     // Change GPIO number if needed
#define FAN_PWM_CHANNEL 0  // Change PWM channel (0-3 available)
```

## Monitoring & Troubleshooting

### Fan Not Starting

- Check PIN 1 connections and PWM configuration
- Verify `tempRegulation.begin()` is called in setup
- Check serial output for state transitions

### Chamber Not Cooling

- Verify chamber temperature sensor is working (OctoPrint reports data)
- Check physical fan connections and airflow
- Monitor PID output values in serial output
- Adjust PID gains if needed

### Louvre Not Opening

- Verify louvre servo is mechanically functional
- Check SERVO_PIN and angle definitions in `const.h`
- Ensure `louver.begin()` is called before `tempRegulation.begin()`

### Material Detection Issues

- Verify filament name matches exactly (case-insensitive):
  - "PLA", "PETG", "ASA", "ABS"
- Other materials default to ASA behavior
- Check OctoPrint API returns correct filament data

## Performance Notes

- **Update frequency:** 1 second minimum (prevents spam)
- **Memory usage:** ~2KB for TemperatureRegulation class
- **CPU impact:** Minimal (simple state machine)
- **Fan PWM:** 5 kHz (inaudible frequency range)

## Future Enhancements

Potential improvements:

1. Add EEPROM settings storage for user-adjustable thresholds
2. Implement hysteresis to prevent oscillation
3. Add temperature logging to SD card
4. Web interface for real-time monitoring
5. Support for multiple fans with individual control
6. Adaptive PID tuning based on material
