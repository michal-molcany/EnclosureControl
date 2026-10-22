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

// Asks the poll task to set the bed target, in whole degrees C (0 turns the bed
// off). Non-blocking and safe to call from any task.
//
// The OctoPrinter instance belongs to the poll task and is mid-request most of
// the time, so this only records the value; the task sends it at the top of its
// next pass. Last write wins - there is only ever one correct target, and a
// queue of superseded preheat commands would just heat the bed late.
//
// This exists for PID calibration, which needs to preheat and then switch the
// bed off without blocking loop() on an HTTP request.
void octoPollerRequestBed(int targetC);

// Asks the poll task to send ONE G-code line to the printer itself.
//
// Same contract as octoPollerRequestBed(): non-blocking, safe from any task,
// last write wins. The poll task owns the OctoPrinter instance and is
// mid-request most of the time, so the line is only recorded here and sent at
// the top of the task's next pass.
//
// The two requests are used by the PID calibration:
//
//   octoPollerRequestPause()    -> M25, the printer pauses
//   octoPollerRequestUnpause()  -> M84, the pause ends and the heaters go off
//
// M25 is what keeps a firmware-side heating timeout from switching the 90 C
// bed off in the middle of the run; M84 is the way back out, so the printer is
// left ready for the next print.
void octoPollerRequestPause();
void octoPollerRequestUnpause();

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

// Repoints the poll task at a different OctoPrint API key.
//
// Same contract and same reason as octoPollerSetEndpoint(): the OctoPrinter
// instance belongs to the poll task and is mid-request most of the time, so the
// value is only recorded here and applied by that task at the top of its own
// loop. Safe to call from any task, never blocks.
//
// `key` is copied into a fixed buffer, so the caller may pass a pointer into a
// String that changes immediately afterwards.
void octoPollerSetApiKey(const char *key);

#endif // OCTO_POLLER_H
