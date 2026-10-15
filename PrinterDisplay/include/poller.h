#ifndef POLLER_H
#define POLLER_H

#include <Arduino.h>

#include "const.h"

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

// Repoints the poll task at a different OctoPrint server, and stores it in NVS
// so it survives a power cycle.
//
// The CYD does not choose this address - the C3 owns it and sends it down the
// link (see EnclosureTelemetry::octoIp). It is stored rather than merely
// applied for one obvious reason: this board can be rebooted or power-cycled
// while the C3 is unreachable, and it must come back on the same server rather
// than reverting to whatever its firmware header says.
//
// Safe to call from any task, non-blocking: like the C3's octoPollerSetEndpoint
// it only publishes a value, because the OctoPrinter instance belongs to the
// poll task and is mid-request most of the time. An all-zero IP means "the C3
// has no override" - clear the stored one and fall back to the compiled-in
// OCTOPRINT_IP, which is what keeps the two boards agreeing about "no
// override" instead of drifting.
void pollerSetEndpoint(IPAddress host, uint16_t port);

// True when this board is running on an address the C3 sent rather than its own
// compiled-in default. Shown in the serial log and available to the UI.
bool pollerEndpointOverridden();

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

// Newest-first printable-file cache, filled on demand by REFRESH_FILES (the
// files view opens it, not the 3 s background poll). Same ownership as the
// snapshot: written by the poll task, read by the UI task.
//
// NOTE on memory: the list (~3.8 KB for 20 entries) lives on the heap,
// allocated once in pollerStart(). The CYD link already overflows its DRAM
// segment with a second static copy, so the UI deliberately holds NO mirror:
// it reads metadata + single entries through the accessors below, and row
// text lives in the LVGL labels themselves.
struct PrintFile
{
    char path[PRINT_FILE_PATH_LEN];
    char name[PRINT_FILE_NAME_LEN];
    uint32_t date = 0;
    uint32_t size = 0;
    // File-info view. Mirrors OctoPrinter::OctoFile and comes from the same
    // listing pass - nothing here costs an extra request. Zeros mean OctoPrint
    // has no value, which the view must show as unknown, not as a real 0.
    char material[16] = {};
    double estimatedPrintTime = 0;   // seconds
    double filamentLengthMm = 0;     // mm
    double filamentVolumeCm3 = 0;    // cm3
    float dimX = 0, dimY = 0, dimZ = 0; // mm
    double averagePrintTime = 0;     // seconds, 0 when never printed
    uint16_t printsSuccess = 0;
    uint16_t printsFailure = 0;
    // GUESSED from the file name, never from OctoPrint - it does not expose
    // either value. 0 = the name had nothing recognisable. The UI must say so.
    float layerHeightMm = 0;
    float nozzleMm = 0;
};

// Cache metadata without copying the list. Returns false if the mutex was
// busy (skip the frame and try again next loop iteration).
bool pollerFilesMeta(uint8_t &count, uint32_t &version, bool &busy, bool &error);

// Copies a single cached entry (0-based). Returns false on a bad index, a
// missing cache, or a busy mutex.
bool pollerCopyFile(int index, PrintFile &out);

// Asks the poll task to re-fetch the file list (sets busy immediately when
// the mutex is free so the view shows "Loading..." without waiting a cycle).
void pollerRequestFilesRefresh();

// Stages a path and asks the poll task to select-and-print it. Ignored when
// empty or when a job is already active (the poll task is the authority).
void pollerRequestFilePrint(const char *path);

#endif
