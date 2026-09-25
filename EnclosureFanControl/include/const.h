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
// IMPORTANT: LEDC channel/timer reservation (Arduino maps chan->timer fixed):
//   servo (ESP32Servo) -> channel 0, timer 0 @ 50 Hz
//   fan               -> channel 4, timer 2 @ 5 kHz
// Sharing a channel retunes the shared timer and kills the other output.
#define FAN_PIN 1
#define FAN_PWM_FREQ 5000 // 5kHz PWM frequency
#define FAN_PWM_RESOLUTION 8
#define FAN_PWM_CHANNEL 4

#define TEMP_STANDARD_TARGET 30
#define TEMP_TPU_TARGET 35
#define TEMP_HIGH_TEMP_TARGET 50
#define TEMP_FAN_COOLDOWN_END 30
#define TEMP_PASSIVE_COOLDOWN_START 35
#define TEMP_PASSIVE_COOLDOWN_END 30
#define TEMP_EMERGENCY_TRIGGER 55
#define TEMP_EMERGENCY_CLEAR 50
#define REMOTE_SETPOINT_MIN 20
#define REMOTE_SETPOINT_MAX 60

// ESP-NOW peer (PrinterDisplay CYD WiFi MAC). All zeros = receive-only
// until set. Both firmwares log their own MAC at boot - copy them over.
static const uint8_t ENC_PEER_MAC[6] = {0x5C, 0x01, 0x3B, 0x8A, 0x63, 0x54};

// PID Parameters for Chamber Temperature Control (cooling: error = current - setpoint)
#define PID_KP 1.5
#define PID_KI 0.1
#define PID_KD 0.5
#define PID_SETPOINT TEMP_STANDARD_TARGET
#define PID_UPDATE_INTERVAL 1000 // 1 second

// OctoPrint background poll interval (FreeRTOS task, CYD-style). Loop only
// copies the latest snapshot and never blocks on the network.
#define OCTO_POLL_INTERVAL_MS 5000 // 5 seconds

#endif // ENCLOSURE_CONST_H
