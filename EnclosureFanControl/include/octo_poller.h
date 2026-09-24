#ifndef OCTO_POLLER_H
#define OCTO_POLLER_H

#include <Arduino.h>

// Minimal plain-data copy of what EnclosureFanControl needs.
// Produced by the background poll task (owns the OctoPrinter instance);
// consumed by loop(), which must never touch the network or OctoPrinter.
struct OctoSnapshot
{
    bool printing = false;
    String filament;
    double toolActual = 0;
    double bedActual = 0;
    double chamberActual = 0;
    bool closedOrError = true;
    bool valid = false;
    unsigned long updatedMs = 0;
};

// True when snapshot holds usable data (same predicate as before:
// not closed/error and not all-zero readings).
inline bool octoSnapshotValid(const OctoSnapshot &s)
{
    if (!s.valid || s.closedOrError)
        return false;
    return !(s.toolActual == 0 && s.bedActual == 0 && s.chamberActual == 0);
}

// Starts the poll task and its sync primitives. Returns immediately.
// Must be called after WiFi is connected.
void octoPollerStart();

// Copies the latest snapshot. Returns false if the mutex was busy (skip
// this iteration and try again next loop). Never blocks on the network.
// Returns true with `out` filled (check out.valid / octoSnapshotValid()).
bool octoPollerCopy(OctoSnapshot &out);

// True once at least one poll cycle has published data.
bool octoPollerHasData();

#endif // OCTO_POLLER_H
