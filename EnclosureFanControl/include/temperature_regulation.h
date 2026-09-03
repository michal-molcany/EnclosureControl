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
    bool louvreOpen;
    unsigned long lastUpdate;
    unsigned long printEndTime;
    bool printWasActive;

    // Material states
    enum MaterialState
    {
        IDLE,
        PLA_COOLING,
        PETG_COOLING,
        ASA_ABS_COOLING
    } currentState;

    // PID fan control
    uint8_t getCurrentFanSpeed(float chamberTemp);

    // Material type detection
    String getMaterialType();

    // State management
    void updatePLALogic();
    void updatePETGLogic();
    void updateASALogic();
    void transitionToState(MaterialState newState);

    // Helper functions
    bool isPLATrigger();
    bool isPETGTrigger();
    bool isASATrigger();
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
