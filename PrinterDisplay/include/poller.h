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
    // When this snapshot was published. Lets a reader tell fresh data from
    // stale, which "valid" alone cannot: the poll task keeps republishing the
    // last good values when OctoPrint stops answering, so valid stays true.
    unsigned long updatedMs = 0;
};

// Starts the poll task and its sync primitives. Returns immediately.
void pollerStart();

// Copies the latest snapshot. Returns false if the mutex was busy (skip the
// frame and try again next loop iteration). Never blocks on the network.
bool pollerCopySnapshot(PrinterSnapshot &out);

// True once at least one poll cycle has published data.
bool pollerHasData();

// Lightweight, mutex-protected view of the poll task's state, for the info
// screen. Deliberately NOT a PrinterSnapshot: that one carries six Strings, and
// the UI only needs a handful of scalars to say "what is this board pointed at,
// and is the data any good".
//
// SCALARS ONLY, no String members. Static DRAM on this board is historically
// ~800 bytes under its linker limit (see the FileListCache note above), so a
// struct that grew a String per field would be the wrong trade for a read-only
// diagnostic view.
//
// lastUpdateMs is when the last snapshot was published, which is the only way
// to tell "OctoPrint is idle" from "OctoPrint stopped answering". Without it a
// stale board and a quiet printer look identical, which is the single most
// common reason this screen is opened.
struct PollerOctoInfo
{
    IPAddress host;
    uint16_t port = 0;
    bool endpointOverridden = false;  // address came from the C3, not const.h
    bool apiKeyOverridden = false;    // key came from the C3, not secrets.h
    bool printing = false;
    bool paused = false;
    bool valid = false;         // at least one poll has ever succeeded
    unsigned long lastUpdateMs = 0; // 0 until the first publish
};

// Copies the above under the snapshot mutex. Returns false if the mutex was busy
// (skip this refresh and try again), exactly like pollerCopySnapshot().
bool pollerCopyOctoInfo(PollerOctoInfo &out);

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

// Adopts an OctoPrint API key sent by the C3 (EnclosureTelemetry::octoKey), and
// stores it in NVS so it survives a power cycle.
//
// `flags` is the ENC_KEYFLAG_* state from the same frame. This is the ONLY entry
// point, for both transports: the ESP-NOW receiver and the HTTP fallback both
// call it, so the two cannot drift apart in how they interpret a key.
//
//   ENC_KEYFLAG_PRESENT -> use `key`
//   ENC_KEYFLAG_CLEAR   -> drop our stored key, return to the compiled-in one
//   0 (or anything else) -> do nothing; keep what we have
//
// 0 is not "clear". The C3 deliberately omits the key from most frames, so
// treating an absent key as a reset would flip this board back to its own
// compiled-in key once per second.
//
// Everything arriving here is UNTRUSTED - see plausibleEndpoint() - so the key is
// validated before it is stored or handed to the client. A corrupt or hostile
// value must not reach the X-Api-Key header, and must not survive a reboot.
//
// Safe to call from any task, non-blocking: it only publishes, because the
// OctoPrinter instance belongs to the poll task.
void pollerSetApiKey(const char *key, uint8_t flags);

// True when this board is running on a key the C3 sent rather than its own
// compiled-in default.
bool pollerApiKeyOverridden();

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
