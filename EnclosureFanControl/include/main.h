#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <ESP32Servo.h>
#include <OctoPrinter.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiManager.h>
#include "const.h"
#include "secrets.h"
#include "servo_louver.h"
#include "temperature_regulation.h"

extern OctoPrinter prusa;
extern ServoLouver louver;
extern TemperatureRegulation tempRegulation;
void ServoMove(int angle);

#endif // MAIN_H