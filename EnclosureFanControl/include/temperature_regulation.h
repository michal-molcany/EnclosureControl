#ifndef TEMPERATURE_REGULATION_H
#define TEMPERATURE_REGULATION_H

#include <Arduino.h>
#include <enclosure_proto.h>
#include <enclosure_pid.h>
#include "octo_poller.h"

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
