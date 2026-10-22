#include "Arduino.h"
#include "OctoPrinter.h"
#include "WiFi.h"
#include "ArduinoJson.h"

// NOTE: WiFiClient::setTimeout() takes SECONDS, not milliseconds. Passing a
// millisecond-scale value there arms Stream::timedRead() - a tight loop that
// never yields - for thousands of seconds and trips the task watchdog.
// All waits below are available-driven with delay(1) plus an absolute
// deadline, so the IDLE tasks always run no matter how silent the server is.
#define OCTOPRINTER_CONNECT_TIMEOUT_MS 3000
#define OCTOPRINTER_REQUEST_TIMEOUT_MS 8000
// File operations are one bulk listing plus, on demand, one entry's details.
// The listing is still ~484 KB from the live server even when only the lightweight
// fields are kept: the filter shrinks the parsed document, not the wire body, so
// this needs a longer deadline than a status poll. Kept separate so a slow or
// dead file fetch cannot make every status poll wait the same 25 s.
#define OCTOPRINTER_FILES_TIMEOUT_MS 25000
#define OCTOPRINTER_READ_TIMEOUT_S 2

// Reads one LF-terminated line (CR/LF stripped), yielding while waiting.
// Returns true only for a complete line; false on disconnect or deadline.
static bool readLineYielding(WiFiClient &client, uint32_t deadlineMs, String &out)
{
  out = "";
  for (;;)
  {
    while (client.available() > 0)
    {
      const int c = client.read();
      if (c < 0)
        break;
      if (c == '\n')
        return true;
      if (c != '\r' && out.length() < 512)
        out += (char)c;
    }
    if (!client.connected() && client.available() == 0)
      return false;
    if ((int32_t)(millis() - deadlineMs) >= 0)
      return false;
    delay(1);
  }
}

// Stream adapter that blocks (yielding) until data arrives, the deadline
// passes, or the peer disconnects, so a deserializer never mistakes a TCP
// gap for end-of-input (Arduino Stream's -1 means "no data right now", which
// a socket returns constantly mid-transfer).
class DeadlineStream : public Stream
{
public:
  DeadlineStream(WiFiClient &client, uint32_t deadlineMs)
      : _client(client), _deadline(deadlineMs)
  {
  }
  int available() override { return _client.available(); }
  int read() override
  {
    while (!_client.available())
    {
      if (!_client.connected() || (int32_t)(millis() - _deadline) >= 0)
        return -1;
      delay(1);
    }
    const int c = _client.read();
    maybeYield();
    return c;
  }
  int peek() override
  {
    while (!_client.available())
    {
      if (!_client.connected() || (int32_t)(millis() - _deadline) >= 0)
        return -1;
      delay(1);
    }
    return _client.peek();
  }
  void flush() override {}
  size_t write(uint8_t) override { return 0; }

private:
  // YIELD BY BYTE COUNT, not only while the socket is idle.
  //
  // Waiting for data is only half the problem. Once the Wi-Fi RX buffer holds
  // data, the deserializer calls read() back-to-back and the loop above never
  // runs - so nothing yields, the idle task never gets scheduled, and the task
  // watchdog aborts the chip mid-parse:
  //
  //   [POLL] files refresh start
  //   E (78556) task_wdt: Aborting.
  //
  // which reaches the user as the display rebooting when they open the file list.
  // GET /api/files?recursive=true is ~75 KB of JSON and the parse alone is long
  // enough to blow the 5 s budget.
  //
  // Every 1 KB rather than every character: a delay(1) per character would
  // dominate the transfer, while a kilobyte is milliseconds of work even on a
  // poor link - comfortably inside the watchdog window.
  void maybeYield()
  {
    if (++_sinceYield >= kYieldEveryBytes)
    {
      _sinceYield = 0;
      delay(1);
    }
  }

  static const uint32_t kYieldEveryBytes = 1024;

  WiFiClient &_client;
  uint32_t _deadline;
  uint32_t _sinceYield = 0;
};

// Reads the response body the same yielding way: exactly contentLength bytes
// when announced, otherwise until close or deadline.
static void readBodyYielding(WiFiClient &client, uint32_t deadlineMs, int contentLength, String &out)
{
  out = "";
  if (contentLength > 0 && contentLength < 32768)
    out.reserve(contentLength);
  for (;;)
  {
    while (client.available() > 0 &&
           (contentLength < 0 || (int)out.length() < contentLength))
    {
      const int c = client.read();
      if (c < 0)
        break;
      out += (char)c;
    }
    if (contentLength >= 0 && (int)out.length() >= contentLength)
      return;
    if (!client.connected() && client.available() == 0)
      return;
    if ((int32_t)(millis() - deadlineMs) >= 0)
      return;
    delay(1);
  }
}

OctoPrinter::OctoPrinter(String apiKey, String hostAddress, int port)
{
  _apiKey = apiKey;
  _hostAddress = hostAddress;
  _port = port;
  _host.fromString(hostAddress);
  _doOnce = false;
}

void OctoPrinter::begin()
{
  if (!_doOnce)
  {
    _parseSystem(_requester("/api/version"));
    update();
    _doOnce = true;
  }
}

void OctoPrinter::setEndpoint(IPAddress host, int port)
{
  if ((uint32_t)host == (uint32_t)_host && port == _port)
    return;

  // A socket open against the old server would keep streaming its response into
  // the old host's parser while the next request targets the new one. Dropping
  // it here means a repoint mid-transfer costs one failed poll, not a corrupt
  // snapshot.
  _client.stop();
  _host = host;
  _hostAddress = host.toString();
  _port = port;
  // The cached version strings came from the server we are leaving.
  _doOnce = false;
  Serial.print(F("[OctoPrinter] endpoint -> "));
  Serial.print(_hostAddress);
  Serial.print(':');
  Serial.println(port);
}

void OctoPrinter::setApiKey(const char *key)
{
  const char *safe = key ? key : "";
  if (_apiKey == safe)
    return;

  // Drop the socket: a transfer started with the old key would keep streaming
  // into the parser while the next request presents different credentials.
  _client.stop();
  _apiKey = safe;
  _doOnce = false;
  Serial.print(F("[OctoPrinter] api key changed ("));
  Serial.print(_apiKey.length());
  Serial.println(F(" chars)"));
}

void OctoPrinter::update()
{
  _parseConnection(_requester("/api/connection"));
  // if (!_is._closed) {
  _parsePrinter(_requester("/api/printer?exclude=sd"));
  if (_is._printing || _is._paused)
  {
    _parseJob(_requester("/api/job"));
    _parseLayerProgress(_requester("/plugin/DisplayLayerProgress/values"));
  }
  // }
}

/*
  The following functions return printer states.
*/
bool OctoPrinter::operational()
{
  return _is._operational;
}

bool OctoPrinter::paused()
{
  return _is._paused;
}

bool OctoPrinter::printing()
{
  return _is._printing;
}

bool OctoPrinter::cancelling()
{
  return _is._cancelling;
}

bool OctoPrinter::pausing()
{
  return _is._pausing;
}

bool OctoPrinter::error()
{
  return _is._error;
}

bool OctoPrinter::ready()
{
  return _is._ready;
}

bool OctoPrinter::closedOrError()
{
  return _is._closedOrError;
}

String OctoPrinter::remainingFormatted()
{
  return _job._remaining.formatted;
}

String OctoPrinter::fileName()
{
  return _job._fileName;
}

String OctoPrinter::filamentName()
{
  return _job._filament;
}

String OctoPrinter::nozzleDiameter()
{
  return _job._nozzle;
}

int OctoPrinter::currentLayer()
{
  return _layerProgress.current;
}

int OctoPrinter::totalLayers()
{
  return _layerProgress.total;
}

String OctoPrinter::averageLayerDuration()
{
  return _layerProgress.averageLayerDuration;
}

String OctoPrinter::lastLayerDuration()
{
  return _layerProgress.lastLayerDuration;
}

int OctoPrinter::averageLayerDurationSeconds()
{
  return _layerProgress.averageLayerDurationInSeconds;
}

int OctoPrinter::lastLayerDurationSeconds()
{
  return _layerProgress.lastLayerDurationInSeconds;
}

// These functions are used to get the tool and bed temperature stats.

double OctoPrinter::toolActual()
{
  return _tool.actual;
}

int OctoPrinter::toolTarget()
{
  return _tool.target;
}

int OctoPrinter::toolOffset()
{
  return _tool.offset;
}

double OctoPrinter::bedActual()
{
  return _bed.actual;
}

int OctoPrinter::bedTarget()
{
  return _bed.target;
}

int OctoPrinter::bedOffset()
{
  return _bed.offset;
}

double OctoPrinter::chamberActual()
{
  return _chamber.actual;
}
int OctoPrinter::chamberTarget()
{
  return _chamber.target;
}
int OctoPrinter::chamberOffset()
{
  return _chamber.offset;
}

/*
  This handy dandy helper function takes the raw job times in seconds from OctoPrint
  and does a number of operations on them:
    1. Uses the elapsed and remaining times to calculate progress.
        Rather than trusting OctoPrint's calculated progress, we're making our own. The
        major benefit of doing it this way is accurately tracking the progress when a plugin
        (i.e. PrintTimeGenius) modifies the time estimate. OctoPrint doesn't report the
        progress based on the PTG times.
    2. Takes the raw elapsed and remaining times and breaks them down into days, hours, minutes,
       and seconds.
    3. Uses the numbers calculated in step 2 and constructs a formatted string, which can be
       fed directly into a print statement.
*/

void OctoPrinter::_setTime(int elapsed, int remaining)
{
  if (elapsed < 0)
    elapsed = 0;
  if (remaining < 0)
    remaining = 0;
  const int total = elapsed + remaining;
  if (total <= 0)
  {
    _job._rawProgress = 0.0;
    _job._progress = 0.0;
  }
  else
  {
    _job._rawProgress = (double)elapsed / (double)total;
    _job._progress = _job._rawProgress * 100.0;
    if (!isfinite(_job._progress))
      _job._progress = 0.0;
    _job._progress = constrain(_job._progress, 0.0, 100.0);
    _job._rawProgress = _job._progress / 100.0;
  }

  _job._elapsed.raw = elapsed;
  _job._elapsed.days = elapsed / (24 * 60 * 60);
  _job._elapsed.hours = (elapsed % (24 * 60 * 60)) / (60 * 60);
  _job._elapsed.minutes = (elapsed % (60 * 60)) / 60;
  _job._elapsed.seconds = elapsed % 60;
  _job._elapsed.formatted = "";
  if (_job._elapsed.days > 0)
  {
    _job._elapsed.formatted += String(_job._elapsed.days);
    _job._elapsed.formatted += ":";
  }
  _job._elapsed.formatted += (_job._elapsed.hours < 10) ? "0" : "";
  _job._elapsed.formatted += String(_job._elapsed.hours);
  _job._elapsed.formatted += (_job._elapsed.minutes < 10) ? ":0" : ":";
  _job._elapsed.formatted += String(_job._elapsed.minutes);
  _job._elapsed.formatted += (_job._elapsed.seconds < 10) ? ":0" : ":";
  _job._elapsed.formatted += String(_job._elapsed.seconds);

  _job._remaining.raw = remaining;
  _job._remaining.days = remaining / (24 * 60 * 60);
  _job._remaining.hours = (remaining % (24 * 60 * 60)) / (60 * 60);
  _job._remaining.minutes = (remaining % (60 * 60)) / 60;
  _job._remaining.seconds = remaining % 60;
  _job._remaining.formatted = "";
  if (_job._remaining.days > 0)
  {
    _job._remaining.formatted += String(_job._remaining.days);
    _job._remaining.formatted += ":";
  }
  _job._remaining.formatted += (_job._remaining.hours < 10) ? "0" : "";
  _job._remaining.formatted += String(_job._remaining.hours);
  _job._remaining.formatted += (_job._remaining.minutes < 10) ? ":0" : ":";
  _job._remaining.formatted += String(_job._remaining.minutes);
  _job._remaining.formatted += (_job._remaining.seconds < 10) ? ":0" : ":";
  _job._remaining.formatted += String(_job._remaining.seconds);
}

String OctoPrinter::_requester(String uri)
{
  _client.setTimeout(OCTOPRINTER_READ_TIMEOUT_S);
  const uint32_t deadlineMs = millis() + OCTOPRINTER_REQUEST_TIMEOUT_MS;
  if (!_client.connect(_host, _port, OCTOPRINTER_CONNECT_TIMEOUT_MS))
  {
    Serial.println("Connection failed");
    return "";
  }
  else
  {
    _client.println("GET " + uri + " HTTP/1.1");
    _client.println("Host: " + _hostAddress);
    _client.println("Cache-Control: no-cache");
    _client.println("Connection: close");
    _client.println("X-Api-Key: " + _apiKey);
    _client.println("");

    int contentLength = -1;
    bool headersComplete = false;
    String line;
    while (readLineYielding(_client, deadlineMs, line))
    {
      if (line.length() == 0)
      {
        headersComplete = true;
        break;
      }
      if (line.startsWith("Content-Length:"))
      {
        contentLength = line.substring(15).toInt();
      }
    }
    String response;
    if (headersComplete)
    {
      readBodyYielding(_client, deadlineMs, contentLength, response);
    }
    _client.stop();

    // if (serialVerbose)
    // {
    // Serial.println(response);
    // }
    return response;
  }
}

String OctoPrinter::_poster(String uri, String body)
{
  _client.setTimeout(OCTOPRINTER_READ_TIMEOUT_S);
  const uint32_t deadlineMs = millis() + OCTOPRINTER_REQUEST_TIMEOUT_MS;
  if (!_client.connect(_host, _port, OCTOPRINTER_CONNECT_TIMEOUT_MS))
  {
    return "ERROR";
  }
  else
  {
    _client.println("POST " + uri + " HTTP/1.1");
    _client.println("Host: " + _hostAddress);
    _client.println("Cache-Control: no-cache");
    _client.println("Connection: close");
    _client.println("X-Api-Key: " + _apiKey);
    _client.println("Content-Type: application/json");
    _client.print("Content-Length: ");
    _client.println(body.length());
    _client.println("");
    _client.println(body);
    _client.println("");

    String response;
    String line;
    while (readLineYielding(_client, deadlineMs, line))
    {
      if (line.length() == 0)
      {
        break;
      }
      if (line.startsWith("HTTP"))
      {
        response = line.substring(9, 12);
      }
    }

    _client.stop();
    return response;
  }
}

void OctoPrinter::_parseLayerProgress(String json)
{
  if (json.length() == 0)
    return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error)
  {
    Serial.print(F("Failed to parse JSON: "));
    Serial.println(error.c_str());
    return;
  }
#if OCTO_DEBUG_LOG
    Serial.println("Layer Progress JSON: " + json);
#endif
  JsonObject layer = doc["layer"];
  if (layer.isNull())
    return;
  // DisplayLayerProgress sends current/total as strings ("38") on some
  // versions and numbers (38) on others - accept both.
  _layerProgress.current = layer["current"].as<String>().toInt();
  _layerProgress.total = layer["total"].as<String>().toInt();

  const char *averageLayerDuration = layer["averageLayerDuration"] | "";
  _layerProgress.averageLayerDuration = String(averageLayerDuration);
  _layerProgress.averageLayerDurationInSeconds = layer["averageLayerDurationInSeconds"] | 0;

  const char *lastLayerDuration = layer["lastLayerDuration"] | "";
  _layerProgress.lastLayerDuration = String(lastLayerDuration);
  _layerProgress.lastLayerDurationInSeconds = layer["lastLayerDurationInSeconds"] | 0;
}

void OctoPrinter::_parsePrinter(String json)
{
  if (json.length() == 0)
    return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error)
  {
    return;
  }

#if OCTO_DEBUG_LOG
    Serial.println("Printer JSON: " + json);
#endif
  JsonObject temperature = doc["temperature"];
  if (temperature.isNull())
    return;

  JsonObject temperature_tool0 = temperature["tool0"];
  _tool.actual = temperature_tool0["actual"] | 0.0;
  _tool.target = temperature_tool0["target"] | 0.0;
  _tool.offset = temperature_tool0["offset"] | 0.0;
  JsonObject temperature_bed = temperature["bed"];
  _bed.actual = temperature_bed["actual"] | 0.0;
  _bed.target = temperature_bed["target"] | 0.0;
  _bed.offset = temperature_bed["offset"] | 0.0;

  // Chamber temperature, tried under three names in order.
  //
  // "chamber" is the only STANDARD one: OctoPrint reports that key as soon as a
  // chamber sensor is configured, and it is the only entry here that is not
  // specific to one particular printer.
  //
  // The other two are custom-sensor names, and the casing matters - JSON keys are
  // case-sensitive, so "enclosure" will never match a sensor called "Enclosure".
  // That gap cost real time: on the printer this was written for, /api/printer
  // reports NEITHER "chamber" NOR lowercase "enclosure", so the whole chamber
  // reading rests on the third name below. The typo in it ("Encosure temp") was
  // likewise load-bearing, and invisible, because it still matched a sensor that
  // really existed under that misspelling.
  //
  // So the third name is not decoration - it is the one carrying the reading on
  // a printer without a standard chamber sensor, and it must match the sensor's
  // EXACT spelling. If that sensor is renamed or removed in OctoPrint, update it
  // here in the same breath: with no match, _chamber.actual silently becomes 0.0,
  // which reads as "no data" to the caller and trips the enclosure cooling
  // fail-safe mid-print. A duplicate sensor left behind does not break anything
  // (both report the same value), so deleting the old misspelled one is safe
  // once this firmware is running.
  //
  // The robust fix is to configure a real OctoPrint chamber sensor so the first
  // lookup answers and these custom names stop mattering at all.
  JsonObject temperature_chamber = temperature["chamber"];
  if (temperature_chamber.isNull())
    temperature_chamber = temperature["enclosure"];
  if (temperature_chamber.isNull())
    temperature_chamber = temperature["Enclosure temp"];
  _chamber.actual = temperature_chamber["actual"] | 0.0;
  _chamber.target = temperature_chamber["target"] | 0.0;
  _chamber.offset = temperature_chamber["offset"] | 0.0;

  JsonObject state_flags = doc["state"]["flags"];
  _is._operational = state_flags["operational"] | false;
  _is._paused = state_flags["paused"] | false;
  _is._printing = state_flags["printing"] | false;
  _is._cancelling = state_flags["cancelling"] | false;
  _is._pausing = state_flags["pausing"] | false;
  _is._error = state_flags["error"] | false;
  _is._ready = state_flags["ready"] | false;
  _is._closedOrError = state_flags["closedOrError"] | false;
}

void OctoPrinter::_parseSystem(String json)
{
  if (json.length() == 0)
    return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error)
  {
    Serial.print(F("Failed to parse JSON: "));
    Serial.println(error.c_str());
    Serial.print(F("Error code: "));
    Serial.println(error.code());
    return;
  }
  const char *serverAPIVersion = doc["api"]; // "0.1"
  const char *serverVersion = doc["server"];

  if (serverAPIVersion == nullptr || serverVersion == nullptr)
  {
    Serial.println(F("System response is missing api or server fields"));
    return;
  }

  _server._serverVersion = String(serverVersion);
  _server._apiVersion = String(serverAPIVersion);
}

void OctoPrinter::_parseJob(String json)
{
  if (json.length() == 0)
    return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error)
  {
    Serial.print(F("Failed to parse JSON: "));
    Serial.println(error.c_str());
    return;
  }
#if OCTO_DEBUG_LOG
    Serial.println("Job JSON: " + json);
#endif
  JsonObject progress = doc["progress"];
  if (progress.isNull())
  {
    return;
  }
  _setTime(progress["printTime"] | 0, progress["printTimeLeft"] | 0);

  // OctoPrint nests the file under "job" ({"job": {"file": {"name": ...}}, ...});
  // fall back to a top-level "file" object for tolerance.
  JsonObject file = doc["job"]["file"];
  if (file.isNull())
    file = doc["file"];
  const char *fileName = file["name"] | "";
  _job._fileName = String(fileName);
  _job._filament = _filamentName[_parseFilament(json)];
  _job._nozzle = _parseNozzle(json);
}

int OctoPrinter::_parseFilament(String json)
{
  for (int i = 0; i < 11; i++)
  {
    if (json.indexOf(_filamentName[i]) != -1)
    {
#if OCTO_DEBUG_LOG
        Serial.println("Parsed filament: " + _filamentName[i]);
#endif
      return i;
    }
  }
  return 11; // UNKNOWN
}

String OctoPrinter::_parseNozzle(String json)
{
  for (int i = 0; i < 5; i++)
  {
    if (json.indexOf(_nozzleName[i]) != -1)
    {
#if OCTO_DEBUG_LOG
        Serial.println("Parsed nozzle: " + _nozzleName[i]);
#endif
      return _nozzleName[i];
    }
  }
  return "0.4"; // Default nozzle size
}

String OctoPrinter::_parseConnection(String json)
{
  if (json.length() == 0)
    return "";
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, json);

  if (error)
  {
    return "";
  }
  const char *currentPrinterProfile = doc["printerProfile"] | "";
  JsonObject current = doc["current"];
  if (current.isNull())
    return "";
  const char *currentState = current["state"] | "";
  if (currentState == nullptr || currentState[0] == '\0')
  {
    return "";
  }
  if (String(currentState) != "Closed")
  {
    _is._closed = false;
  }
  else
  {
    _is._closed = true;
  }
  String response = String(currentPrinterProfile);
  return response;
}

String OctoPrinter::serverVersion()
{
  return _server._serverVersion;
}

String OctoPrinter::apiVersion()
{
  return _server._apiVersion;
}

int OctoPrinter::preheatTool(int toolTemp)
{
  String command = "{\"command\": \"target\", \"targets\": {\"tool0\": " + String(toolTemp) + "}}";
  String response = _poster("/api/printer/tool", command);
  return response.toInt();
}

int OctoPrinter::preheatToolOff()
{
  String command = "{\"command\": \"target\", \"targets\": {\"tool0\": 0}}";
  String response = _poster("/api/printer/tool", command);
  return response.toInt();
}

int OctoPrinter::preheatBed(int bedTemp)
{
  // NOTE: the bed endpoint does NOT mirror the tool's body shape. /api/printer/tool
  // takes a "targets" map keyed by tool name ({"targets": {"tool0": N}}), but
  // /api/printer/bed takes a scalar "target". Sending the tool's shape here is
  // rejected with HTTP 400 "Mandatory parameters missing". 0 turns the bed off,
  // so it needs no separate call.
  String command = "{\"command\": \"target\", \"target\": " + String(bedTemp) + "}";
  String response = _poster("/api/printer/bed", command);
  return response.toInt();
}

int OctoPrinter::sendGcode(const char *command)
{
  if (!command || !*command)
    return 0;

  // The body is JSON, so the two characters that could break out of the string
  // are escaped. The lines this is used for (M25, M84) contain neither, but a
  // command that did would otherwise be turned into a malformed request that
  // OctoPrint rejects with a 400 and no explanation.
  String body = "{\"command\": \"";
  for (const char *p = command; *p; ++p)
  {
    if (*p == '"' || *p == '\\')
      body += '\\';
    body += *p;
  }
  body += "\"}";

  // 204 on success. OctoPrint answers 409 when the printer is not operational
  // and 400 for a body it does not like; both are returned to the caller so
  // the failure is visible instead of silent.
  return _poster("/api/printer/command", body).toInt();
}

String OctoPrinter::Status()
{
  if (_is._closed)
  {
    return "Closed";
  }
  else if (_is._printing)
  {
    return "Printing";
  }
  else if (_is._paused)
  {
    return "Paused";
  }
  else if (_is._cancelling)
  {
    return "Cancelling";
  }
  else if (_is._pausing)
  {
    return "Pausing";
  }
  else if (_is._error)
  {
    return "Error";
  }
  else if (_is._ready)
  {
    return "Ready";
  }
  return "";
}

double OctoPrinter::progress()
{
  return _job._progress;
}

bool OctoPrinter::closed()
{
  return _is._closed;
}

int OctoPrinter::startJob()
{
  String response = _poster("/api/job", "{\"command\": \"start\"}");
  return response.toInt();
}

int OctoPrinter::cancelJob()
{
  String response = _poster("/api/job", "{\"command\": \"cancel\"}");
  return response.toInt();
}

int OctoPrinter::restartJob()
{
  String response = _poster("/api/job", "{\"command\": \"restart\"}");
  return response.toInt();
}

int OctoPrinter::pauseJob()
{
  return _poster("/api/job", "{\"command\": \"pause\", \"action\": \"pause\"}").toInt();
}

int OctoPrinter::resumeJob()
{
  return _poster("/api/job", "{\"command\": \"pause\", \"action\": \"resume\"}").toInt();
}

int OctoPrinter::toggleJobPauseState()
{
  return _poster("/api/job", "{\"command\": \"pause\", \"action\": \"toggle\"}").toInt();
}

// Percent-encodes a file path for the POST URI, leaving '/' separators and
// unreserved characters alone.
static String urlEncodePath(const char *path)
{
  String out;
  out.reserve(strlen(path) + 8);
  char hex[4];
  for (const char *p = path; *p; ++p)
  {
    const unsigned char c = static_cast<unsigned char>(*p);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
      out += static_cast<char>(c);
    else
    {
      snprintf(hex, sizeof(hex), "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

// Heuristic hints read out of a file name.
//
// WHY THIS EXISTS: layer height and nozzle diameter are the two slicer settings
// OctoPrint does not expose anywhere. Checked against the live server: neither
// /api/files nor /api/files/local/<path> carries them, not even in the embedded
// slicer metadata. The name is all that is left, and it is where PrusaSlicer,
// Orca and Cura all put them by default.
//
// WHAT IT IS NOT: authoritative. It is a guess from a naming convention, so the
// UI marks these two values as coming from the name. A file called
// spare-part.gcode yields nothing, and 0 here means not-found, not a real zero.

static bool isNameBoundary(char c)
{
  return !(isalnum((unsigned char)c) || c == '_' || c == '.');
}

// Materials worth recognising. Ordered so the longer names are tried before the
// shorter ones they start with; PC is last and matches only as a whole token.
static const char *const kMaterialOrder[] = {"NYLON", "HIPS", "WOOD", "FLEX", "PETG",
                                             "POLY",  "PVA",  "TPU",  "ABS",  "ASA",
                                             "PLA",   "PC",   nullptr};

static bool findMaterial(const char *name, char *out, size_t outSize)
{
  for (int i = 0; kMaterialOrder[i]; ++i)
  {
    const size_t len = strlen(kMaterialOrder[i]);
    for (const char *p = name; *p; ++p)
    {
      if (strncasecmp(p, kMaterialOrder[i], len) != 0)
        continue;
      const bool leftOk = (p == name) || isNameBoundary(p[-1]);
      if (!leftOk || !isNameBoundary(p[len]))
        continue;
      strncpy(out, kMaterialOrder[i], outSize - 1);
      out[outSize - 1] = '\0';
      return true;
    }
  }
  return false;
}

// Collects every <number>mm in the name. Both a dot and a comma are accepted as
// the decimal separator, because European slicer locales write 0,15mm.
static int scanMmValues(const char *name, float *out, int maxOut)
{
  int found = 0;
  for (const char *p = name; *p && found < maxOut; ++p)
  {
    if (!isdigit((unsigned char)*p))
      continue;
    char *end = nullptr;
    float value = strtof(p, &end);
    if (end == p || value <= 0)
      continue;
    if (*end == ',' || *end == '.')
    {
      // A locale decimal comma: re-parse with that separator honoured.
      value = strtof(p, &end);
      if (end == p || value <= 0)
        continue;
    }
    if (strncasecmp(end, "mm", 2) != 0)
      continue;
    if (end - p > 8)
      continue; // too many digits to be a dimension we care about
    out[found++] = value;
    p = end + 1; // resume past the digits, before the unit
  }
  return found;
}

static void parseNameHints(const char *name, OctoFile &entry)
{
  if (entry.material[0] == '\0')
    findMaterial(name, entry.material, sizeof(entry.material));

  float mm[8];
  const int count = scanMmValues(name, mm, 8);
  if (count == 0)
    return;

  // Distinguish the two by size, not by keyword. Layer heights run about
  // 0.05-0.35 and nozzles about 0.2-0.8, so the ranges overlap and a keyword
  // alone is not reliable. With two or more values the ordering is unambiguous:
  // smallest is the layer, largest the nozzle.
  if (count >= 2)
  {
    float lo = mm[0], hi = mm[0];
    for (int i = 1; i < count; ++i)
    {
      if (mm[i] < lo)
        lo = mm[i];
      if (mm[i] > hi)
        hi = mm[i];
    }
    if (lo > 0.001f && lo < hi)
    {
      entry.layerHeightMm = lo;
      entry.nozzleMm = hi;
    }
    else
    {
      entry.layerHeightMm = mm[0];
    }
    return;
  }

  // A single value: 0.4 on its own is far more often a nozzle, and 0.15 on its
  // own is obviously a layer height.
  if (mm[0] < 0.36f)
    entry.layerHeightMm = mm[0];
  else
    entry.nozzleMm = mm[0];
}

// Inserts one entry into the out[0..*found] array kept sorted newest-first.
// When full, only displaces the oldest entry if the newcomer is newer.
static void insertFileSorted(OctoFile *out, size_t capacity, size_t &found,
                             const OctoFile &entry)
{
  size_t slot;
  if (found < capacity)
  {
    slot = found++;
  }
  else
  {
    if (entry.date <= out[found - 1].date)
      return;
    slot = found - 1;
  }
  out[slot] = entry;
  while (slot > 0 && out[slot].date > out[slot - 1].date)
  {
    OctoFile tmp = out[slot];
    out[slot] = out[slot - 1];
    out[slot - 1] = tmp;
    --slot;
  }
}

// The lightweight fields every listing entry needs. Returns false for folders
// and non-printable entries, which the list skips.
static bool fillCommonFileFields(JsonObject obj, OctoFile &entry)
{
  const char *type = obj["type"] | "";
  if (strcmp(type, "machinecode") != 0)
    return false;
  const char *path = obj["path"] | "";
  if (!path || !*path)
    return false;

  entry = {};
  strncpy(entry.path, path, sizeof(entry.path) - 1);
  const char *name = obj["name"] | "";
  strncpy(entry.name, (name && *name) ? name : path, sizeof(entry.name) - 1);
  entry.date = obj["date"] | 0;
  entry.size = obj["size"] | 0;
  return true;
}

// The heavy blocks: gcodeAnalysis, statistics and prints. Only parsed for the
// one file whose info panel is open, never for the whole list.
static void parseFileDetails(JsonObject obj, OctoFile &entry)
{
  // Each block is guarded because a file sliced by something other than
  // PrusaSlicer/Orca has no analysis at all, and one absent field must not lose
  // the rest.
  JsonObject analysis = obj["gcodeAnalysis"].as<JsonObject>();
  if (!analysis.isNull())
  {
    entry.estimatedPrintTime = analysis["estimatedPrintTime"] | 0.0;
    const char *mat = analysis["firstFilament"] | "";
    if (mat && *mat)
      strncpy(entry.material, mat, sizeof(entry.material) - 1);

    JsonObject dims = analysis["dimensions"].as<JsonObject>();
    if (!dims.isNull())
    {
      entry.dimX = dims["X"] | 0.0f;
      entry.dimY = dims["Y"] | 0.0f;
      entry.dimZ = dims["Z"] | 0.0f;
    }

    // filament is keyed per tool; the enclosure is single-extruder and the
    // display has room for one total, so tools are summed rather than tool0
    // being picked - a two-tool file would otherwise under-report by half.
    // The single-file endpoint returns every tool, unlike the filtered list.
    JsonObject fil = analysis["filament"].as<JsonObject>();
    if (!fil.isNull())
    {
      for (JsonPair kv : fil)
      {
        JsonObject tool = kv.value().as<JsonObject>();
        if (tool.isNull())
          continue;
        entry.filamentLengthMm += tool["length"] | 0.0;
        entry.filamentVolumeCm3 += tool["volume"] | 0.0;
      }
    }
  }

  JsonObject stats = obj["statistics"].as<JsonObject>();
  if (!stats.isNull())
  {
    JsonObject avg = stats["averagePrintTime"].as<JsonObject>();
    if (!avg.isNull())
      entry.averagePrintTime = avg["_default"] | 0.0;
  }
  JsonObject prints = obj["prints"].as<JsonObject>();
  if (!prints.isNull())
  {
    entry.printsSuccess = prints["success"] | 0;
    entry.printsFailure = prints["failure"] | 0;
  }
}

// Parses one object into an entry. The list passes withDetails=false because it
// never displays these fields; the info fetch passes true.
static bool parseFileObject(JsonObject obj, OctoFile &entry, bool withDetails)
{
  if (!fillCommonFileFields(obj, entry))
    return false;
  if (withDetails)
    parseFileDetails(obj, entry);

  // Name-derived hints LAST, and only as a fallback: real slicer metadata wins,
  // this is a guess from a naming convention.
  parseNameHints(entry.name, entry);
  return true;
}

// Walks one files[] level (top level or a folder's children), recursing into
// folders and collecting printable machinecode entries.
static void collectFiles(JsonArray level, OctoFile *out, size_t capacity, size_t &found)
{
  for (JsonVariant item : level)
  {
    JsonObject obj = item.as<JsonObject>();
    if (obj.isNull())
      continue;
    const char *type = obj["type"] | "";
    if (strcmp(type, "folder") == 0)
    {
      JsonArray children = obj["children"].as<JsonArray>();
      if (!children.isNull())
        collectFiles(children, out, capacity, found);
      continue;
    }

    OctoFile entry;
    if (parseFileObject(obj, entry, false))
      insertFileSorted(out, capacity, found, entry);
  }
}

int OctoPrinter::fetchFiles(OctoFile *out, size_t capacity, FetchDiag &diag, uint8_t &parseCode)
{
  diag = FETCH_NOFILES;
  parseCode = 0;
  if (!out || capacity == 0)
    return -1;

  // The full recursive listing is ~484 KB on the live server (gcodeAnalysis,
  // thumbnails, refs, statistics per file) - far beyond what fits in a String
  // plus a full JsonDocument on this heap. So the body is deserialized straight
  // from the socket through a field filter keeping only what the lightweight
  // display fields need; the parsed document stays around 11 KB even for a
  // hundred files. The transfer is unchanged, which is why the fetch has its own
  // longer deadline.
  //
  // The [0] element templates every array item (see parseArray). Children
  // nesting is written out to 4 levels; anything deeper is skipped by the
  // filter (its "type" never materializes, so collectFiles ignores it).
  //
  // Build ONLY through handles to real storage (to<>()/add<>()): resolving an
  // intermediate proxy into a JsonVariant first (e.g. `level =
  // filter["files"][0]`) yields a detached null handle whose writes go
  // nowhere, leaving an empty filter that parses every reply down to {}.
  JsonDocument filter;
  JsonVariant level = filter["files"].to<JsonArray>().add<JsonVariant>();
  for (int depth = 0; depth < 4; ++depth)
  {
    level["type"] = true;
    level["path"] = true;
    level["name"] = true;
    level["date"] = true;
    level["size"] = true;
    level = level["children"].to<JsonArray>().add<JsonVariant>();
  }
  if (filter["files"].isNull())
  {
    Serial.println(F("[FILES] filter build failed"));
    diag = FETCH_PARSE;
    return -1;
  }

  _client.setTimeout(OCTOPRINTER_READ_TIMEOUT_S);
  const uint32_t deadlineMs = millis() + OCTOPRINTER_FILES_TIMEOUT_MS;
  if (!_client.connect(_host, _port, OCTOPRINTER_CONNECT_TIMEOUT_MS))
  {
    Serial.println(F("[FILES] connect failed"));
    diag = FETCH_CONN;
    return -1;
  }
  _client.println("GET /api/files?recursive=true HTTP/1.1");
  _client.println("Host: " + _hostAddress);
  _client.println("Cache-Control: no-cache");
  _client.println("Connection: close");
  _client.println("X-Api-Key: " + _apiKey);
  _client.println("");

  String line;
  bool headersComplete = false;
  while (readLineYielding(_client, deadlineMs, line))
  {
    if (line.length() == 0)
    {
      headersComplete = true;
      break;
    }
  }
  int found = -1;
  if (headersComplete)
  {
    DeadlineStream body(_client, deadlineMs);
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err)
    {
      parseCode = static_cast<uint8_t>(err.code());
      diag = FETCH_PARSE;
      Serial.printf("[FILES] stream parse fail %s heap=%u maxblock=%u\n",
                    err.c_str(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
    }
    else
    {
      JsonArray roots = doc["files"].as<JsonArray>();
      if (roots.isNull())
      {
        diag = FETCH_NOFILES;
        Serial.println(F("[FILES] no files[]"));
      }
      else
      {
        size_t n = 0;
        collectFiles(roots, out, capacity, n);
        found = static_cast<int>(n);
        diag = FETCH_OK;
        Serial.printf("[FILES] ok count=%d heap=%u\n", found, ESP.getFreeHeap());
      }
    }
  }
  else
  {
    diag = FETCH_HDRS;
    Serial.println(F("[FILES] headers incomplete"));
  }
  _client.stop();
  return found;
}

int OctoPrinter::fetchFileInfo(const char *path, OctoFile &out,
                               FetchDiag &diag, uint8_t &parseCode)
{
  diag = FETCH_NOFILES;
  parseCode = 0;
  if (!path || !*path)
    return -1;

  // One entry's details, still filtered: the response also carries thumbnails
  // and refs, which the info panel never reads.
  JsonDocument filter;
  filter["type"] = true;
  filter["path"] = true;
  filter["name"] = true;
  filter["date"] = true;
  filter["size"] = true;
  JsonObject analysis = filter["gcodeAnalysis"].to<JsonObject>();
  analysis["estimatedPrintTime"] = true;
  analysis["firstFilament"] = true;
  analysis["dimensions"] = true;
  // true keeps the whole object, so every tool survives. The list filter cannot
  // do this because it does not know the tool count; one file is cheap enough.
  analysis["filament"] = true;
  JsonObject statistics = filter["statistics"].to<JsonObject>();
  JsonObject average = statistics["averagePrintTime"].to<JsonObject>();
  average["_default"] = true;
  JsonObject prints = filter["prints"].to<JsonObject>();
  prints["success"] = true;
  prints["failure"] = true;

  _client.setTimeout(OCTOPRINTER_READ_TIMEOUT_S);
  const uint32_t deadlineMs = millis() + OCTOPRINTER_FILES_TIMEOUT_MS;
  if (!_client.connect(_host, _port, OCTOPRINTER_CONNECT_TIMEOUT_MS))
  {
    Serial.println(F("[INFO] connect failed"));
    diag = FETCH_CONN;
    return -1;
  }
  _client.println("GET /api/files/local/" + urlEncodePath(path) + " HTTP/1.1");
  _client.println("Host: " + _hostAddress);
  _client.println("Cache-Control: no-cache");
  _client.println("Connection: close");
  _client.println("X-Api-Key: " + _apiKey);
  _client.println("");

  String line;
  bool headersComplete = false;
  while (readLineYielding(_client, deadlineMs, line))
  {
    if (line.length() == 0)
    {
      headersComplete = true;
      break;
    }
  }

  int result = -1;
  if (headersComplete)
  {
    DeadlineStream body(_client, deadlineMs);
    JsonDocument doc;
    DeserializationError err =
        deserializeJson(doc, body, DeserializationOption::Filter(filter));
    if (err)
    {
      parseCode = static_cast<uint8_t>(err.code());
      diag = FETCH_PARSE;
      Serial.printf("[INFO] parse fail %s heap=%u\n", err.c_str(),
                    ESP.getFreeHeap());
    }
    else
    {
      JsonObject obj = doc.as<JsonObject>();
      OctoFile entry;
      if (!obj.isNull() && parseFileObject(obj, entry, true))
      {
        out = entry;
        diag = FETCH_OK;
        result = 1;
      }
      else
      {
        diag = FETCH_NOFILES;
        Serial.println(F("[INFO] no file object"));
      }
    }
  }
  else
  {
    diag = FETCH_HDRS;
    Serial.println(F("[INFO] headers incomplete"));
  }
  _client.stop();
  return result;
}

int OctoPrinter::selectAndPrint(const char *path)
{
  if (!path || !*path)
    return 0;
  String response = _poster("/api/files/local/" + urlEncodePath(path),
                            "{\"command\":\"select\",\"print\":true}");
  return response.toInt();
}