#include <Arduino.h>
#include <ArduinoOTA.h>
#include <WiFi.h>

#include "const.h"
#include "display.h"
#include "poller.h"
#include "secrets.h"

unsigned long display_lasttime = 0;
Display disp;
secrets sec;
PrinterSnapshot snapshot;

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
    disp.drawWiFiSignal(WiFi.RSSI());
    disp.jobUpdate(snapshot);
    const bool activeJob = snapshot.printing || snapshot.paused;
    disp.setPreheatButtonsVisibility(!activeJob);
    disp.setPrintButtonsVisibility(activeJob);
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

    pollerStart();
    const uint32_t bootStart = millis();
    while (!pollerHasData() && (millis() - bootStart < 20000))
    {
        disp.splashScreen("Connecting to OctoPrint");
        delay(100);
    }

    ArduinoOTA.setHostname("enclosure-display");
    ArduinoOTA.setPassword(sec.getOTAPassword().c_str());
    ArduinoOTA.begin();
    disp.createLabes();
}

void loop()
{
    ArduinoOTA.handle();
    octoPrintUpdate();
    lv_tick_inc(5);
    lv_timer_handler();
    delay(5);
}
