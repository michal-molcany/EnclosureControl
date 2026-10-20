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
// Generate a long random value rather than typing one. This is a bearer token
// sent in the clear on every request, and under WPA2-PSK every client on your
// access point can decrypt other clients' traffic, so anything on the LAN can
// capture it by passive sniffing - the length is what makes that impractical:
//
//   python -c "import secrets;print(secrets.token_hex(32))"
//
// It guards the enclosure API (8081) AND the browser page (8082), so one leaked
// value covers both. Changing it requires reflashing both boards.
#define ENC_API_TOKEN "replace-with-64-random-hex-characters"

#endif // ENCLOSURE_API_H