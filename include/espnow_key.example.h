#ifndef ESPNOW_KEY_H
#define ESPNOW_KEY_H

// Link key for the ESP-NOW link between the two boards. SHARED BY BOTH
// FIRMWARES - PrinterDisplay (the CYD) and EnclosureFanControl (the C3) must
// agree on it byte for byte.
//
// Copy this file next to it and fill in your own value:
//   copy include\espnow_key.example.h include\espnow_key.h
//
// The real file is git-ignored. Both projects will refuse to build with a
// readable message until it exists.
//
// WHY THIS EXISTS
// ---------------
// ESP-NOW frames never leave the radio, so the access point's WPA2 does nothing
// for them: with peer.encrypt = false, every telemetry frame was plaintext on
// the air. That matters more than it used to, because telemetry now carries the
// OctoPrint API key - so an unencrypted link handed a live printer credential to
// anything within radio range. Setting peer.encrypt = true with the key below
// makes those frames confidential and authenticated.
//
// 16 bytes is not a free parameter: ESP-NOW's LTK is AES-128-CCM, and esp_now
// rejects any other length. Generate it once, keep it, and never edit it in only
// one board:
//
//   python -c "import secrets;print(secrets.token_bytes(16).hex())"
//
// The result is 32 hex characters, which is what the array below expects.
//
// IF THE TWO BOARDS DISAGREE
// ---------------------------
// There is no error and no negotiation. Each encrypts to the key it holds and
// the other cannot read it, so the link is simply dead - indistinguishable from
// a radio fault, a channel split, or a missing peer. Both boards print whether
// encryption is enabled at boot so you can rule this out first, before the
// channel/AP checklist in ../ESPNOW_PROTOCOL.md. Changing this value requires
// reflashing BOTH boards.
//
// WHAT THIS DOES NOT DO
// ---------------------
// Encryption is configured on the SEND side. There is no receiver-side option to
// reject an unencrypted frame, so anyone who forges the other board's MAC (it is
// broadcast in every frame) can still inject plaintext commands. This key
// protects what the boards transmit; it does not authenticate what they receive.

#define ENC_ESP_NOW_LTK                                                            \
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,                               \
     0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}

#endif // ESPNOW_KEY_H