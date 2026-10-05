#ifndef TEMPERATURE_REGULATION_H
#define TEMPERATURE_REGULATION_H

#include <Arduino.h>
#include <enclosure_proto.h>
#include "octo_poller.h"

// Forward declaration
class ServoLouver;

class PIDController
{
private:
    float kp, ki, kd;
    float setpoint;
    float lastError;
    float integral;
    unsigned long lastTime;

public:
    PIDController(float _kp, float _ki, float _kd, float _setpoint);
    // Cooling PID: output grows when currentValue rises above setpoint.
    float calculate(float currentValue);
    void setSetpoint(float newSetpoint);
    int getSetpoint() const { return (int)setpoint; }
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

    float coolingDemand(float chamberTemp);
    void updateAutomatic(double chamberTemp);
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

    // Remote control (ESP-NOW commands from PrinterDisplay)
    void applyRemoteCommand(const EnclosureCommand &cmd);

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
};

#endif
