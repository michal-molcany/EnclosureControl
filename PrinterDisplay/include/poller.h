#ifndef POLLER_H
#define POLLER_H

#include <Arduino.h>

// Plain-data copy of everything the UI renders. Produced by the background
// poll task (core 0) which owns the OctoPrinter instance; consumed by the UI
// task, which must never touch the network or the OctoPrinter object.
struct PrinterSnapshot
{
    String state;
    bool printing = false;
    bool paused = false;
    double toolActual = 0;
    int toolTarget = 0;
    double bedActual = 0;
    int bedTarget = 0;
    double chamberActual = 0;
    int chamberTarget = 0;
    float progress = 0;
    String remaining;
    String fileName;
    String filament;
    String nozzle;
    int layerCurrent = 0;
    int layerTotal = 0;
    String lastLayerDuration;
    int lastLayerDurationSeconds = 0;
    bool valid = false;
};

// Starts the poll task and its sync primitives. Returns immediately.
void pollerStart();

// Copies the latest snapshot. Returns false if the mutex was busy (skip the
// frame and try again next loop iteration). Never blocks on the network.
bool pollerCopySnapshot(PrinterSnapshot &out);

// True once at least one poll cycle has published data.
bool pollerHasData();

// Non-blocking enqueue of a ButtonFunction command for the poll task.
void pollerSendCommand(int code);

#endif
