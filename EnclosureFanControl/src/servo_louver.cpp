#include "servo_louver.h"
#include "const.h"

ServoLouver::ServoLouver() : currentAngle(-1), lastMoveMs(0)
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
    currentAngle = -1; // force first move
    lastMoveMs = 0;
    setAngle(SERVO_OFF_ANGLE);
}

void ServoLouver::setAngle(int targetAngle)
{
    if (targetAngle == currentAngle)
        return;
    // Rate-limit physical moves instead of blocking with delay().
    unsigned long now = millis();
    if (currentAngle != -1 && (now - lastMoveMs) < SERVO_MIN_MOVE_INTERVAL_MS)
        return;
    servo.write(targetAngle);
    lastMoveMs = now;
    currentAngle = targetAngle;
}

int ServoLouver::getAngle() const
{
    return currentAngle;
}

void ServoLouver::updateForPrinter(bool isPrinting, String filamentName)
{
    // Deprecated: TemperatureRegulation owns the louvre now.
    // Kept (case-insensitive, non-blocking) so old callers don't break.
    String norm = filamentName;
    norm.trim();
    norm.toUpperCase();

    int targetAngle = SERVO_OFF_ANGLE;

    if (isPrinting && (norm.indexOf("PLA") >= 0 || norm.indexOf("PETG") >= 0))
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
