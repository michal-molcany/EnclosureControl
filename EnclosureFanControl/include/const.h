#define SERVO_PIN 0
#define SERVO_PWM_FREQ 50
#define SERVO_MIN 500
#define SERVO_MAX 2400
#define SERVO_OFF_ANGLE 0
#define SERVO_ON_ANGLE 130
#define SERVO_DELAY 200

// Fan Control (PWM)
#define FAN_PIN 1
#define FAN_PWM_FREQ 5000 // 5kHz PWM frequency

// Temperature Regulation - PLA
#define TEMP_PLA_NOZZLE_TARGET 215
#define TEMP_PLA_NOZZLE_TOLERANCE 10 // 215±10°C
#define TEMP_PLA_CHAMBER_TARGET 30
#define TEMP_PLA_BED_COOLDOWN 30

// Temperature Regulation - PETG
#define TEMP_PETG_CHAMBER_THRESHOLD 35
#define TEMP_PETG_CHAMBER_TARGET 30

// Temperature Regulation - ASA/ABS/Others
#define TEMP_ASA_ENCLOSURE_THRESHOLD 40
#define TEMP_ASA_CHAMBER_TARGET 45
#define TEMP_ASA_COOLING_RATE 5 // Very slow cooling

// PID Parameters for Chamber Temperature Control
#define PID_KP 1.5
#define PID_KI 0.1
#define PID_KD 0.5
#define PID_SETPOINT 30
#define PID_UPDATE_INTERVAL 1000 // 1 second