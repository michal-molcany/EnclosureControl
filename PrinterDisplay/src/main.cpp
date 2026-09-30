#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#include "HttpOta.h"
#include "const.h"
#include "display.h"
#include "poller.h"
#include "enclosure_link.h"
#include "secrets.h"

unsigned long display_lasttime = 0;
unsigned long wifi_lasttime = 0;
unsigned long enclosure_lasttime = 0;
bool enclosure_seeded = false;
Display disp;
secrets sec;
PrinterSnapshot snapshot;

// The splash overlay stays up while lib/HttpOta reports busy (uploading, or
// briefly after a failure so the reason is readable), then comes off.
static bool otaSplashUp = false;

static void otaStatus(const char *text)
{
    disp.splashScreen(String("OTA ") + text);
    otaSplashUp = true;
}

void preheat215()
{
    pollerSendCommand(PREHEAT_215);
}
void preheat230()
{
    pollerSendCommand(PREHEAT_230);
}
void preheatOff()
{
    pollerSendCommand(PREHEAT_OFF);
}
void pausePrint()
{
    pollerSendCommand(PAUSE_PRINT_BUTTON);
}
void stopPrint()
{
    pollerSendCommand(STOP_PRINT_BUTTON);
}
void resumePrint()
{
    pollerSendCommand(RESUME_PRINT_BUTTON);
}

void octoPrintUpdate()
{
    // The network lives on the poll task; here we only copy the latest
    // snapshot (never blocking on a socket) and repaint from it.
    if (millis() - display_lasttime <= DISPLAY_REFRESH_TIME && display_lasttime != 0)
        return;
    display_lasttime = millis();
    if (!pollerCopySnapshot(snapshot))
        return;
    disp.printerStatisticUpdate(snapshot);
    if (wifi_lasttime == 0 || millis() - wifi_lasttime >= WIFI_SIGNAL_REFRESH_TIME)
    {
        wifi_lasttime = millis();
        disp.drawWiFiSignal(WiFi.RSSI());
    }
    disp.jobUpdate(snapshot);
    const bool activeJob = snapshot.printing || snapshot.paused;
    disp.setJobActive(activeJob);

    // Enclosure link: feed live louvre/fan telemetry into the chamber view
    // (throttled to 1 Hz; stale data is greyed out by the view itself).
    if (millis() - enclosure_lasttime >= 1000)
    {
        enclosure_lasttime = millis();
        EnclosureSnapshot enc;
        if (enclosureLinkCopy(enc))
            disp.enclosureUpdate(enc);
    }
    // Seed the C3 with current UI state once, so it learns mode + setpoint
    // even if the user never touches the chamber view. Heartbeat repeats it.
    if (!enclosure_seeded)
    {
        enclosure_seeded = true;
        EnclosureCommand cmd = {};
        disp.fillEnclosureCommand(cmd);
        enclosureLinkSend(cmd);
    }
}

void setup()
{
    Serial.begin(115200);
    disp.Initialization();
    disp.setButtonActions(preheat215, preheat230, preheatOff, pausePrint, stopPrint, resumePrint);

    WiFi.mode(WIFI_STA);
    WiFi.begin(sec.getWiFiSSID(), sec.getWiFiPassword());
    disp.splashScreen("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(100);
        disp.splashScreen("Connecting to WiFi");
    }

    // Before the link starts: the link task calls esp_now_init() as its first
    // action, so power save still enabled at that point would leave the radio in
    // a power-saving state for the rest of the session and hurt ESP-NOW latency.
    WiFi.setSleep(false); // mains-powered: minimize ESP-NOW latency

    pollerStart();
    enclosureLinkStart(ENC_PEER_MAC);

    const uint32_t bootStart = millis();
    while (!pollerHasData() && (millis() - bootStart < 20000))
    {
        disp.splashScreen("Connecting to OctoPrint");
        delay(100);
    }

    ArduinoOTA.setHostname(OTA_ARDUINO_HOSTNAME);
    ArduinoOTA.setPassword(sec.getOTAPassword().c_str());
    ArduinoOTA.begin();
    // Primary update path: the board listens and the uploader POSTs to it, so
    // only outbound traffic is needed (ArduinoOTA is blocked by the host
    // firewall - see the README).
    HttpOta::begin(OTA_HTTP_PORT, OTA_HTTP_USER, sec.getOTAPassword().c_str(),
                   SERIAL_DEBUG != 0, otaStatus);
    disp.createLabes();
}

void loop()
{
    ArduinoOTA.handle();
    HttpOta::poll();
    // A failed upload is not a reboot, so put the main view back once the
    // status hold expires. A successful one reboots and never gets here.
    if (otaSplashUp && !HttpOta::busy())
    {
        disp.hideSplash();
        otaSplashUp = false;
    }
    octoPrintUpdate();
    lv_tick_inc(5);
    lv_timer_handler();
    delay(5);
}
