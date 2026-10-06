#ifndef OCTOPRINT_ENDPOINT_H
#define OCTOPRINT_ENDPOINT_H

// Address of the OctoPrint server. SHARED BY BOTH FIRMwares - PrinterDisplay
// (the CYD) and EnclosureFanControl (the C3) talk to the same server, so this
// value lives here once and both projects include it. Editing one copy used to
// leave the other pointing at the old server with nothing to warn about it.
//
// Copy this file next to it and fill in your own values:
//   copy include\octoprint_endpoint.example.h include\octoprint_endpoint.h
//
// The real file is git-ignored. Both projects will refuse to build with a
// readable message until it exists.
#define OCTOPRINT_IP "192.168.1.xxx" // OctoPrint server IP address
#define OCTOPRINT_PORT 80            // OctoPrint server port

#endif // OCTOPRINT_ENDPOINT_H