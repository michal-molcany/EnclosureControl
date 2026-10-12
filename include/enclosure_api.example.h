#ifndef ENCLOSURE_API_H
#define ENCLOSURE_API_H

// Settings for the enclosure HTTP API (lib/EnclosureApi). SHARED BY BOTH
// FIRMwares - EnclosureFanControl (the C3) serves the API and PrinterDisplay
// (the CYD) queries it, so the port and the token must agree or the CYD talks
// to nothing and never says why.
//
// Copy this file next to it and fill in your own values:
//   copy include\enclosure_api.example.h include\enclosure_api.h
//
// The real file is git-ignored. Both projects refuse to build with a readable
// message until it exists.

// Port the C3 serves the API on. Deliberately NOT OTA_HTTP_PORT (8080): a
// firmware upload and the API must never contend for one socket, and
// WiFiManager's captive portal already owns 80.
#define ENC_API_PORT 8081

// Shared secret. The C3 requires it on every request (HTTP Basic auth, same
// scheme lib/HttpOta already uses); the CYD sends it. Leave empty to disable
// auth, which is only reasonable on a trusted LAN.
//
// This is a shared-secret over plain HTTP, so it stops a casual LAN device from
// driving the fan and louvre - it is not a defence against someone who can
// already sniff the Wi-Fi link.
#define ENC_API_TOKEN "change-me"

#endif // ENCLOSURE_API_H