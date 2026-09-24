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
    void reset();
};

class TemperatureRegulation
{
private:
    OctoSnapshot lastSnap; // last-good snapshot (keep-last-good across failed polls)
    bool hasGoodData = false;
    ServoLouver *louver;
    PIDController pidController;
    bool fanActive;
    uint8_t lastFanSpeed;
    bool louvreOpen;
    unsigned long lastUpdate;
    unsigned long printEndTime;
    bool printEndValid;
    bool printWasActive;
    String lastMaterial; // upper-cased, latched while printing (for post-print windows)
    unsigned long invalidSince; // millis when data first went invalid (0 = valid)

    // Remote control (PrinterDisplay via ESP-NOW)
    uint8_t controlMode = ENC_MODE_AUTO; // EnclosureMode
    uint8_t remoteSetpoint = 0;          // desired chamber C, AUTO only
    bool hasRemoteSetpoint = false;
    uint8_t manualLouvrePct = 0; // 0-100, MANUAL only
    uint8_t manualFanPct = 0;    // 0-100, MANUAL only
    unsigned long lastRemoteRxMs = 0;
    bool hasRemoteRx = false;
    uint8_t lastLouvrePct = 0; // 0-100, tracked for telemetry

    // Material states
    enum MaterialState
    {
        IDLE,
        PLA_COOLING,
        PETG_COOLING,
        ASA_ABS_COOLING
    } currentState;
    unsigned long stateEnteredAt;

    // PID fan control
    uint8_t getCurrentFanSpeed(float chamberTemp);

    // Material type detection (normalized: trimmed + upper-cased)
    String getMaterialType(); // raw from last snapshot
    static String normalizeMaterial(const String &raw);
    bool materialContains(const String &norm, const char *token);

    // State management (snapshot-driven, never touch the network here)
    void updatePLALogic(const OctoSnapshot &snap);
    void updatePETGLogic(const OctoSnapshot &snap);
    void updateASALogic(const OctoSnapshot &snap);
    void updateManualLogic();
    void transitionToState(MaterialState newState);

    // Helper functions
    static bool isDataValid(const OctoSnapshot &s);
    bool isPLATrigger(const String &normMaterial, bool isPrinting, double nozzleTemp);
    bool isPETGTrigger(const String &normMaterial, bool isPrinting, double chamberTemp);
    bool isASATrigger(const String &normMaterial, bool isPrinting);
    void openLouvre();
    void closeLouvre();

public:
    explicit TemperatureRegulation(ServoLouver *servoLouver);

    // Initialize temperature regulation
    void begin();

    // Main update function - call this in the main loop with the latest
    // snapshot copy (non-blocking). Keeps regulating on last-good data
    // when the fresh copy is invalid; fan stays off until first good data.
    void update(const OctoSnapshot &snap);

    // Control functions
    void setFanSpeed(uint8_t speed); // 0-255
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
};

#endif
