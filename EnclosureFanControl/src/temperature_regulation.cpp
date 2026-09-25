#include "temperature_regulation.h"
#include "const.h"
#include "servo_louver.h"

static void fanPwmInit()
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcAttach(FAN_PIN, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
#else
    ledcSetup(FAN_PWM_CHANNEL, FAN_PWM_FREQ, FAN_PWM_RESOLUTION);
    ledcAttachPin(FAN_PIN, FAN_PWM_CHANNEL);
#endif
}

static void fanPwmWrite(uint8_t value)
{
#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR >= 3
    ledcWrite(FAN_PIN, value);
#else
    ledcWrite(FAN_PWM_CHANNEL, value);
#endif
}

static bool materialHasToken(const String &material, const char *token)
{
    if (material.length() == 0 || token == nullptr)
        return false;

    const int tokenLength = strlen(token);
    if (tokenLength == 0)
        return false;

    auto isLetter = [](char value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };

    for (int start = material.indexOf(token); start >= 0; start = material.indexOf(token, start + 1))
    {
        const int end = start + tokenLength;
        const bool leftBoundary = start == 0 || !isLetter(material[start - 1]);
        const bool rightBoundary = end >= material.length() || !isLetter(material[end]);
        if (leftBoundary && rightBoundary)
            return true;
    }
    return false;
}

PIDController::PIDController(float kpValue, float kiValue, float kdValue, float target)
    : kp(kpValue), ki(kiValue), kd(kdValue), setpoint(target),
      lastError(0), integral(0), lastTime(millis())
{
}

float PIDController::calculate(float currentValue)
{
    const unsigned long currentTime = millis();
    const float timeStep = (currentTime - lastTime) / 1000.0f;
    lastTime = currentTime;

    if (timeStep <= 0)
        return 0;

    const float error = currentValue - setpoint;
    if (error > 0)
        integral += error * timeStep;
    else
        integral *= 0.9f;

    integral = constrain(integral, 0.0f, 200.0f);

    const float derivative = (error - lastError) / timeStep;
    lastError = error;

    const float output = kp * error + ki * integral + kd * derivative;
    return constrain(output, 0.0f, 200.0f);
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

TemperatureRegulation::TemperatureRegulation(ServoLouver *servoLouver)
    : louver(servoLouver), pidController(PID_KP, PID_KI, PID_KD, PID_SETPOINT),
      fanActive(false), lastFanSpeed(0), louvreOpen(false), lastUpdate(0),
      invalidSince(0)
{
}

void TemperatureRegulation::begin()
{
    pinMode(FAN_PIN, OUTPUT);
    fanPwmInit();
    fanPwmWrite(0);
    fanActive = false;
    lastFanSpeed = 0;
    lastLouvrePct = 0;
    lastUpdate = millis();
    pidController.reset();
#if SERIAL_DEBUG
    Serial.println("TemperatureRegulation initialized");
#endif
}

void TemperatureRegulation::update(const OctoSnapshot &snap)
{
    const unsigned long now = millis();
    if (now - lastUpdate < PID_UPDATE_INTERVAL)
        return;
    lastUpdate = now;

    OctoSnapshot effective = snap;
    bool haveData = isDataValid(snap);
    if (haveData)
    {
        lastSnap = snap;
        hasGoodData = true;
        invalidSince = 0;
    }
    else if (hasGoodData)
    {
        if (invalidSince == 0)
        {
            invalidSince = now;
#if SERIAL_DEBUG
            Serial.println("[WARN] OctoPrint data invalid - using last-good snapshot");
#endif
        }
        effective = lastSnap;
        haveData = true;
    }

    const bool useRemote = remoteLinkAlive();
    const bool jobActive = useRemote
                               ? remoteJobActive
                               : haveData && (effective.printing || effective.paused);

    uint8_t material = ENC_MATERIAL_UNKNOWN;
    if (useRemote && hasRemoteMaterial)
        material = remoteMaterial;
    else if (haveData)
        material = classifyMaterial(effective.filament);

    if (jobActive)
    {
        lastMaterial = material;
        jobWasActive = true;
    }
    else if (jobWasActive)
    {
        jobWasActive = false;
        const MaterialState cooldownState =
            (lastMaterial == ENC_MATERIAL_PLA || lastMaterial == ENC_MATERIAL_PETG)
                ? COOLING_FAN
                : COOLING_PASSIVE;
        transitionToState(cooldownState);
#if SERIAL_DEBUG
        Serial.println("Print ended - starting cooldown sequence");
#endif
    }

    if (jobActive)
        transitionToState(ACTIVE_AUTO);

    if (haveData && updateEmergency(effective.chamberActual))
        return;

    if (controlMode == ENC_MODE_MANUAL)
    {
        if (useRemote)
        {
            updateManual();
            return;
        }

        controlMode = ENC_MODE_AUTO;
        pidController.reset();
#if SERIAL_DEBUG
        Serial.println("[LINK] Manual link lost - falling back to AUTO");
#endif
    }

    if (!haveData)
    {
        if (invalidSince == 0)
        {
            invalidSince = now;
#if SERIAL_DEBUG
            Serial.println("[WARN] OctoPrint data invalid - holding outputs closed");
#endif
        }
        closeOutputs();
        return;
    }

    if (currentState == COOLING_FAN)
    {
        if (effective.chamberActual < TEMP_FAN_COOLDOWN_END)
        {
            closeOutputs();
            transitionToState(IDLE);
#if SERIAL_DEBUG
            Serial.println("[COOLING] Fan cooldown complete");
#endif
        }
        else
        {
            setLouvrePercent(100);
            setFanPercent(100);
        }
        return;
    }

    if (currentState == COOLING_PASSIVE)
    {
        setFanPercent(0);
        if (effective.chamberActual < TEMP_PASSIVE_COOLDOWN_END)
        {
            setLouvrePercent(0);
            transitionToState(IDLE);
#if SERIAL_DEBUG
            Serial.println("[COOLING] Passive cooldown complete");
#endif
        }
        else if (effective.chamberActual < TEMP_PASSIVE_COOLDOWN_START)
        {
            setLouvrePercent(100);
        }
        else
        {
            setLouvrePercent(0);
        }
        return;
    }

    if (currentState != ACTIVE_AUTO)
    {
        closeOutputs();
        return;
    }

    updateAutomatic(effective.chamberActual);
}

bool TemperatureRegulation::updateEmergency(double chamberTemp)
{
    if (!emergencyActive && chamberTemp >= TEMP_EMERGENCY_TRIGGER)
    {
        emergencyActive = true;
        pidController.reset();
        if (!emergencyCancelRequested)
        {
            octoPollerRequestCancel();
            emergencyCancelRequested = true;
        }
        Serial.println("[EMERGENCY] 55C reached - cancelling print and forcing maximum cooling");
    }

    if (emergencyActive && chamberTemp < TEMP_EMERGENCY_CLEAR)
    {
        emergencyActive = false;
        emergencyCancelRequested = false;
        pidController.reset();
        Serial.println("[EMERGENCY] Temperature safe - restoring selected control");
    }

    if (!emergencyActive)
        return false;

    setLouvrePercent(100);
    setFanPercent(100);
    return true;
}

void TemperatureRegulation::updateAutomatic(double chamberTemp)
{
    applyActiveSetpoint(lastMaterial);

    float demand = coolingDemand((float)chamberTemp);
    if (louvreOnly(lastMaterial))
        demand = constrain(demand, 0.0f, 100.0f);

    const uint8_t louvrePercent = demand >= 100.0f ? 100 : (uint8_t)lroundf(demand);
    uint8_t fanPercent = 0;
    if (!louvreOnly(lastMaterial) && demand > 100.0f)
        fanPercent = (uint8_t)constrain(lroundf(demand) - 100, 0, 100);

    setLouvrePercent(louvrePercent);
    setFanPercent(fanPercent);
}

void TemperatureRegulation::updateManual()
{
    setLouvrePercent(manualLouvrePct);
    setFanPercent(manualFanPct);
}

float TemperatureRegulation::coolingDemand(float chamberTemp)
{
    return pidController.calculate(chamberTemp);
}

void TemperatureRegulation::applyActiveSetpoint(uint8_t material)
{
    const bool useRemote = remoteLinkAlive() && hasRemoteSetpoint;
    const int target = useRemote ? remoteSetpoint : materialTarget(material);

    if (usingRemoteSetpoint != useRemote)
    {
        usingRemoteSetpoint = useRemote;
#if SERIAL_DEBUG
        Serial.println(useRemote ? "[LINK] Using CYD setpoint"
                                 : "[LINK] CYD unavailable - using material target");
#endif
    }

    if (lastAppliedSetpoint != target)
    {
        pidController.reset();
        lastAppliedSetpoint = target;
    }
    pidController.setSetpoint(target);
}

void TemperatureRegulation::applyRemoteCommand(const EnclosureCommand &cmd)
{
    if (cmd.magic != ENC_PROTO_MAGIC)
        return;

    lastRemoteRxMs = millis();
    hasRemoteRx = true;
    remoteJobActive = cmd.jobActive != 0;
#if SERIAL_DEBUG
    Serial.printf("[REMOTE] mode=%u setpoint=%u material=%u job=%u louvre=%u fan=%u\n",
                  static_cast<unsigned>(cmd.mode),
                  static_cast<unsigned>(cmd.setpoint),
                  static_cast<unsigned>(cmd.material),
                  static_cast<unsigned>(cmd.jobActive),
                  static_cast<unsigned>(cmd.louvrePct),
                  static_cast<unsigned>(cmd.fanPct));
#endif

    if (validRemoteMaterial(cmd.material))
    {
        remoteMaterial = cmd.material;
        hasRemoteMaterial = true;
    }

    if (cmd.mode == ENC_MODE_MANUAL)
    {
        controlMode = ENC_MODE_MANUAL;
        manualLouvrePct = cmd.louvrePct > 100 ? 100 : cmd.louvrePct;
        manualFanPct = cmd.fanPct > 100 ? 100 : cmd.fanPct;
        return;
    }

    if (cmd.mode == ENC_MODE_AUTO)
    {
        controlMode = ENC_MODE_AUTO;
        if (cmd.setpoint >= REMOTE_SETPOINT_MIN && cmd.setpoint <= REMOTE_SETPOINT_MAX)
        {
            remoteSetpoint = cmd.setpoint;
            hasRemoteSetpoint = true;
        }
    }
}

bool TemperatureRegulation::isDataValid(const OctoSnapshot &s)
{
    if (!s.valid || s.closedOrError)
        return false;
    if (s.toolActual == 0 && s.bedActual == 0 && s.chamberActual == 0)
        return false;
    return isfinite(s.toolActual) && isfinite(s.bedActual) && isfinite(s.chamberActual);
}

uint8_t TemperatureRegulation::classifyMaterial(const String &raw)
{
    String material = raw;
    material.trim();
    material.toUpperCase();

    if (material.length() == 0)
        return ENC_MATERIAL_UNKNOWN;
    if (materialHasToken(material, "PLA"))
        return ENC_MATERIAL_PLA;
    if (materialHasToken(material, "PETG"))
        return ENC_MATERIAL_PETG;
    if (materialHasToken(material, "ASA"))
        return ENC_MATERIAL_ASA;
    if (materialHasToken(material, "ABS"))
        return ENC_MATERIAL_ABS;
    if (materialHasToken(material, "TPU"))
        return ENC_MATERIAL_TPU;
    if (materialHasToken(material, "NYLON") || materialHasToken(material, "POLYAMIDE") ||
        materialHasToken(material, "PA"))
        return ENC_MATERIAL_NYLON;

    static const char *highTempMaterials[] = {
        "PC", "POLYCARBONATE", "HIPS", "PVA", "WOOD", "FLEX", "PP", "PPSU", "PPS", "PEI", "ULTEM"
    };
    for (const char *candidate : highTempMaterials)
    {
        if (materialHasToken(material, candidate))
            return ENC_MATERIAL_HIGH_TEMP;
    }

    return ENC_MATERIAL_UNKNOWN;
}

bool TemperatureRegulation::validRemoteMaterial(uint8_t material)
{
    return material <= ENC_MATERIAL_UNKNOWN;
}

int TemperatureRegulation::materialTarget(uint8_t material)
{
    switch (material)
    {
    case ENC_MATERIAL_PLA:
    case ENC_MATERIAL_PETG:
        return TEMP_STANDARD_TARGET;
    case ENC_MATERIAL_TPU:
        return TEMP_TPU_TARGET;
    case ENC_MATERIAL_ASA:
    case ENC_MATERIAL_ABS:
    case ENC_MATERIAL_NYLON:
    case ENC_MATERIAL_HIGH_TEMP:
    case ENC_MATERIAL_UNKNOWN:
    default:
        return TEMP_HIGH_TEMP_TARGET;
    }
}

bool TemperatureRegulation::louvreOnly(uint8_t material)
{
    return material == ENC_MATERIAL_ASA ||
           material == ENC_MATERIAL_ABS ||
           material == ENC_MATERIAL_UNKNOWN;
}

void TemperatureRegulation::transitionToState(MaterialState newState)
{
    if (newState == currentState)
        return;

    currentState = newState;
    pidController.reset();
    if (newState == ACTIVE_AUTO)
        applyActiveSetpoint(lastMaterial);
#if SERIAL_DEBUG
    Serial.printf("State transition: %s\n", getCurrentState().c_str());
#endif
}

void TemperatureRegulation::setLouvrePercent(uint8_t percent)
{
    if (percent > 100)
        percent = 100;
    if (lastLouvrePct != percent)
    {
        if (louver)
            louver->setPercent(percent);
        lastLouvrePct = percent;
        louvreOpen = percent > 0;
    }
}

void TemperatureRegulation::setFanPercent(uint8_t percent)
{
    if (percent > 100)
        percent = 100;
    setFanSpeed((uint8_t)(((uint16_t)percent * 255U) / 100U));
}

void TemperatureRegulation::setFanSpeed(uint8_t speed)
{
    if (speed == lastFanSpeed && fanActive == (speed > 0))
        return;
    fanPwmWrite(speed);
    lastFanSpeed = speed;
    fanActive = speed > 0;
}

void TemperatureRegulation::stopFan()
{
    setFanPercent(0);
    pidController.reset();
}

void TemperatureRegulation::closeOutputs()
{
    setLouvrePercent(0);
    setFanPercent(0);
}

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
    if (emergencyActive)
        return "EMERGENCY";

    switch (currentState)
    {
    case ACTIVE_AUTO:
        return "AUTO";
    case COOLING_FAN:
        return "FAN_COOLING";
    case COOLING_PASSIVE:
        return "PASSIVE_COOLING";
    case IDLE:
    default:
        return "IDLE";
    }
}

uint8_t TemperatureRegulation::stateCode() const
{
    if (currentState == IDLE)
        return ENC_STATE_IDLE;
    if (lastMaterial == ENC_MATERIAL_PLA)
        return ENC_STATE_PLA;
    if (lastMaterial == ENC_MATERIAL_PETG)
        return ENC_STATE_PETG;
    return ENC_STATE_ASA;
}

uint8_t TemperatureRegulation::controlModeCode() const
{
    return controlMode;
}

uint8_t TemperatureRegulation::fanPercent() const
{
    return (uint8_t)(((uint16_t)lastFanSpeed * 100U) / 255U);
}

uint8_t TemperatureRegulation::louvrePercent() const
{
    return lastLouvrePct;
}

bool TemperatureRegulation::remoteLinkAlive() const
{
    return hasRemoteRx && millis() - lastRemoteRxMs <= ENC_LINK_LOSS_TIMEOUT_MS;
}
