#include "temperature_regulation.h"
#include "const.h"
#include "servo_louver.h"

// ==================== PWM compat (Arduino-ESP32 v2 vs v3) ====================

static void fanPwmInit()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
#else
    ledcSetup(0, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcAttachPin(FAN_PIN, 0);
#endif
}

static void fanPwmWrite(uint8_t value)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(FAN_PIN, value);
#else
    ledcWrite(0, value);
#endif
}

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

    // Cooling: positive error (too hot) -> positive output (fan on).
    // Previously this was (setpoint - current), which ran the fan when cold.
    float error = currentValue - setpoint;

    // Only integrate positive (cooling demand) error to avoid windup while cold.
    if (error > 0)
        integral += error * timeStep;
    else
        integral *= 0.9f; // decay stale integral when at/below target

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
      fanActive(false), lastFanSpeed(0), louvreOpen(false), lastUpdate(0), printEndTime(0),
      printEndValid(false), printWasActive(false), lastMaterial(""),
      invalidSince(0), currentState(IDLE), stateEnteredAt(0)
{
}

void TemperatureRegulation::begin()
{
    pinMode(FAN_PIN, OUTPUT);
    fanPwmInit();
    fanPwmWrite(0);
    fanActive = false;
    lastFanSpeed = 0;
    stateEnteredAt = millis();

    Serial.println("TemperatureRegulation initialized");
}

void TemperatureRegulation::update()
{
    // Prevent too frequent updates
    if (millis() - lastUpdate < PID_UPDATE_INTERVAL)
        return;
    lastUpdate = millis();

    bool isPrinting = printer->printing();

    // Latch material while printing for post-print windows (filament may clear after end).
    if (isPrinting)
    {
        String norm = normalizeMaterial(printer->filamentName());
        if (norm.length() > 0)
            lastMaterial = norm;
        printWasActive = true;
    }

    // Check if print just ended
    if (printWasActive && !isPrinting)
    {
        printWasActive = false;
        printEndTime = millis();
        printEndValid = true;
        Serial.println("Print ended - starting cooldown sequence");
    }

    if (!isDataValid())
    {
        if (invalidSince == 0)
        {
            invalidSince = millis();
            Serial.println("[WARN] OctoPrint data invalid - holding state (fan off)");
        }
        stopFan();
        return;
    }
    invalidSince = 0;

    String normMaterial = normalizeMaterial(isPrinting ? printer->filamentName() : lastMaterial);
    // Fall back to live name if latched copy is empty (e.g. boot mid-print).
    if (normMaterial.length() == 0)
        normMaterial = normalizeMaterial(printer->filamentName());

    // Material-change preemption: switch directly between cooling states.
    if (currentState != IDLE && isPrinting)
    {
        if (currentState != PLA_COOLING && isPLATrigger(normMaterial))
            transitionToState(PLA_COOLING);
        else if (currentState != PETG_COOLING && isPETGTrigger(normMaterial))
            transitionToState(PETG_COOLING);
        else if (currentState != ASA_ABS_COOLING && isASATrigger(normMaterial))
            transitionToState(ASA_ABS_COOLING);
    }

    // Execute material-specific logic
    switch (currentState)
    {
    case IDLE:
        if (isPrinting)
        {
            if (isPLATrigger(normMaterial))
                transitionToState(PLA_COOLING);
            else if (isPETGTrigger(normMaterial))
                transitionToState(PETG_COOLING);
            else if (isASATrigger(normMaterial))
                transitionToState(ASA_ABS_COOLING);
        }
        else
        {
            // Post-print PETG cooling can start after the print ended.
            if (isPETGTrigger(normMaterial))
                transitionToState(PETG_COOLING);
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
    // PLA: open louvre and regulate chamber to 30°C with PID.
    // Run while printing; after print keep running until the bed cools
    // to TEMP_PLA_BED_COOLDOWN or the failsafe timeout expires.

    double chamberTemp = printer->chamberActual();
    double bedTemp = printer->bedActual();
    bool isPrinting = printer->printing();

    if (isPrinting)
    {
        if (!louvreOpen)
            openLouvre();
        uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp);
        setFanSpeed(fanSpeed);

        Serial.print("[PLA] Chamber: ");
        Serial.print(chamberTemp);
        Serial.print("°C, Bed: ");
        Serial.print(bedTemp);
        Serial.print("°C, Fan speed: ");
        Serial.println(fanSpeed);
        return;
    }

    // Not printing anymore.
    bool pastMinWindow = !printEndValid || (millis() - printEndTime >= PLA_POSTPRINT_MIN_MS);
    bool timedOut = (millis() - stateEnteredAt) >= PLA_COOLDOWN_MAX_MS;

    if (bedTemp <= TEMP_PLA_BED_COOLDOWN || timedOut)
    {
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
        Serial.println(timedOut ? "[PLA] Failsafe timeout - returning to IDLE"
                                : "[PLA] Cooldown complete - returning to IDLE");
        return;
    }

    // Bed still hot (or within minimum post-print window): keep cooling.
    (void)pastMinWindow; // window is a minimum; bed temperature governs the exit
    if (!louvreOpen)
        openLouvre();
    uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp);
    setFanSpeed(fanSpeed);

    Serial.print("[PLA] Post-print Chamber: ");
    Serial.print(chamberTemp);
    Serial.print("°C, Bed: ");
    Serial.print(bedTemp);
    Serial.print("°C, Fan speed: ");
    Serial.println(fanSpeed);
}

void TemperatureRegulation::updatePETGLogic()
{
    // PETG: during print keep louvre open but fan off (avoid warping / stringing).
    // After print, cool only if chamber >= threshold, down to target.

    double chamberTemp = printer->chamberActual();
    bool isPrinting = printer->printing();

    if (isPrinting)
    {
        if (!louvreOpen)
            openLouvre();
        stopFan();
        return;
    }

    bool timedOut = (millis() - stateEnteredAt) >= PETG_COOLDOWN_MAX_MS;

    if (timedOut)
    {
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
        Serial.println("[PETG] Failsafe timeout - returning to IDLE");
        return;
    }

    if (chamberTemp >= TEMP_PETG_CHAMBER_THRESHOLD && chamberTemp > TEMP_PETG_CHAMBER_TARGET)
    {
        if (!louvreOpen)
            openLouvre();

        uint8_t fanSpeed = getCurrentFanSpeed(chamberTemp);
        setFanSpeed(fanSpeed);

        Serial.print("[PETG] Cooling from ");
        Serial.print(chamberTemp);
        Serial.print("°C to target ");
        Serial.print(TEMP_PETG_CHAMBER_TARGET);
        Serial.print("°C, Fan speed: ");
        Serial.println(fanSpeed);
    }
    else if (chamberTemp <= TEMP_PETG_CHAMBER_TARGET)
    {
        // Target reached (or chamber was never hot): done.
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
        Serial.println("[PETG] Chamber cooled to target - returning to IDLE");
    }
    else
    {
        // Between target and threshold: hold louvre open, fan off.
        if (!louvreOpen)
            openLouvre();
        stopFan();
    }
}

void TemperatureRegulation::updateASALogic()
{
    // ASA/ABS: keep chamber warm. Louvre opens at >= threshold, fan runs
    // gently only above target, everything closes below close-threshold.
    // While still printing we stay in this state (no flapping to IDLE).

    double chamberTemp = printer->chamberActual();
    bool isPrinting = printer->printing();

    bool timedOut = (millis() - stateEnteredAt) >= ASA_MAX_MS;
    if (timedOut)
    {
        stopFan();
        closeLouvre();
        transitionToState(IDLE);
        Serial.println("[ASA/ABS] Failsafe timeout - returning to IDLE");
        return;
    }

    if (chamberTemp >= TEMP_ASA_ENCLOSURE_THRESHOLD)
    {
        if (!louvreOpen)
        {
            openLouvre();
            Serial.println("[ASA/ABS] Enclosure temperature high - opening louvre");
        }

        if (chamberTemp > TEMP_ASA_CHAMBER_TARGET)
        {
            // Gentle cooling: PID against the ASA setpoint, scaled down.
            uint8_t fanSpeed = (uint8_t)(getCurrentFanSpeed(chamberTemp) * TEMP_ASA_FAN_SCALE);
            setFanSpeed(fanSpeed);

            Serial.print("[ASA/ABS] Slow cooling from ");
            Serial.print(chamberTemp);
            Serial.print("°C, Fan speed (slow): ");
            Serial.println(fanSpeed);
        }
        else
        {
            stopFan(); // warm enough to vent passively, no forced cooling
        }
        return;
    }

    // Below open threshold.
    if (chamberTemp <= TEMP_ASA_CLOSE_THRESHOLD || !isPrinting)
    {
        stopFan();
        closeLouvre();
        if (!isPrinting)
        {
            transitionToState(IDLE);
            Serial.println("[ASA/ABS] Print done and chamber normal - returning to IDLE");
        }
    }
    else
    {
        // Hysteresis band (38-40°C): hold position, fan off.
        stopFan();
    }
}

// ==================== Trigger Detection ====================

bool TemperatureRegulation::isPLATrigger(const String &normMaterial)
{
    bool isPrinting = printer->printing();
    if (!isPrinting)
        return false;

    if (materialContains(normMaterial, "PLA"))
        return true;

    // Fallback: unknown/empty filament name + nozzle in PLA range.
    if (normMaterial.length() == 0 || normMaterial == "UNKNOWN" || normMaterial == "UNKN")
    {
        double nozzleTemp = printer->toolActual();
        return (nozzleTemp >= (TEMP_PLA_NOZZLE_TARGET - TEMP_PLA_NOZZLE_TOLERANCE) &&
                nozzleTemp <= (TEMP_PLA_NOZZLE_TARGET + TEMP_PLA_NOZZLE_TOLERANCE));
    }
    return false;
}

bool TemperatureRegulation::isPETGTrigger(const String &normMaterial)
{
    bool isPrinting = printer->printing();

    if (isPrinting)
        return materialContains(normMaterial, "PETG");

    // Post-print: latched PETG material within window, or still hot chamber
    // shortly after any print (covers filament name clearing on job end).
    if (!printEndValid)
        return false;
    unsigned long sinceEnd = millis() - printEndTime;
    bool petgPostPrint = materialContains(normMaterial, "PETG") || materialContains(lastMaterial, "PETG");
    if (petgPostPrint && sinceEnd < (PETG_POSTPRINT_MS + PETG_COOLDOWN_MAX_MS))
    {
        double chamberTemp = printer->chamberActual();
        return chamberTemp >= TEMP_PETG_CHAMBER_THRESHOLD;
    }
    return false;
}

bool TemperatureRegulation::isASATrigger(const String &normMaterial)
{
    bool isPrinting = printer->printing();
    if (!isPrinting)
        return false;

    if (materialContains(normMaterial, "ASA") || materialContains(normMaterial, "ABS"))
        return true;

    // Explicitly handled materials that are NOT high-temp.
    if (materialContains(normMaterial, "PLA") || materialContains(normMaterial, "PETG"))
        return false;

    // Known high-temp / engineering materials default to ASA behaviour.
    static const char *highTemp[] = {"TPU", "PC", "NYLON", "PA", "HIPS", "PVA", "WOOD", "FLEX", "PP", "PPS", "PEI", "ULTEM"};
    for (const char *m : highTemp)
    {
        if (materialContains(normMaterial, m))
            return true;
    }

    // Empty/unknown while printing: stay conservative (louvre closed unless hot).
    return (normMaterial.length() == 0 || normMaterial == "UNKNOWN" || normMaterial == "UNKN");
}

// ==================== Helper Functions ====================

String TemperatureRegulation::getMaterialType()
{
    return printer->filamentName();
}

String TemperatureRegulation::normalizeMaterial(const String &raw)
{
    String s = raw;
    s.trim();
    s.toUpperCase();
    return s;
}

bool TemperatureRegulation::materialContains(const String &norm, const char *token)
{
    if (norm.length() == 0 || token == nullptr)
        return false;
    String t(token);
    return norm.indexOf(t) >= 0;
}

bool TemperatureRegulation::isDataValid()
{
    if (printer->closedOrError())
        return false;
    // All-zero readings mean no successful poll yet / printer unreachable.
    if (printer->toolActual() == 0 && printer->bedActual() == 0 && printer->chamberActual() == 0)
        return false;
    return true;
}

uint8_t TemperatureRegulation::getCurrentFanSpeed(float chamberTemp)
{
    // Use PID controller to calculate fan speed based on chamber temperature
    float pidOutput = pidController.calculate(chamberTemp);
    return (uint8_t)pidOutput;
}

void TemperatureRegulation::setFanSpeed(uint8_t speed)
{
    // Always drive the pin so speed 0 actually stops the fan.
    // Skip redundant writes to reduce PWM churn.
    if (speed == lastFanSpeed && (fanActive == (speed > 0)))
        return;
    fanPwmWrite(speed);
    lastFanSpeed = speed;
    fanActive = (speed > 0);
}

void TemperatureRegulation::stopFan()
{
    if (!fanActive && lastFanSpeed == 0)
        return;
    fanPwmWrite(0);
    lastFanSpeed = 0;
    fanActive = false;
    pidController.reset();
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
        stateEnteredAt = millis();
        pidController.reset();
        // Per-material PID setpoints (previously fixed at 30 for all states).
        switch (newState)
        {
        case PLA_COOLING:
            pidController.setSetpoint(TEMP_PLA_CHAMBER_TARGET);
            break;
        case PETG_COOLING:
            pidController.setSetpoint(TEMP_PETG_CHAMBER_TARGET);
            break;
        case ASA_ABS_COOLING:
            pidController.setSetpoint(TEMP_ASA_CHAMBER_TARGET);
            break;
        case IDLE:
        default:
            pidController.setSetpoint(PID_SETPOINT);
            break;
        }

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
