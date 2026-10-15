#ifndef OCTO_POLLER_H
#define OCTO_POLLER_H

#include <Arduino.h>

// Minimal plain-data copy of what EnclosureFanControl needs.
// Produced by the background poll task (owns the OctoPrinter instance);
// consumed by loop(), which must never touch the network or OctoPrinter.
struct OctoSnapshot
{
    bool printing = false;
    bool paused = false;
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
void octoPollerRequestCancel();

// Repoints the poll task at a different OctoPrint server.
//
// Non-blocking and safe to call from any task: it only records the value. The
// OctoPrinter instance is owned by the poll task and is mid-request most of the
// time, so mutating its host underneath that would be a data race. The task
// applies a pending change at the top of its own loop, which also means a
// repoint can never land in the middle of a request.
//
// Last write wins: the value is a single slot, not a queue, because there is
// only ever one correct answer and queueing a backlog of intermediate addresses
// would just poll each of them in turn.
void octoPollerSetEndpoint(IPAddress host, uint16_t port);

#endif // OCTO_POLLER_H
