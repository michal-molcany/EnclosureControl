#include "servo_louver.h"
#include "const.h"

ServoLouver::ServoLouver() : currentAngle(-1)
{
}

void ServoLouver::begin()
{
    ESP32PWM::allocateTimer(0);
    ESP32PWM::allocateTimer(1);
    ESP32PWM::allocateTimer(2);
    ESP32PWM::allocateTimer(3);
    servo.setPeriodHertz(SERVO_PWM_FREQ);
    servo.attach(SERVO_PIN, SERVO_MIN, SERVO_MAX);
    setAngle(SERVO_OFF_ANGLE);
}

void ServoLouver::setAngle(int targetAngle)
{
    if (targetAngle != currentAngle)
    {
        servo.write(targetAngle);
        delay(SERVO_DELAY);
        currentAngle = targetAngle;
    }
}

int ServoLouver::getAngle() const
{
    return currentAngle;
}

void ServoLouver::updateForPrinter(bool isPrinting, String filamentName)
{
    int targetAngle = SERVO_OFF_ANGLE;

    if (isPrinting && (filamentName == "PLA" || filamentName == "PETG"))
    {
        targetAngle = SERVO_ON_ANGLE;
    }

    if (targetAngle != currentAngle)
    {
        setAngle(targetAngle);

        Serial.print("Printer is ");
        Serial.print(isPrinting ? "printing " : "idle");
        Serial.print(filamentName);
        Serial.print(", setting servo angle to ");
        Serial.println(targetAngle);
    }
}
