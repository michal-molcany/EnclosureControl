#include "temperature_regulation.h"
#include "const.h"
#include "servo_louver.h"

// ==================== PIDController Implementation ====================

PIDController::PIDController(float _kp, float _ki, float _kd, float _setpoint)
    : kp(_kp), ki(_ki), kd(_kd), setpoint(_setpoint),
      lastError(0), integral(0), lastTime(millis())
{
}

float PIDController::calculate(float currentValue)
{
    unsigned long currentTime = millis();
    float timeStep = (currentTime - lastTime) / 1000.0; // Convert to seconds
    lastTime = currentTime;

    if (timeStep <= 0)
        return 0;

    float error = setpoint - currentValue;
    integral += error * timeStep;

    // Anti-windup: limit integral
    integral = constrain(integral, -100, 100);

    float derivative = (error - lastError) / timeStep;
    lastError = error;

    float output = kp * error + ki * integral + kd * derivative;
    return constrain(output, 0.0, 255.0); // Output for PWM (0-255)
}

void PIDController::setSetpoint(float newSetpoint)
{
    setpoint = newSetpoint;
}

void PIDController::reset()
{
    lastError = 0;
    integral = 0;
    lastTime = millis();
}

// ==================== TemperatureRegulation Implementation ====================

TemperatureRegulation::TemperatureRegulation(OctoPrinter *octoPrinter, ServoLouver *servoLouver)
    : printer(octoPrinter), louver(servoLouver), pidController(PID_KP, PID_KI, PID_KD, PID_SETPOINT),
      fanActive(false), louvreOpen(false), lastUpdate(0), printEndTime(0),
      printWasActive(false), currentState(IDLE)
{
}

void TemperatureRegulation::begin()
{
    pinMode(FAN_PIN, OUTPUT);
    ledcAttach(FAN_PIN, FAN_PWM_FREQ, 8); // 8-bit resolution (0-255)
    stopFan();

    Serial.println("TemperatureRegulation initialized");
}

void TemperatureRegulation::update()
{
    // Prevent too frequent updates
    if (millis() - lastUpdate < 1000)
        return;
    lastUpdate = millis();

    // Check if print just ended
    if (printWasActive && !printer->printing())
    {
        printWasActive = false;
        printEndTime = millis();
        Serial.println("Print ended - starting cooldown sequence");
    }

    if (printer->printing())
    {
        printWasActive = true;
    }

    // Execute material-specific logic
    switch (currentState)
    {
    case IDLE:
        if (isPLATrigger())
        {
            transitionToState(PLA_COOLING);
        }
        else if (isPETGTrigger())
        {
            transitionToState(PETG_COOLING);
        }
        else if (isASATrigger())
        {
            transitionToState(ASA_ABS_COOLING);
        }
        break;

    case PLA_COOLING:
        updatePLALogic();
        break;

    case PETG_COOLING:
        updatePETGLogic();
        break;

    case ASA_ABS_COOLING:
        updateASALogic();
        break;
    }
}

// ==================== Material-Specific Logic ====================

void TemperatureRegulation::updatePLALogic()
{
    // PLA: Open louvre and start Fan on PIN 1 with PID regulation of chamber to 30°C
    // Hold until after print bed temperature is 30°C, then stop fan and close louvre

    double chamberTemp = printer->chamberActual();
    double bedTemp = printer->bedActual();
    bool isPrinting = printer->printing();

    // During printing or shortly after
    if (isPrinting || (millis() - printEndTime < 60000)) // Continue for 60 seconds after print ends
    {
        // Open louvre if not already
        if (!louvreOpen)
        {
            openLouvre();
        }

        // Apply PID control to maintain chamber at 30°C
        uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp);
        setFanSpeed(fanSpeed);

        Serial.print("[PLA] Chamber: ");
        Serial.print(chamberTemp);
        Serial.print("°C, Bed: ");
        Serial.print(bedTemp);
        Serial.print("°C, Fan speed: ");
        Serial.println(fanSpeed);
    }
    else if (bedTemp <= TEMP_PLA_BED_COOLDOWN)
    {
        // Bed cooled down, stop cooling
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
        Serial.println("[PLA] Cooldown complete - returning to IDLE");
    }
}

void TemperatureRegulation::updatePETGLogic()
{
    // PETG: Open louvre and start cooling only if temperature of chamber is 35°C
    // and after end of print for faster cooling

    double chamberTemp = printer->chamberActual();
    bool isPrinting = printer->printing();

    // Only cool if chamber is hot enough and print is done
    if (!isPrinting && chamberTemp >= TEMP_PETG_CHAMBER_THRESHOLD)
    {
        if (!louvreOpen)
        {
            openLouvre();
        }

        // Cool down slowly to 30°C
        if (chamberTemp > TEMP_PETG_CHAMBER_TARGET)
        {
            uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp);
            setFanSpeed(fanSpeed);

            Serial.print("[PETG] Cooling from ");
            Serial.print(chamberTemp);
            Serial.print("°C to target ");
            Serial.print(TEMP_PETG_CHAMBER_TARGET);
            Serial.print("°C, Fan speed: ");
            Serial.println(fanSpeed);
        }
        else
        {
            // Target reached
            stopFan();
            closeLouvre();
            transitionToState(IDLE);
            Serial.println("[PETG] Chamber cooled to target - returning to IDLE");
        }
    }
    else if (isPrinting || (chamberTemp < TEMP_PETG_CHAMBER_THRESHOLD))
    {
        // Still printing or chamber not hot enough, stay in this state
        // but don't run fan
        stopFan();
    }
}

void TemperatureRegulation::updateASALogic()
{
    // ASA/ABS/Others: Open louvre only if enclosure temperature reaches 40°C
    // Start cooling very slowly to 45°C
    // Never open for other reasons (to prevent warping)

    double chamberTemp = printer->chamberActual();
    bool isPrinting = printer->printing();

    // Only act if chamber reaches critical temperature
    if (chamberTemp >= TEMP_ASA_ENCLOSURE_THRESHOLD)
    {
        if (!louvreOpen)
        {
            openLouvre();
            Serial.println("[ASA/ABS] Enclosure temperature critical - opening louvre");
        }

        // Very slow cooling - use lower PID gains
        if (chamberTemp > TEMP_ASA_CHAMBER_TARGET)
        {
            // Use a much gentler PID output (scale down)
            uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp) * 0.3; // 30% of normal fan speed
            setFanSpeed(fanSpeed);

            Serial.print("[ASA/ABS] Slow cooling from ");
            Serial.print(chamberTemp);
            Serial.print("°C, Fan speed (slow): ");
            Serial.println(fanSpeed);
        }
        else
        {
            // Target reached, stop cooling
            stopFan();
            closeLouvre();
            transitionToState(IDLE);
            Serial.println("[ASA/ABS] Target temperature reached - returning to IDLE");
        }
    }
    else
    {
        // Chamber not hot, stop fan and close
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
    }
}

// ==================== Trigger Detection ====================

bool TemperatureRegulation::isPLATrigger()
{
    // Trigger: PLA printing OR nozzle temp is 215±10°C
    String material = getMaterialType();
    double nozzleTemp = printer->toolActual();
    bool isPrinting = printer->printing();

    bool isPLAMaterial = (material.equalsIgnoreCase("PLA"));
    bool isNozzleTempInRange = (nozzleTemp >= (TEMP_PLA_NOZZLE_TARGET - TEMP_PLA_NOZZLE_TOLERANCE) &&
                                nozzleTemp <= (TEMP_PLA_NOZZLE_TARGET + TEMP_PLA_NOZZLE_TOLERANCE));

    return isPrinting && (isPLAMaterial || isNozzleTempInRange);
}

bool TemperatureRegulation::isPETGTrigger()
{
    // Trigger: PETG printing or just ended, and chamber is warm enough
    String material = getMaterialType();
    double chamberTemp = printer->chamberActual();
    bool isPrinting = printer->printing();

    bool isPETGMaterial = (material.equalsIgnoreCase("PETG"));
    bool isPostPrint = !isPrinting && (millis() - printEndTime < 30000); // 30 seconds after print

    return isPETGMaterial && (isPrinting || isPostPrint);
}

bool TemperatureRegulation::isASATrigger()
{
    // Trigger: ASA/ABS/Others material being printed
    String material = getMaterialType();
    bool isPrinting = printer->printing();

    bool isHighTempMaterial = (material.equalsIgnoreCase("ASA") ||
                               material.equalsIgnoreCase("ABS") ||
                               (!material.equalsIgnoreCase("PLA") && !material.equalsIgnoreCase("PETG")));

    return isPrinting && isHighTempMaterial;
}

// ==================== Helper Functions ====================

String TemperatureRegulation::getMaterialType()
{
    return printer->filamentName();
}

uint8_t TemperatureRegulation::getCurrentFanSpeed(float chamberTemp)
{
    // Use PID controller to calculate fan speed based on chamber temperature
    float pidOutput = pidController.calculate(chamberTemp);
    return (uint8_t)pidOutput;
}

void TemperatureRegulation::setFanSpeed(uint8_t speed)
{
    if (!fanActive || speed > 0)
    {
        ledcWrite(FAN_PIN, speed);
        fanActive = (speed > 0);
    }
}

void TemperatureRegulation::stopFan()
{
    setFanSpeed(0);
    pidController.reset();
    fanActive = false;
}

void TemperatureRegulation::openLouvre()
{
    if (!louvreOpen && louver)
    {
        louver->setAngle(SERVO_ON_ANGLE);
        louvreOpen = true;
        Serial.println("Louvre opened");
    }
}

void TemperatureRegulation::closeLouvre()
{
    if (louvreOpen && louver)
    {
        louver->setAngle(SERVO_OFF_ANGLE);
        louvreOpen = false;
        Serial.println("Louvre closed");
    }
}

void TemperatureRegulation::transitionToState(MaterialState newState)
{
    if (newState != currentState)
    {
        currentState = newState;
        pidController.reset();

        Serial.print("State transition: ");
        Serial.println(getCurrentState());
    }
}

// ==================== Status Functions ====================

bool TemperatureRegulation::isFanActive() const
{
    return fanActive;
}

bool TemperatureRegulation::isLouvreOpen() const
{
    return louvreOpen;
}

String TemperatureRegulation::getCurrentState() const
{
    switch (currentState)
    {
    case IDLE:
        return "IDLE";
    case PLA_COOLING:
        return "PLA_COOLING";
    case PETG_COOLING:
        return "PETG_COOLING";
    case ASA_ABS_COOLING:
        return "ASA_ABS_COOLING";
    default:
        return "UNKNOWN";
    }
}
