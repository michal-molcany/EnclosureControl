#include "temperature_regulation.h"
#include "const.h"
#include "servo_louver.h"

#include <string.h>

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
    : applied{kpValue, kiValue, kdValue}, setpoint(target),
      lastError(0), integral(0), lastTime(millis())
{
}

bool PIDController::calculate(float currentValue, float &out)
{
    const unsigned long currentTime = millis();
    const float timeStep = (currentTime - lastTime) / 1000.0f;
    lastTime = currentTime;

    if (timeStep <= 0)
        return false;

    // A long gap means the readings behind us were stale rather than a real
    // time step, and integrating a stale error across it would wind the
    // integral on information that was never true. Skipping is the honest
    // response: lastTime is already re-based, so the next call carries on.
    if (timeStep > PID_MAX_TIMESTEP_S)
        return false;

    const float error = currentValue - setpoint;
    if (error > 0)
        integral += error * timeStep;
    else
        integral *= 0.9f;

    // Clamp the I term's CONTRIBUTION, not the integral. Clamping the integral
    // would make the maximum windup depend on ki, which is now tunable - so
    // turning ki up would silently move the bound. See PID_I_TERM_MAX.
    const float iTerm = constrain(applied.ki * integral, 0.0f, PID_I_TERM_MAX);

    const float derivative = (error - lastError) / timeStep;
    lastError = error;

    const float output = applied.kp * error + iTerm + applied.kd * derivative;
    if (!isfinite(output))
        return false;
    out = constrain(output, 0.0f, 200.0f);
    return true;
}

bool PIDController::setGains(const PidGains &g)
{
    if (!pidGainsValid(g))
        return false;
    applied = g;
    return true;
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
    : louver(servoLouver),
      pidController(PID_KP_DEFAULT, PID_KI_DEFAULT, PID_KD_DEFAULT, TEMP_STANDARD_TARGET),
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
    fanTargetSpeed = 0;
    fanBoostUntilMs = 0;
    fanStartedMs = 0;
    lastLouvrePct = 0;
    lastUpdate = millis();
    lastDemand = 0.0f;
    // Force the first snapshot to count as new. 0 cannot be a real updatedMs
    // (the poll task stamps it before publishing), and starting here would
    // otherwise make the first regulation cycle a no-op.
    lastChamberUpdatedMs = 0;
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

    // A calibration run owns the outputs and bypasses the material/cooldown
    // machine entirely. The emergency check still goes first: it is the one
    // path that must never be behind an experiment, especially with a 90 C bed
    // heating the chamber.
    if (calibrationRunning())
    {
        if (haveData && updateEmergency(effective.chamberActual))
        {
            finishCalibration(false, "emergency: chamber reached 55 C");
            return;
        }
        updateCalibration(effective, haveData, now);
        return;
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

    updateAutomatic(effective.chamberActual, effective.updatedMs);
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

void TemperatureRegulation::updateAutomatic(double chamberTemp, unsigned long sampleMs)
{
    applyActiveSetpoint(lastMaterial);

    // LOOP RATE vs DATA RATE
    // ----------------------
    // This function runs at PID_UPDATE_INTERVAL (1 Hz), but chamberActual only
    // changes when the OctoPrint poll task publishes a new snapshot - every
    // OCTO_POLL_INTERVAL_MS (5 s). Running the integrator on all five ticks
    // meant four of them re-integrated an unchanged reading, which made the
    // controller behave like a 0.2 Hz loop wearing the costume of a 1 Hz one:
    // the dead time was really ~5-6 s while Ti was tuned against a 1 s step.
    //
    // So the PID is gated on NEW DATA (the snapshot's own updatedMs) and holds
    // its last demand in between. Zero-order hold is the correct treatment for
    // a sampled input - re-deriving a command from an identical reading cannot
    // produce a different answer, and setLouvrePercent/setFanPercent would drop
    // it as unchanged anyway.
    //
    // The 1 Hz gate above is deliberately KEPT for everything else in update():
    // the emergency check and the state machine are threshold tests on the
    // latest known value, and they want 1 Hz resolution regardless of how often
    // the temperature itself was measured.
    if (sampleMs != lastChamberUpdatedMs)
    {
        lastChamberUpdatedMs = sampleMs;
        float fresh = 0.0f;
        // A skipped sample (dt zero, or a stall too long to integrate across)
        // must leave lastDemand alone. Treating it as zero would slam the
        // louvre shut every time a poll ran late.
        if (coolingDemand((float)chamberTemp, fresh))
            lastDemand = fresh;
    }

    float demand = lastDemand;
    if (louvreOnly(lastMaterial))
        demand = constrain(demand, 0.0f, 100.0f);

    const uint8_t louvrePercent = demand >= 100.0f ? 100 : (uint8_t)lroundf(demand);
    uint8_t fanPercent = 0;
    if (!louvreOnly(lastMaterial) && demand > 100.0f)
        fanPercent = (uint8_t)constrain(lroundf(demand) - 100, 0, 100);

    setLouvrePercent(louvrePercent);
    setFanPercent(fanPercent);
}

bool TemperatureRegulation::setGains(const PidGains &g)
{
    if (!pidController.setGains(g))
    {
        Serial.printf("[PID] REJECTED kp=%.3f ki=%.3f kd=%.3f (out of range or not finite)\n",
                      (double)g.kp, (double)g.ki, (double)g.kd);
        return false;
    }
    Serial.printf("[PID] applied kp=%.3f ki=%.3f kd=%.3f\n",
                  (double)gains().kp, (double)gains().ki, (double)gains().kd);
    return true;
}

void TemperatureRegulation::updateManual()
{
    setLouvrePercent(manualLouvrePct);
    setFanPercent(manualFanPct);
}

bool TemperatureRegulation::coolingDemand(float chamberTemp, float &out)
{
    return pidController.calculate(chamberTemp, out);
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
    // fromRemote is what separates "the CYD is talking to us" from "a browser
    // asked nicely". Only the former may refresh lastRemoteRxMs, which is what
    // remoteLinkAlive() and the command-loss fallback read.
    applyCommand(cmd, true);
}

void TemperatureRegulation::applyLocalCommand(const EnclosureCommand &cmd)
{
    applyCommand(cmd, false);
}

void TemperatureRegulation::applyCommand(const EnclosureCommand &cmd, bool fromRemote)
{
    if (cmd.magic != ENC_PROTO_MAGIC)
        return;

    if (fromRemote)
    {
        lastRemoteRxMs = millis();
        hasRemoteRx = true;
    }
    remoteJobActive = cmd.jobActive != 0;
#if SERIAL_DEBUG
    // The tag names the SOURCE, because a web-UI command and a CYD command are
    // not interchangeable: only the CYD one refreshes the link liveness.
    Serial.printf("%s mode=%u setpoint=%u material=%u job=%u louvre=%u fan=%u\n",
                  fromRemote ? "[REMOTE]" : "[WEBUI]",
                  static_cast<unsigned>(cmd.mode),
                  static_cast<unsigned>(cmd.setpoint),
                  static_cast<unsigned>(cmd.material),
                  static_cast<unsigned>(cmd.jobActive),
                  static_cast<unsigned>(cmd.louvrePct),
                  static_cast<unsigned>(cmd.fanPct));
#endif

    // The relay needs the outputs to itself. A linked CYD keeps refreshing the
    // link liveness above - so the board still reports it as connected - but its
    // mode and material are ignored until the run ends. Otherwise its 2 s
    // heartbeat would fight the relay every other second, which is exactly the
    // kind of interference that makes the measured amplitude meaningless.
    if (calibrationRunning())
    {
#if SERIAL_DEBUG
        Serial.printf("[CAL] %s command ignored while calibration runs\n",
                      fromRemote ? "CYD" : "web UI");
#endif
        return;
    }

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

// ---------------------------------------------------------------------------
// PID calibration
// ---------------------------------------------------------------------------

bool TemperatureRegulation::calibrationRunning() const
{
    return calPhase == PidCalibration::Phase::Preheating ||
           calPhase == PidCalibration::Phase::Settling ||
           calPhase == PidCalibration::Phase::Relay;
}

bool TemperatureRegulation::startCalibration(String &reason)
{
    reason = "";

    if (calibrationRunning())
    {
        reason = "calibration is already running";
        return false;
    }
    if (calPhase == PidCalibration::Phase::Done)
    {
        reason = "apply or discard the previous calibration result first";
        return false;
    }
    if (emergencyActive)
    {
        reason = "emergency cooling is active";
        return false;
    }
    // The bed is driven through OctoPrint, so without a valid snapshot there is
    // no way to preheat it and no way to watch the chamber. Refuse rather than
    // start an experiment blind.
    if (!hasGoodData || !isDataValid(lastSnap))
    {
        reason = "no valid OctoPrint data - the bed cannot be controlled";
        return false;
    }
    // The relay needs the chamber to itself. A print would both move the
    // temperature and compete for the same printer.
    if (lastSnap.printing || lastSnap.paused || jobWasActive)
    {
        reason = "the printer is not idle (printing, paused, or still cooling down)";
        return false;
    }
    if (!isfinite(lastSnap.chamberActual) || !isfinite(lastSnap.bedActual))
    {
        reason = "the printer's temperature readings are not usable";
        return false;
    }
    // Starting from an already-warm chamber would spend the preheat phase
    // climbing towards the 50 C abort instead of preparing the experiment. The
    // run is recoverable, but refusing up front is clearer than failing later.
    if (lastSnap.chamberActual >= CAL_PREHEAT_COOL_C)
    {
        reason = "the chamber is already above 45 C - let it cool before calibrating";
        return false;
    }

    calOldGains = gains();
    calResult = PidCalibration::Result();
    calError = "";
    calResultApplied = false;
    calPhase = PidCalibration::Phase::Preheating;
    calStartedMs = millis();
    calPhaseStartedMs = calStartedMs;
    calExperimentStartedMs = 0;
    calFinishedMs = 0;
    calLastSampleMs = lastSnap.updatedMs;
    calLastSampleAtMs = calStartedMs;

    // Zero the detectors now, not at relay start: the progress dialog shows the
    // phase count from the first second, and a count left over from a previous
    // run would look like the new one was already oscillating.
    calAutotuner.begin(CAL_RELAY_LOW, CAL_RELAY_HIGH, CAL_NOISEBAND_C);

    // The run owns the outputs; leave the normal controller in AUTO so the
    // state it resumes into is deterministic, and drop any integral wound up by
    // the previous mode.
    controlMode = ENC_MODE_AUTO;
    pidController.reset();
    lastDemand = 0.0f;

    octoPollerRequestBed(CAL_BED_TARGET_C);

    Serial.printf("[CAL] starting: bed %d C, setpoint %.1f C, relay %.0f-%.0f, noiseband %.2f C\n",
                  CAL_BED_TARGET_C, (double)CAL_SETPOINT_C, (double)CAL_RELAY_LOW,
                  (double)CAL_RELAY_HIGH, (double)CAL_NOISEBAND_C);
    return true;
}

void TemperatureRegulation::cancelCalibration()
{
    if (calibrationRunning())
        finishCalibration(false, "cancelled from the web page");
}

bool TemperatureRegulation::applyCalibrationResult(String &reason)
{
    reason = "";
    if (calPhase != PidCalibration::Phase::Done || !calResult.valid)
    {
        reason = "no calibration result to apply";
        return false;
    }
    if (!pidController.setGains(calResult.gains))
    {
        reason = "the calculated gains are outside the accepted range";
        return false;
    }

    calResultApplied = true;
    const PidGains g = calResult.gains;
    // Consume the result: the gains are now the running gains, and the page's
    // PID card shows them. Leaving the dialog armed would only invite a second
    // apply of the same numbers.
    calPhase = PidCalibration::Phase::None;
    Serial.printf("[CAL] applied kp=%.3f ki=%.3f kd=%.3f\n",
                  (double)g.kp, (double)g.ki, (double)g.kd);
    return true;
}

void TemperatureRegulation::discardCalibrationResult()
{
    // A dismissal is not a cancel. Refusing while the relay is running keeps
    // the only two ways out of a run explicit: it finishes, or it is cancelled.
    if (calibrationRunning())
        return;

    calPhase = PidCalibration::Phase::None;
    calError = "";
    calResult = PidCalibration::Result();
    calResultApplied = false;
}

PidCalibration::Info TemperatureRegulation::calibrationInfo() const
{
    PidCalibration::Info info;
    info.phase = calPhase;
    info.running = calibrationRunning();
    info.hasResult = (calPhase == PidCalibration::Phase::Done) && calResult.valid;
    info.aborted = (calPhase == PidCalibration::Phase::Aborted);

    if (calStartedMs == 0)
        info.elapsedMs = 0;
    else if (calibrationRunning())
        info.elapsedMs = (uint32_t)(millis() - calStartedMs);
    else
        info.elapsedMs = (uint32_t)(calFinishedMs - calStartedMs);

    info.cycles = calAutotuner.phaseCount();
    info.ku = calResult.ku;
    info.pu = calResult.pu;
    info.amplitude = calResult.amplitude;
    info.relayDelta = calResult.relayDelta;
    info.oldGains = calOldGains;
    info.newGains = calResult.gains;
    info.clamped = calResult.clamped;
    info.applied = calResultApplied;
    info.zcSymmetrical = calResult.zcSymmetrical;
    info.amplitudeConvergent = calResult.amplitudeConvergent;
    info.proceededWithWarning = calResult.proceededWithWarning;
    info.bedTargetC = (float)CAL_BED_TARGET_C;
    info.setpointC = CAL_SETPOINT_C;

    strncpy(info.error, calError.c_str(), sizeof(info.error) - 1);
    info.error[sizeof(info.error) - 1] = '\0';
    return info;
}

void TemperatureRegulation::updateCalibration(const OctoSnapshot &snap, bool haveData,
                                              unsigned long now)
{
    if (!haveData)
    {
        finishCalibration(false, "lost valid OctoPrint data");
        return;
    }

    // The experiment window starts at the setpoint, not at the button press:
    // the preheat and the first climb to 40 C must not eat into the oscillation
    // time.
    if (calExperimentStartedMs != 0 &&
        now - calExperimentStartedMs > CAL_MAX_DURATION_MS)
    {
        finishCalibration(false, "timed out after 20 minutes at the setpoint");
        return;
    }
    // Absolute backstop. If the chamber never reaches the setpoint, the window
    // above never starts; this still ends the run and switches the bed off.
    if (now - calStartedMs > CAL_ABSOLUTE_TIMEOUT_MS)
    {
        finishCalibration(false, "timed out after 30 minutes in total");
        return;
    }

    // The normal regulator deliberately keeps running on the last-good snapshot
    // through a short poll outage. The relay cannot afford that: it is actively
    // driving the temperature, so an outage means the next relay decision would
    // be based on a reading that may no longer be true.
    if (snap.updatedMs != calLastSampleMs)
    {
        calLastSampleMs = snap.updatedMs;
        calLastSampleAtMs = now;
    }
    else if (now - calLastSampleAtMs > CAL_DATA_STALE_MS)
    {
        finishCalibration(false, "OctoPrint data went stale (link lost?)");
        return;
    }

    // A print starting mid-run changes the chamber's heat load and would fight
    // the relay for the fan and louvre. The start check refuses a busy printer;
    // this keeps that promise if one starts afterwards.
    if (snap.printing || snap.paused)
    {
        finishCalibration(false, "a print started during calibration");
        return;
    }

    const float chamber = (float)snap.chamberActual;
    if (!isfinite(chamber))
    {
        finishCalibration(false, "the chamber reading is not usable");
        return;
    }
    // Below the 55 C emergency trigger and above the 40 C setpoint, so the
    // no-cooling half of the relay has headroom without approaching the point
    // where the board would cancel a print.
    if (chamber > CAL_ABORT_CHAMBER_C)
    {
        finishCalibration(false, "chamber exceeded the 50 C abort limit");
        return;
    }

    switch (calPhase)
    {
    case PidCalibration::Phase::Preheating:
        // Let the bed heat the chamber, but do not let the chamber drift into
        // the abort limit on the way there. Opening the louvre is passive; the
        // fan stays off so it does not cool the bed's heat source.
        if (chamber >= CAL_PREHEAT_COOL_C)
        {
            setLouvrePercent(100);
            forceFanOff();
        }
        else
        {
            closeOutputs();
        }

        if (snap.bedActual >= CAL_BED_TARGET_C - CAL_BED_TOLERANCE_C)
        {
            calPhase = PidCalibration::Phase::Settling;
            calPhaseStartedMs = now;
            Serial.println("[CAL] bed at target - settling");
        }
        else if (now - calPhaseStartedMs > CAL_PREHEAT_TIMEOUT_MS)
        {
            finishCalibration(false, "the bed did not reach 90 C within 5 minutes");
        }
        break;

    case PidCalibration::Phase::Settling:
        // Short confirmation that the bed is holding target. Keep the same
        // preheat strategy so the chamber does not climb while we wait.
        if (chamber >= CAL_PREHEAT_COOL_C)
        {
            setLouvrePercent(100);
            forceFanOff();
        }
        else
        {
            closeOutputs();
        }

        if (now - calPhaseStartedMs >= CAL_SETTLE_MS)
        {
            calAutotuner.begin(CAL_RELAY_LOW, CAL_RELAY_HIGH, CAL_NOISEBAND_C);
            calPhase = PidCalibration::Phase::Relay;
            calPhaseStartedMs = now;
            Serial.printf("[CAL] relay autotune started at %.1f C (chamber %.1f C)\n",
                          (double)CAL_SETPOINT_C, (double)chamber);
        }
        break;

    case PidCalibration::Phase::Relay:
    {
        // The 20-minute clock starts the first time the chamber reaches the
        // setpoint, not when the relay starts: a cold enclosure can spend
        // several minutes in the initial no-cooling phase, and that is not the
        // experiment.
        if (calExperimentStartedMs == 0 && chamber >= CAL_SETPOINT_C)
        {
            calExperimentStartedMs = now;
            Serial.printf("[CAL] chamber reached %.1f C - 20 min window starts\n",
                          (double)CAL_SETPOINT_C);
        }
        bool finished = false;
        const float output = calAutotuner.update(chamber - CAL_SETPOINT_C,
                                                 (uint32_t)now, finished, calResult);
        applyRawDemand(output);
        if (finished)
            finishCalibration(true, nullptr);
        break;
    }

    default:
        break;
    }
}

void TemperatureRegulation::finishCalibration(bool success, const char *reason)
{
    const bool wasRunning = calibrationRunning();

    // The bed goes off on success AND on every abort. Leaving a 90 C bed behind
    // after a failed experiment is the one outcome that must never happen.
    octoPollerRequestBed(0);

    calFinishedMs = millis();
    calResultApplied = false;
    pidController.reset();
    lastDemand = 0.0f;

    if (success)
    {
        calPhase = PidCalibration::Phase::Done;
        calError = "";
        Serial.printf("[CAL] done: ku=%.2f pu=%.1f s amp=%.2f C kp=%.3f ki=%.3f kd=%.3f%s\n",
                      (double)calResult.ku, (double)calResult.pu,
                      (double)calResult.amplitude, (double)calResult.gains.kp,
                      (double)calResult.gains.ki, (double)calResult.gains.kd,
                      calResult.clamped ? " (clamped)" : "");
    }
    else
    {
        calPhase = PidCalibration::Phase::Aborted;
        calError = reason ? reason : "aborted";
        calResult = PidCalibration::Result();
        Serial.printf("[CAL] aborted: %s\n", calError.c_str());
    }

    if (wasRunning)
    {
        // Full cooling until the chamber is back at the normal cooldown end,
        // then the normal state machine closes the outputs and returns to IDLE.
        // This is also the safe path for an abort at 50 C: the louvre and fan
        // stay open rather than shutting a hot chamber in.
        transitionToState(COOLING_FAN);
    }
}

void TemperatureRegulation::applyRawDemand(float demand)
{
    if (!isfinite(demand))
        demand = 0.0f;
    if (demand < 0.0f)
        demand = 0.0f;
    if (demand > 200.0f)
        demand = 200.0f;

    const uint8_t louvre = demand >= 100.0f ? 100 : (uint8_t)lroundf(demand);
    setLouvrePercent(louvre);

    if (demand > 100.0f)
    {
        const uint8_t fan = (uint8_t)constrain(lroundf(demand) - 100, 0, 100);
        if (fan > 0)
            setFanPercent(fan);
        else
            forceFanOff();
    }
    else
    {
        // Immediate stop. setFanPercent(0) would honour FAN_MIN_ON_MS and
        // stretch the cooling phase, biasing the measured period.
        forceFanOff();
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

    // Duty floor. Below FAN_MIN_RUN_PCT the rotor cannot break away, and between
    // it and full speed it spins unevenly and tones up. A 0 request still means
    // "off" - only nonzero requests are raised, so the fan can always be stopped.
    const uint8_t target = (percent == 0)
                               ? 0
                               : (uint8_t)constrain((int)percent, (int)FAN_MIN_RUN_PCT, 100);

    setFanTarget((uint8_t)(((uint16_t)target * 255U) / 100U));
}

void TemperatureRegulation::forceFanOff()
{
    // Deliberately unconditional: no floor, no boost to unwind, and no
    // FAN_MIN_ON_MS hold. This is the path a fail-safe shutdown takes.
    fanBoostUntilMs = 0;
    fanTargetSpeed = 0;
    applyFanSpeed(0);
}

void TemperatureRegulation::setFanTarget(uint8_t speed)
{
    fanTargetSpeed = speed;

    if (speed == 0)
    {
        // Anti-chatter: hold the fan on briefly after it starts, so a PID demand
        // oscillating at the 1Hz update rate cannot chop it on and off and
        // re-trigger the break-away boost every second.
        if (fanActive && (millis() - fanStartedMs) < FAN_MIN_ON_MS)
            return;
        fanBoostUntilMs = 0;
        applyFanSpeed(0);
        return;
    }

    if (!fanActive)
    {
        // Off -> on. Break-away torque is highest at rest and worse when cold, so
        // go to full power briefly rather than stepping straight to the floor.
        fanStartedMs = millis();
        fanBoostUntilMs = fanStartedMs + FAN_START_BOOST_MS;
        applyFanSpeed((uint8_t)(((uint16_t)FAN_START_BOOST_PCT * 255U) / 100U));
        return;
    }

    // Still boosting: serviceFan() settles it when the deadline passes, because
    // update() only reaches here once a second.
    if (fanBoostUntilMs != 0 && millis() < fanBoostUntilMs)
        return;

    applyFanSpeed(speed);
}

void TemperatureRegulation::serviceFan()
{
    if (fanBoostUntilMs == 0)
        return;
    if (millis() < fanBoostUntilMs)
        return;
    fanBoostUntilMs = 0;
    // Whatever the regulator asked for may have changed during the boost; the
    // latest target wins.
    applyFanSpeed(fanTargetSpeed);
}

void TemperatureRegulation::applyFanSpeed(uint8_t speed)
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
    forceFanOff();
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

    if (calibrationRunning())
        return "CAL";

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
