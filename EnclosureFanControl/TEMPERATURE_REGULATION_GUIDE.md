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
- Targets chamber at 50°C (not aggressive)

**Stops when:**

- Chamber reaches 50°C target, OR
- Chamber drops below 40°C

**Important:** Never opens louvre for other reasons to prevent warping of high-temperature materials

---

## Technical Details

### Fan Control

- **Pin:** GPIO 1
- **PWM Channel:** Channel 4, timer 2
- **Frequency:** 25 kHz
- **Resolution:** 8-bit (0-255 speed levels)
- **Control:** PID-based smooth regulation

> **Do not move the fan to channel 0.** Channel 0, timer 0 belongs to the louvre
> servo (ESP32Servo runs at 50 Hz there). Arduino maps channel to timer, so
> putting the fan on the same timer makes each retune the other and the servo
> stops working. The split is deliberate.

**The fan's usable duty range is not 0-100%.** A 2/3-wire fan on a low-side
switch cannot break away from rest below roughly half duty, and between half duty
and full speed it spins unevenly so blade-pass becomes tonal. Measured on this
hardware: nothing spun below ~50%, and it resonated below ~60%. Three mechanisms
in `const.h` map the commanded 0-100% onto the range the rotor can actually
achieve:

| Setting | Default | Purpose |
|---|---|---|
| `FAN_MIN_RUN_PCT` | 60 | Any nonzero request is raised to this. Below it the fan stalls or screams. |
| `FAN_START_BOOST_PCT` / `_MS` | 100 / 300 ms | Full power briefly on off→on, because break-away torque is highest at rest and worse cold. |
| `FAN_MIN_ON_MS` | 3000 | Holds the fan on after it starts. `update()` is gated to 1 Hz, so a PID demand near the engage threshold would otherwise chop the fan every second and re-trigger the boost — audibly worse than the original fault. |

`FAN_MIN_ON_MS` is bypassed by the fail-safe paths (invalid OctoPrint data,
leaving the active-auto state) via `forceFanOff()`, so a safety shutdown is never
delayed. `serviceFan()` must be called from `loop()` on every iteration to end the
boost promptly; `update()` alone only reaches the fan once a second.

**Control resolution is not lost to the floor.** The louvre is commanded at 1%
steps across 0-100 and saturates before the fan is even engaged — in automatic
mode the fan only runs once PID demand exceeds 100 (`demand - 100`). So the
0-100% span is still covered, just by the louvre at the bottom and the fan at the
top.

The reported `fanPercent()` is the **actual** PWM duty, not the request, so the
CYD shows 60% when 10% was asked for rather than displaying a value the fan is
not honouring.

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
- Chamber target: 50°C (gentle)
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
[ASA/ABS] Slow cooling from 52.0°C, Fan speed (slow): 25
[ASA/ABS] Slow cooling from 50.5°C, Fan speed (slow): 3
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
#define TEMP_ASA_CHAMBER_TARGET 50
```

### Tuning PID Controller

The gains are adjustable at runtime. The C3 owns them and keeps them in NVS, so a
tuning survives a reboot and there is never a second copy that can disagree with
the board. Defaults (and the accepted ranges) live in
`lib/EnclosureProto/src/enclosure_pid.h`:

```cpp
#define PID_KP_DEFAULT 6.0f  // Increase for faster response
#define PID_KI_DEFAULT 0.1f  // Increase to eliminate steady-state error
#define PID_KD_DEFAULT 0.0f  // Increase to reduce overshoot (see the header)
```

Those are the values a blank NVS is seeded with - editing them only affects a
board whose stored tuning has been cleared.

#### The web UI (easiest)

Open **`http://enclosure-fan-control.local:8082/`** from any browser on the network.
Log in as user **`Enclosure`** with `ENC_API_TOKEN` as the password. The page shows
the live gains, the chamber temperature, and whether what is running still
matches what is in NVS - so it is obvious when a tuning is one reboot away from
being lost. See `lib/EnclosureUi/README.md`.

#### Over HTTP

Both the page and the API use HTTP Basic auth with `ENC_API_TOKEN` as the
password. The username is **not** ignored - it is part of the credential, and it
differs by port: **`Enclosure`** on 8082 (the UI's own server) and **`cyd`** on
8081 (the enclosure API the CYD falls back to). The PID routes below exist on
**both** ports.

```bash
C3="http://<c3-host>:8081"
AUTH="cyd:$ENC_API_TOKEN"               # port 8081: the CYD's username
JSON='Content-Type: application/json'   # required - see the note below

curl -u "$AUTH" $C3/api/pid                                     # current gains
curl -u "$AUTH" -H "$JSON" -X POST -d '{"kp":6.0,"ki":0.1,"kd":0.0}' $C3/api/pid
curl -u "$AUTH" -X POST $C3/api/pid/save                       # persist to NVS
curl -u "$AUTH" -X POST $C3/api/pid/defaults                   # compiled-in defaults + save
```

`POST /api/pid` applies the gains immediately in RAM but does **not** persist
them - a reboot without a `save` returns to whatever is stored. A value outside
the range in `enclosure_pid.h` is rejected with a 400 and the running gains are
left untouched, so a typo cannot wedge the regulator mid-print.

> **The `Content-Type: application/json` header is not optional on a `POST` that
> carries a body.** `curl -d` alone sends `application/x-www-form-urlencoded`,
> which makes `WebServer` parse the body as form fields; a JSON string has no
> `=` in it, so the body is discarded before the handler ever sees it and the
> request fails with `bad json`. The two bodyless `POST`s above need no header.
> The web UI always sets it.

Tuning advice: the controller drives the louvre over the lower half of its output
span and the fan over the upper half, so size `kp` against a realistic chamber
excursion (0-15 C), not the full span. `kd` defaults to 0 because the controller
samples on a zero-order hold from a 5 s OctoPrint poll, where a derivative term
amplifies sensor noise rather than adding damping.

#### PID auto-calibration

The web UI's **Calibrate** button derives the three gains from an experiment
instead of guesswork. It is a relay-feedback auto-tuner, ported from ESPHome's
`PIDAutotuner` (`esphome/components/pid/pid_autotuner.cpp`), with the control
direction adapted to a cooling-only actuator.

Why a relay: replace the PID with a two-state controller that switches between
`output_positive = 200` (full cooling) and `output_negative = 0` (no cooling)
when the chamber error crosses `±noiseband` (0.5 °C). A stable limit cycle
appears; its amplitude `a` and period `Tu` are the process's describing-function
data at that operating point. The ultimate gain is

    Ku = 4d / (pi * a),   d = (200 - 0) / 2 = 100

and Ziegler-Nichols gives

    kp = 0.6 * Ku
    ki = 1.2 * Ku / Tu
    kd = 0.075 * Ku * Tu

The detector details that matter:

- **Zero crossings** use a band of `noiseband / 4` (0.125 °C), so sensor noise
  does not create extra crossings. The period is twice the mean interval between
  crossings.
- **Amplitude** is measured per relay phase (the peak-to-peak of the chamber
  error during that phase), the first phase is discarded, and the newest seven
  phases are averaged. A phase is recorded when the relay changes state, because
  the process lags the actuator.
- **Quality gates**: the half-periods must be within a factor of 1.5 of each
  other (symmetry), and the mean amplitude is compared with the global envelope
  (convergence). The symmetry gate is the meaningful one. The convergence test is
  ported exactly as ESPHome writes it - `(mean - global) / global < 0.05` - and
  because the mean of a subset of the phase extremes cannot exceed the global
  envelope, it does not fail in practice. A failed gate does not abort or retry:
  the result is computed anyway and carries a warning, which the result dialog
  shows. (ESPHome's six-phase grace is unreachable in its source too: the first
  failure sets the phase and falls through to the calculation.)
- The relay and the detectors run on the 1 Hz regulator tick, but the chamber
  value is held for the 5 s OctoPrint poll interval. That quantisation is much
  smaller than the oscillation period and amplitude the experiment produces.

Operating point: the bed is preheated to 90 °C to make the heat load repeatable,
and the chamber setpoint is **40 °C**. That is deliberately below the 50 °C
abort and the 55 °C emergency trigger, because the no-cooling half of the relay
lets the chamber rise. The relay span is the full 0-200 output: louvre 0 / fan
off, versus louvre 100 / fan 100. The 20-minute experiment window starts when the
chamber first reaches 40 °C, so the preheat and the initial climb do not consume
it; an absolute 30-minute cap is the backstop for a chamber that never gets
there. The run is aborted above 50 °C, on bed timeout, on stale OctoPrint data,
if a print starts, or on cancel; the bed is switched off and the normal
full-cooling state is entered on every exit path.

`kd` from the formula is usually 300-1500, well above `PID_KD_MAX = 50`, so it
is clamped and the result dialog warns. `kp` can clamp too when the measured
amplitude is small, and `ki` can approach `PID_KI_MAX` on a fast oscillation.
The clamped values are still the tuner's result within the accepted ranges, not
arbitrary substitutes. Given the derivative-noise caveat above, treat a
calibration result as a starting point: **Apply**, then compare it with a
`kd = 0` run before **Save to NVS**.

The algorithm lives in `include/pid_calibration.h` / `src/pid_calibration.cpp`;
the state machine (preheat, settle, relay, abort, apply) is in
`TemperatureRegulation`. `test/pid_calibration_sim.py` models the tuner against
first-order processes with transport delay and checks the measured period and
amplitude against the simulated process, the ZN formulas, and the clamp ranges.
It does not execute the C++ (there is no host C++ toolchain here); the firmware
build covers compilation.

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
- **Confirm it is not a duty-floor problem.** If the fan is commanded to 0% it is
  genuinely off, and any request from 1% up is raised to `FAN_MIN_RUN_PCT` (60%).
  A fan that will not start is usually below the floor, not broken — see
  [Fan Control](#fan-control). If it will not start *at* 60% or above, the break-away
  boost (`FAN_START_BOOST_PCT`/`_MS`) is the thing to check.
- **If the louvre servo stopped moving too**, the fan and servo have been put on
  the same LEDC timer. Check that the fan is still on channel 4 and the servo on
  channel 0.

### Fan Whines or Resonates

- **Whine (steady tone):** PWM switching noise. `FAN_PWM_FREQ` must be above
  ~20 kHz; 5 kHz is audible and is what caused the original whine.
- **Resonance (tone that changes with speed, or visible shaking):** the rotor is
  spinning unevenly under partial drive. Raise `FAN_MIN_RUN_PCT` so the fan never
  operates in that band rather than lowering it.

### Chamber Not Cooling

- Verify chamber temperature sensor is working (OctoPrint reports data)
- Check physical fan connections and airflow
- Monitor PID output values in serial output
- Adjust PID gains if needed - see [Tuning PID Controller](#tuning-pid-controller)

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
- **Fan PWM:** 25 kHz, requested. The driver picks the nearest integer divider from
  the 80 MHz APB clock, so this lands at ~24.0 kHz (80e6 / (256 × 13)) — above
  the 20 kHz top of human hearing. This was previously 5 kHz, which is *inside*
  the audible band and produced a tonal whine on this fan; the earlier claim that
  5 kHz was inaudible was simply wrong.

## Future Enhancements

Potential improvements:

1. Add EEPROM settings storage for user-adjustable thresholds
2. Implement hysteresis to prevent oscillation
3. Add temperature logging to SD card
4. Web interface for real-time monitoring
5. Support for multiple fans with individual control
6. Adaptive PID tuning based on material
