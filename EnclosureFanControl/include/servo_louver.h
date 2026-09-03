#ifndef SERVO_LOUVER_H
#define SERVO_LOUVER_H

#include <ESP32Servo.h>

class ServoLouver
{
private:
    Servo servo;
    int currentAngle;

public:
    ServoLouver();

    // Initialize the servo
    void begin();

    // Move servo to target angle if it differs from current
    void setAngle(int targetAngle);

    // Get current angle
    int getAngle() const;

    // Update servo angle based on printer printing state
    void updateForPrinter(bool isPrinting, String filamentName);
};

#endif
