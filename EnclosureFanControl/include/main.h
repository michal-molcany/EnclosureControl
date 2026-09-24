#ifndef MAIN_H
#define MAIN_H

#include <Arduino.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include "const.h"
#include "secrets.h"
#include "octo_poller.h"
#include "servo_louver.h"
#include "temperature_regulation.h"
#include "chamber_display.h"
#include "espnow_link.h"

extern ServoLouver louver;
extern TemperatureRegulation tempRegulation;
extern ChamberDisplay display;

#endif // MAIN_H
