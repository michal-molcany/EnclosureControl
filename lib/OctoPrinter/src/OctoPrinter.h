#ifndef OctoPrinter_h
#define OctoPrinter_h

#include "Arduino.h"
#include "WiFi.h"
#include "IPAddress.h"
#include "ArduinoJson.h"

// Master switch for this library's verbose logging. OFF by default.
//
// When on, every poll dumps the raw OctoPrint JSON to the serial console - several
// KB per poll across Printer JSON, Job JSON and Layer Progress JSON. That is
// enough to bury everything else on the line, including the ESP-NOW link status
// line, which is the one output that matters when a link is down.
//
// This deliberately does NOT use SERIAL_DEBUG. Both projects define that for their
// own diagnostics (the C3 sets it to 1 for its [DBG] and [REMOTE] lines), and this
// library used to squat on the same macro with a `#ifndef` default, which meant
// turning on project logging silently turned on megabytes of JSON here too. Keep
// the two switches independent.
//
// Enable per project from platformio.ini with e.g. -DOCTO_DEBUG_LOG=1, or locally
// by defining it before including this header.
#ifndef OCTO_DEBUG_LOG
#define OCTO_DEBUG_LOG 0
#endif

struct temperatureTracker
{
  double actual;
  int target;
  int offset;
};

struct jobTimer
{
  int raw;
  int days;
  int hours;
  int minutes;
  int seconds;
  String formatted;
};

// One printable file from GET /api/files. Fixed-size so the caller can hold
// the latest-20 list statically (no per-file malloc on the poll task).
// path is the OctoPrint file path (folders included, e.g. "subdir/part.gcode")
// used to build POST /api/files/local/<path>; name is the bare display name;
// date is the Unix timestamp OctoPrint reports for sorting newest-first.
struct OctoFile
{
  char path[120];
  char name[64];
  uint32_t date;
  uint32_t size;

  // Details for the file-info view. The list fetch leaves these zero; they are
  // filled on demand by fetchFileInfo() when the user opens one file's details.
  // The listing already contains them, but carrying them for every file made the
  // parsed document - not the transfer - the slow part of the file list.
  //
  // Every numeric field is 0 when OctoPrint has no value (typically: the slicer
  // wrote no analysis into the file). Callers must show that as unknown rather
  // than as a real zero - a 0 mm filament length is not information.
  char material[16];          // firstFilament from the slicer metadata
  double estimatedPrintTime;  // seconds, from gcodeAnalysis
  double filamentLengthMm;    // mm, total across tools
  double filamentVolumeCm3;   // cm3
  float dimX, dimY, dimZ;     // mm, object bounding box
  double averagePrintTime;    // seconds, 0 when never printed
  uint16_t printsSuccess;
  uint16_t printsFailure;

  // Layer height and nozzle diameter, ONLY ever read out of the file name.
  //
  // These are the one pair of slicer settings OctoPrint does not expose at all:
  // /api/files and /api/files/local/<path> both answer with gcodeAnalysis,
  // dimensions and statistics, and neither carries layer height or nozzle - not
  // even in the embedded slicer metadata. The name is the only place left, and
  // it is where most slicers put them anyway (name_0.15mm_ASA_MK3S.gcode).
  //
  // Both are therefore HEURISTICS and the UI labels them as such. 0 means the
  // name had nothing recognisable.
  float layerHeightMm = 0;
  float nozzleMm = 0;
};

class OctoPrinter
{
public:
  OctoPrinter(String key, String host, int port);
  void update();
  void begin();

  // Repoints this instance at a different OctoPrint.
  //
  // Clears _doOnce so the next begin() re-runs /api/version against the new
  // host: the cached server/API version strings belong to the OLD server and
  // would otherwise be reported as the new one's.
  //
  // NOT THREAD SAFE. Every other method here touches _client and the parsed
  // state without locking, because the library's contract is that its owner
  // calls it from one task only. Both pollers own their instance on a dedicated
  // task, so a caller changing the address at runtime must publish the new
  // value and let that task apply it - see octoPollerSetEndpoint /
  // pollerSetEndpoint, which exist for exactly that reason.
  void setEndpoint(IPAddress host, int port);

  // Replaces the API key used in the X-Api-Key header of every request.
  //
  // Exists so the key can be changed at runtime - entered in the web UI and
  // stored in NVS - instead of being frozen into the binary at build time by
  // the constructor.
  //
  // Same rules as setEndpoint(): NOT THREAD SAFE (it touches _apiKey and the
  // socket without locking), and it clears _doOnce so the next begin() re-runs
  // /api/version against the new credentials - the cached server/API version
  // strings were fetched with the old key, and a key that cannot read them
  // would otherwise report another server's versions as its own.
  void setApiKey(const char *key);

  /*
    These functions are used to check on the printer's status flags.
    All they do is return the state of the boolean flag.
  */
  bool operational();
  bool paused();
  bool printing();
  bool cancelling();
  bool pausing();
  bool error();
  bool ready();
  bool closed();
  bool closedOrError();

  // give job related information
  String remainingFormatted();
  String fileName();
  String Status();
  double progress();
  String filamentName();
  String nozzleDiameter();

  // give layer progress information
  int currentLayer();
  int totalLayers();
  String averageLayerDuration();
  String lastLayerDuration();
  int averageLayerDurationSeconds();
  int lastLayerDurationSeconds();

  /*
    Functions to control jobs.
    These each return the HTTP status code from the server. As OctoPrint's
    API is incapable of waiting for the printer response without holding
    the entire webserver, we won't get more information than that.
  */
  int startJob();
  int cancelJob();
  int restartJob();
  int pauseJob();
  int resumeJob();
  int toggleJobPauseState();

  // File list + remote print. fetchFiles() GETs /api/files?recursive=true,
  // keeps only printable machinecode entries, sorts newest-first by the
  // server timestamp and fills up to capacity entries. It deliberately keeps
  // only type/path/name/date/size: the analysis/statistics blocks are most of
  // the response and are not needed to draw the list. Returns the stored
  // count, or -1 on transport/parse failure.
  //
  // fetchFileInfo() GETs /api/files/local/<path> for one entry and fills the
  // detail fields (material, times, dimensions, filament, print history) for
  // the info view. That is the second request the list no longer pays for.
  //
  // selectAndPrint() POSTs {"command":"select","print":true} to
  // /api/files/local/<path> and returns the HTTP status (204 on success, 0
  // when unreachable).
  //
  // diag always reports the outcome (FETCH_OK on success); parseCode carries
  // the ArduinoJson error code when diag == FETCH_PARSE, so the caller can
  // re-log failures without keeping strings around.
  enum FetchDiag : uint8_t
  {
    FETCH_OK = 0,
    FETCH_CONN,
    FETCH_HDRS,
    FETCH_PARSE,
    FETCH_NOFILES,
    FETCH_NOMEM
  };
  int fetchFiles(OctoFile *out, size_t capacity, FetchDiag &diag, uint8_t &parseCode);
  int fetchFileInfo(const char *path, OctoFile &out, FetchDiag &diag,
                    uint8_t &parseCode);
  int selectAndPrint(const char *path);

  // temperature control functions
  int preheatTool(int toolTemp);
  int preheatToolOff();
  // Bed target. Unlike the tool, 0 is just "target 0" (bed off), so there is no
  // separate off variant.
  int preheatBed(int bedTemp);

  // give server related information
  String serverVersion();
  String apiVersion();

  // temperature related functions
  double toolActual();
  int toolTarget();
  int toolOffset();
  double bedActual();
  int bedTarget();
  int bedOffset();
  double chamberActual();
  int chamberTarget();
  int chamberOffset();

private:
  WiFiClient _client;
  void _setTime(int, int);
  String _requester(String);
  String _poster(String, String);
  void _parsePrinter(String);
  void _parseSystem(String);
  void _parseJob(String);
  void _parseLayerProgress(String);
  String _parseConnection(String);
  void _parseProfile(String);
  String _parseNozzle(String json);
  int _parseFilament(String json);

  String _apiKey;
  IPAddress _host;
  String _hostAddress;
  int _port;
  bool _doOnce;

  struct
  {
    String _apiVersion;
    String _serverVersion;
  } _server;
  struct
  {
    jobTimer _elapsed;
    jobTimer _remaining;
    String _fileName;
    double _progress;
    double _rawProgress;
    String _filament;
    String _nozzle;
  } _job;

  struct layerProgress
  {
    int current;
    int total;
    String averageLayerDuration;
    int averageLayerDurationInSeconds;
    String lastLayerDuration;
    int lastLayerDurationInSeconds;
  } _layerProgress;
  struct print
  {
    int changeFilamentCount;
    String changeFilamentTimeLeft;
    int changeFilamentTimeLeftInSeconds;
    String estimatedChangedFilamentTime;
    String estimatedEndTime;
    String m73progress;
    String printerState;
    String progress;
    String timeLeft;
    int timeLeftInSeconds;
  } _print;
  struct
  {
    bool _operational;
    bool _paused;
    bool _printing;
    bool _cancelling;
    bool _pausing;
    bool _error;
    bool _ready;
    bool _closed;
    bool _closedOrError;
  } _is;
  struct
  {
    bool _heatedBed;
    bool _heatedChamber;
    int _toolCout;
  } _has;
  temperatureTracker _bed;
  temperatureTracker _chamber;
  temperatureTracker _tool;

  enum Filament
  {
    PLA,
    PETG,
    ABS,
    ASA,
    TPU,
    PC,
    NYLON,
    HIPS,
    PVA,
    WOOD,
    FLEX,
    UNKNOWN
  };
  String _filamentName[12] = {"PLA", "PETG", "ABS", "ASA", "TPU", "PC", "NYLON", "HIPS", "PVA", "WOOD", "FLEX", "UNKN"};

  enum Nozzle
  {
    NOZZLE_0_25,
    NOZZLE_0_4,
    NOZZLE_0_6,
    NOZZLE_0_8,
    NOZZLE_1_0,
    UNKNOWN_NOZZLE
  };
  String _nozzleName[6] = {"0.25n", "0.4n", "0.6n", "0.8n", "1.0n", "UNKN"};
};

#endif