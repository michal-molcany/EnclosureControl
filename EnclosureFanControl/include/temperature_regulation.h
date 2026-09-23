#ifndef TEMPERATURE_REGULATION_H
#define TEMPERATURE_REGULATION_H

#include <Arduino.h>
#include <OctoPrinter.h>

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
    OctoPrinter *printer;
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
    String getMaterialType(); // raw from OctoPrinter
    static String normalizeMaterial(const String &raw);
    bool materialContains(const String &norm, const char *token);

    // State management
    void updatePLALogic();
    void updatePETGLogic();
    void updateASALogic();
    void transitionToState(MaterialState newState);

    // Helper functions
    bool isDataValid();
    bool isPLATrigger(const String &normMaterial);
    bool isPETGTrigger(const String &normMaterial);
    bool isASATrigger(const String &normMaterial);
    void openLouvre();
    void closeLouvre();

public:
    TemperatureRegulation(OctoPrinter *octoPrinter, ServoLouver *servoLouver);

    // Initialize temperature regulation
    void begin();

    // Main update function - call this in the main loop
    void update();

    // Control functions
    void setFanSpeed(uint8_t speed); // 0-255
    void stopFan();

    // Status functions
    bool isFanActive() const;
    bool isLouvreOpen() const;
    String getCurrentState() const;
};

#endif
