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
  if (SERIAL_DEBUG)
  {
    Serial.println("Layer Progress JSON: " + json);
  }
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

  if (SERIAL_DEBUG)
  {
    Serial.println("Printer JSON: " + json);
  }
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

  JsonObject temperature_chamber = temperature["chamber"];
  if (temperature_chamber.isNull())
    temperature_chamber = temperature["enclosure"];
  if (temperature_chamber.isNull())
    temperature_chamber = temperature["Encosure temp"];
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
  if (SERIAL_DEBUG)
  {
    Serial.println("Job JSON: " + json);
  }
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
#if (SERIAL_DEBUG)
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
#if (SERIAL_DEBUG)
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

int OctoPrinter::preheat(int toolTemp)
{
  String command = "{\"command\": \"target\", \"targets\": {\"tool0\": " + String(toolTemp) + "}}";
  String response = _poster("/api/printer/tool", command);
  return response.toInt();
}

int OctoPrinter::preheatOff()
{
  String command = "{\"command\": \"target\", \"targets\": {\"tool0\": 0}}";
  String response = _poster("/api/printer/tool", command);
  return response.toInt();
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