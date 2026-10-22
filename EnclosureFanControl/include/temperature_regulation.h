#ifndef TEMPERATURE_REGULATION_H
#define TEMPERATURE_REGULATION_H

#include <Arduino.h>
#include <enclosure_proto.h>
#include <enclosure_pid.h>
#include "octo_poller.h"
#include "pid_calibration.h"

// Forward declaration
class ServoLouver;

class PIDController
{
private:
    // Named `applied` rather than `gains` because the accessor below wants that
    // name; a member and a method cannot share one in a class scope.
    PidGains applied;
    float setpoint;
    float lastError;
    float integral;
    unsigned long lastTime;

public:
    PIDController(float _kp, float _ki, float _kd, float _setpoint);
    // Cooling PID: output grows when currentValue rises above setpoint.
    // Returns false when no sample was taken - the interval since the last call
    // was zero, or too long to integrate across - and leaves `out` untouched.
    // Callers must HOLD their previous value in that case: treating a skipped
    // sample as a demand of zero would slam the louvre shut every time a poll
    // was late.
    bool calculate(float currentValue, float &out);
    void setSetpoint(float newSetpoint);
    int getSetpoint() const { return (int)setpoint; }
    // Runtime gain update. Returns false and changes NOTHING when the values are
    // out of range or non-finite - a rejected tuning must leave the regulator
    // exactly as it was, mid-print. Deliberately does not reset(): reset() also
    // zeroes lastError, which causes a derivative kick on the next sample.
    bool setGains(const PidGains &g);
    const PidGains &gains() const { return applied; }
    void reset();
};

class TemperatureRegulation
{
private:
    OctoSnapshot lastSnap;
    bool hasGoodData = false;
    ServoLouver *louver;
    PIDController pidController;
    bool fanActive;
    uint8_t lastFanSpeed;
    // What the regulator asked for (0-255), as opposed to what is currently on
    // the PWM. They differ while a break-away boost is in progress.
    uint8_t fanTargetSpeed = 0;
    unsigned long fanBoostUntilMs = 0; // 0 = not boosting
    unsigned long fanStartedMs = 0;    // for FAN_MIN_ON_MS anti-chatter
    bool louvreOpen;
    unsigned long lastUpdate;
    unsigned long invalidSince;
    // Loop-rate bookkeeping. update() itself runs at PID_UPDATE_INTERVAL (1 Hz)
    // because the state machine and the emergency check want that resolution,
    // but the PID only gets to run when a NEW chamber reading has actually
    // arrived - see the loop-rate note in updateAutomatic().
    float lastDemand = 0.0f;
    unsigned long lastChamberUpdatedMs = 0;
    bool jobWasActive = false;
    uint8_t lastMaterial = ENC_MATERIAL_UNKNOWN;
    bool emergencyActive = false;
    bool emergencyCancelRequested = false;

    uint8_t controlMode = ENC_MODE_AUTO;
    uint8_t remoteSetpoint = 0;
    bool hasRemoteSetpoint = false;
    uint8_t remoteMaterial = ENC_MATERIAL_UNKNOWN;
    bool hasRemoteMaterial = false;
    bool remoteJobActive = false;
    uint8_t manualLouvrePct = 0;
    uint8_t manualFanPct = 0;
    unsigned long lastRemoteRxMs = 0;
    bool hasRemoteRx = false;
    bool usingRemoteSetpoint = false;
    int lastAppliedSetpoint = -1;
    uint8_t lastLouvrePct = 0;

    // -----------------------------------------------------------------------
    // PID calibration (the web UI's Calibrate button).
    //
    // The state machine lives HERE rather than in a separate class because this
    // is the only object allowed to drive the louvre and fan. A second owner
    // would be the exact "two things fighting over one actuator" bug the single
    // owner rule exists to prevent.
    //
    // While a run is active, update() hands control to updateCalibration() and
    // returns: the normal material/cooldown machine is bypassed, but the
    // emergency check still runs first and can abort the run.
    // -----------------------------------------------------------------------
    PidCalibration::Phase calPhase = PidCalibration::Phase::None;
    PidCalibration::Autotuner calAutotuner;
    PidCalibration::Result calResult;
    String calError;
    PidGains calOldGains = {0.0f, 0.0f, 0.0f};
    unsigned long calStartedMs = 0;
    unsigned long calPhaseStartedMs = 0;
    // 0 until the chamber first reaches CAL_SETPOINT_C. The 20-minute
    // experiment window is measured from here, so the preheat does not consume
    // it.
    unsigned long calExperimentStartedMs = 0;
    unsigned long calFinishedMs = 0;
    unsigned long calLastSampleMs = 0;
    unsigned long calLastSampleAtMs = 0;
    bool calResultApplied = false;

    enum MaterialState
    {
        IDLE,
        ACTIVE_AUTO,
        COOLING_FAN,
        COOLING_PASSIVE
    } currentState = IDLE;

    bool coolingDemand(float chamberTemp, float &out);
    // Shared body of applyRemoteCommand()/applyLocalCommand(). `fromRemote` is
    // the only difference between them: it decides whether this counts as proof
    // that the CYD is alive.
    void applyCommand(const EnclosureCommand &cmd, bool fromRemote);
    void updateAutomatic(double chamberTemp, unsigned long sampleMs);
    void updateManual();
    bool updateEmergency(double chamberTemp);
    void applyActiveSetpoint(uint8_t material);
    void transitionToState(MaterialState newState);
    static bool isDataValid(const OctoSnapshot &s);
    static uint8_t classifyMaterial(const String &raw);
    static bool validRemoteMaterial(uint8_t material);
    static int materialTarget(uint8_t material);
    static bool louvreOnly(uint8_t material);
    void setLouvrePercent(uint8_t percent);
    void setFanPercent(uint8_t percent);
    // Immediate off, bypassing the duty floor, the break-away boost and
    // FAN_MIN_ON_MS. Used by the fail-safe paths (invalid OctoPrint data, leaving
    // the active-auto state) so a safety shutdown is never delayed.
    void forceFanOff();
    // Applies the floor and boost/min-on-time, then delegates to applyFanSpeed.
    void setFanTarget(uint8_t speed);
    // The only place that writes the PWM, so the change-detection early-out and
    // fanActive bookkeeping cannot be bypassed.
    void applyFanSpeed(uint8_t speed);
    void closeOutputs();

    bool calibrationRunning() const;
    void updateCalibration(const OctoSnapshot &snap, bool haveData, unsigned long now);
    void finishCalibration(bool success, const char *reason);
    // Applies a 0-200 cooling demand directly, bypassing the fan duty floor and
    // FAN_MIN_ON_MS. The relay's two levels are 0 and 200, so the fan must be
    // able to stop the instant the relay flips; the anti-chatter hold would
    // stretch the cooling phase and bias the measured period.
    void applyRawDemand(float demand);

public:
    explicit TemperatureRegulation(ServoLouver *servoLouver);

    // Initialize temperature regulation
    void begin();

    // Main update function - call this in the main loop with the latest
    // snapshot copy (non-blocking). Keeps regulating on last-good data
    // when the fresh copy is invalid; fan stays off until first good data.
    void update(const OctoSnapshot &snap);

    // Call this from loop() on EVERY iteration, not on a timer. update() is
    // gated to PID_UPDATE_INTERVAL (1Hz), which is far too coarse to end a
    // sub-second break-away boost - without this the fan holds full power for a
    // whole second on every start. Does nothing when no boost is in progress.
    void serviceFan();

    // Control functions
    void stopFan();

    // -----------------------------------------------------------------------
    // PID calibration
    // -----------------------------------------------------------------------

    // Starts the bed preheat + relay experiment. Returns false with `reason`
    // filled when the board is not in a state to run it (printer busy, no valid
    // OctoPrint data, a run already active, or an unacknowledged result from a
    // previous run that must be applied or discarded first).
    bool startCalibration(String &reason);

    // Aborts a run. Does nothing when no run is active. The bed is switched off
    // and the outputs enter the normal full-cooling path.
    void cancelCalibration();

    // Applies the measured gains to RAM and consumes the result. Persisting is
    // deliberately NOT done here: the UI calls its existing /api/pid/save for
    // that, so "apply" and "apply and save" share one board-side write path.
    bool applyCalibrationResult(String &reason);

    // Drops the result without touching the running gains. No-op while a run is
    // active - cancel it first, so a dismissal can never leave the relay driving
    // the outputs with nothing left to finish it.
    void discardCalibrationResult();

    // Snapshot for the web UI. Safe at any time.
    PidCalibration::Info calibrationInfo() const;

    // Runtime PID gains. Returns false (leaving the regulator untouched) when
    // the values are out of range or non-finite. Takes effect on the next
    // chamber reading, not immediately - the PID only samples when new data
    // arrives.
    bool setGains(const PidGains &g);
    PidGains gains() const { return pidController.gains(); }

    // Remote control (ESP-NOW commands from PrinterDisplay)
    void applyRemoteCommand(const EnclosureCommand &cmd);

    // A command from the board's OWN web UI (lib/EnclosureUi POST /api/manual).
    //
    // Applies exactly what applyRemoteCommand() applies - mode, and the manual
    // louvre/fan percentages - but deliberately does NOT stamp the remote-link
    // liveness. Routing a browser click through applyRemoteCommand() would make
    // the OLED claim the display is connected while it is not, and would
    // suppress the command-link-loss fallback that exists to catch a real
    // disconnection. The two sources must not be able to impersonate each other.
    void applyLocalCommand(const EnclosureCommand &cmd);

    // Status functions
    bool isFanActive() const;
    bool isLouvreOpen() const;
    String getCurrentState() const;
    uint8_t stateCode() const;   // EnclosureState for telemetry
    uint8_t controlModeCode() const; // EnclosureMode for telemetry
    uint8_t fanPercent() const;  // 0-100
    uint8_t louvrePercent() const; // 0-100
    bool remoteLinkAlive() const;
    bool jobIsActive() const { return jobWasActive; }
    // Bed actual from the last good OctoPrint snapshot. Used by the calibration
    // progress dialog and the preheat wait; meaningless until hasGoodData.
    double bedActual() const { return lastSnap.bedActual; }

    // Read-only extras for the web UI (lib/EnclosureUi), which renders what the
    // regulator is actually doing rather than what was last requested.
    uint8_t activeMaterial() const { return lastMaterial; }   // ENC_MATERIAL_*
    // Setpoint currently in force, whole C. Negative until one has been applied,
    // which the page shows as unavailable instead of as a temperature.
    int activeSetpoint() const { return lastAppliedSetpoint; }
    // Named isEmergencyActive() rather than emergencyActive(): a member and a
    // method cannot share one name in a class scope, and the member is the
    // flag this reports.
    bool isEmergencyActive() const { return emergencyActive; }
    // What the manual mode is holding, so the page can show the committed values
    // rather than its own unsent slider positions.
    uint8_t manualLouvrePercent() const { return manualLouvrePct; }
    uint8_t manualFanPercent() const { return manualFanPct; }
};

#endif
