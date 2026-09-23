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
};

inline String secrets::getOctoprintApiKey()
{
    return "REPLACE_WITH_OCTOPRINT_API_KEY";
}

// IP address of the OctoPrint server
#define OCTOPRINT_IP "192.168.0.100"
// Port of the OctoPrint server
#define OCTOPRINT_PORT 5000

// Keep legacy misspelled macro working for older code.
#ifndef OCTOTPRINT_IP
#define OCTOTPRINT_IP OCTOPRINT_IP
#endif
#ifndef OCTOTPRINT_PORT
#define OCTOTPRINT_PORT OCTOPRINT_PORT
#endif

#endif // SECRETS_EXAMPLE_H
