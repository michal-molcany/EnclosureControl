# OctoPrinter

### Description

An Arduino library for interfacing with an OctoPrint instance.

# Dependencies
* [ArduinoJson](https://github.com/bblanchon/ArduinoJson)

# Usage
```cpp
#include <Arduino.h>
#include <OctoPrinter.h>
#include <WiFi.h>

WiFiClient client;
OctoPrinter printerName(API_KEY, HOST_IP, HOST_PORT);

void setup() {
    Serial.begin(115200);
    WiFi.begin(SSID, PASSWORD);
    while(WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    printerName.begin();
}

void loop() {
    Serial.print("Nozzle temperature actual/target: ");
    Serial.print(printerName.toolActual());
    Serial.print("/");
    Serial.println(printerName.toolTarget());
    delay(1000);
}
```

# Liveness: `lastUpdateOk()`

Every value getter here is **keep-last-good**: when a request fails, the parsed
state is left exactly as it was. Nothing in the values can then distinguish "the
printer is idle" from "OctoPrint stopped answering". `lastUpdateOk()` answers
that: true when the last `update()` actually reached the server (its
`/api/connection` request, which every cycle makes, returned HTTP 200), false
when that request could not connect, timed out, or came back with any other
status.

The decision is taken from the HTTP status line, not from the response body: an
error page is also a body, and a truncated one is an empty body, so neither can
tell a dead server from one that answered something unexpected.

The pollers use it to decide whether their snapshot timestamp may advance, which
is what makes staleness detectable downstream: the CYD's info screen turns its
OctoPrint row red, and the C3 holds its PID demand instead of integrating a
reading it never received.
