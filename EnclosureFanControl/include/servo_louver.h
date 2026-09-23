#ifndef SERVO_LOUVER_H
#define SERVO_LOUVER_H

#include <ESP32Servo.h>

class ServoLouver
{
private:
    Servo servo;
    int currentAngle;
    unsigned long lastMoveMs;

public:
    ServoLouver();

    // Initialize the servo
    void begin();

    // Move servo to target angle if it differs from current.
    // Non-blocking: rate-limited, no delay().
    void setAngle(int targetAngle);

    // Proportional 0-100% opening between SERVO_OFF_ANGLE and SERVO_ON_ANGLE.
    // Used by remote manual control.
    void setPercent(int percent);

    // Power-on self-test: opens to 25% and closes again to verify the servo.
    // Blocking (~2s), call once from setup().
    void selfTest();

    // Get current angle
    int getAngle() const;

    // Legacy helper kept for compatibility. TemperatureRegulation now owns
    // the louvre - do NOT call this alongside TemperatureRegulation::update()
    // or the two controllers will fight over the servo.
    // Now case-insensitive and non-blocking.
    void updateForPrinter(bool isPrinting, String filamentName);
};

#endif
