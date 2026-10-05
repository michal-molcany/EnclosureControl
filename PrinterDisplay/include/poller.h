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

// One command for the poll task to execute off the UI task.
//
// Carries a target and a payload rather than a flat code enum, because a
// temperature is open-ended: the bed view's stepper produces values that no
// fixed set of enum constants can name. An earlier revision encoded the tool
// temperature directly in the enum (PREHEAT_215 = 215), which silently breaks if
// those constants are ever renumbered.
struct PollerCommand
{
    enum Target : uint8_t
    {
        Tool,
        Bed,
        Job
    };

    Target target;
    int value; // temperature in C for Tool/Bed (0 = off); JobCommand for Job
};

// Non-blocking enqueue of a command for the poll task.
void pollerSendCommand(const PollerCommand &cmd);

#endif
