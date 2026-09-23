#ifndef ENCLOSURE_CONST_H
#define ENCLOSURE_CONST_H

#define SERVO_PIN 0
#define SERVO_PWM_FREQ 50
#define SERVO_MIN 500
#define SERVO_MAX 2400
#define SERVO_OFF_ANGLE 0
#define SERVO_ON_ANGLE 130
// Minimum time between physical servo moves (replaces blocking delay).
#define SERVO_MIN_MOVE_INTERVAL_MS 500

// Fan Control (PWM)
#define FAN_PIN 1
#define FAN_PWM_FREQ 5000 // 5kHz PWM frequency
#define FAN_PWM_RESOLUTION 8

// Temperature Regulation - PLA
#define TEMP_PLA_NOZZLE_TARGET 215
#define TEMP_PLA_NOZZLE_TOLERANCE 10 // 215±10°C (fallback only when filament name unknown)
#define TEMP_PLA_CHAMBER_TARGET 30
#define TEMP_PLA_BED_COOLDOWN 30
#define PLA_POSTPRINT_MIN_MS 60000UL      // keep running at least 60s after print ends
#define PLA_COOLDOWN_MAX_MS (30UL * 60UL * 1000UL) // failsafe: leave PLA state after 30min

// Temperature Regulation - PETG
#define TEMP_PETG_CHAMBER_THRESHOLD 35
#define TEMP_PETG_CHAMBER_TARGET 30
#define PETG_POSTPRINT_MS 30000UL
#define PETG_COOLDOWN_MAX_MS (20UL * 60UL * 1000UL) // failsafe: leave PETG state after 20min

// Temperature Regulation - ASA/ABS/Others
// Hysteresis band: open louvre at >= 40, run fan above 45,
// close again below 38 to avoid 1Hz flapping.
#define TEMP_ASA_ENCLOSURE_THRESHOLD 40 // open louvre at/above this
#define TEMP_ASA_CHAMBER_TARGET 45      // PID setpoint + fan threshold
#define TEMP_ASA_CLOSE_THRESHOLD 38     // close louvre below this
#define TEMP_ASA_FAN_SCALE 0.3f         // 30% of normal fan speed (gentle)
#define ASA_MAX_MS (3UL * 60UL * 60UL * 1000UL) // failsafe: leave ASA state after 3h

// PID Parameters for Chamber Temperature Control (cooling: error = current - setpoint)
#define PID_KP 1.5
#define PID_KI 0.1
#define PID_KD 0.5
#define PID_SETPOINT 30
#define PID_UPDATE_INTERVAL 1000 // 1 second

#endif // ENCLOSURE_CONST_H
