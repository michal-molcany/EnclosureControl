#ifndef SECRETS_EXAMPLE_H
#define SECRETS_EXAMPLE_H

// Copy this file to secrets.h and fill in your real values.
// secrets.h is git-ignored and must never be committed.

// Example:
//   copy include\secrets.example.h include\secrets.h   (Windows)
//   cp include/secrets.example.h include/secrets.h       (Unix)

#include <Arduino.h>

class secrets
{
public:
    String getOctoprintApiKey();
    String getOTAPassword();
};

inline String secrets::getOctoprintApiKey()
{
    return "REPLACE_WITH_OCTOPRINT_API_KEY";
}

// Password for ArduinoOTA over WiFi. Leave empty to disable OTA auth.
inline String secrets::getOTAPassword()
{
    return "REPLACE_WITH_OTA_PASSWORD";
}

// The OctoPrint server address is NOT here. It is shared with the PrinterDisplay
// (CYD) firmware, so it lives in one file at the repo root:
//   copy include\octoprint_endpoint.example.h include\octoprint_endpoint.h

#endif // SECRETS_EXAMPLE_H
