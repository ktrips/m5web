#pragma once

#include <Arduino.h>

// CamS3 can print in one of two ways, switchable from the web UI (a
// reboot applies the change — see setMode()):
//
//   kDirect  — a thermal printer is wired straight to CamS3 (see
//              printer.h's configurable TX/RX pins). CamS3 runs the full
//              menu itself (gallery, haiku/QR/text printing, storage
//              choice, printer pin settings, ...), same as m5web.
//   kViaAtom — CamS3 has no printer of its own; it's just a camera. A
//              captured photo is POSTed to an ATOM Lite running m5web
//              (see atomHost()) at /api/print/photo, and m5web does
//              everything else (printing, gallery, settings). This is
//              the original camS3.ino behavior and the default, so a
//              fresh CamS3 keeps working as a wireless shutter for an
//              existing m5web setup without any extra configuration.
namespace PrinterMode {

enum class Mode { kDirect, kViaAtom };

void begin();

Mode mode();

// Persists the new mode and reboots — the two modes initialize a
// different enough set of modules (Printer/Gallery/Storage vs. nothing)
// that re-initializing in place isn't worth the complexity a boot-time
// branch already handles for free.
void setMode(Mode m);

// Hostname (or IP) of the m5web device to forward captures to in
// kViaAtom mode. Defaults to "m5web.local".
String atomHost();
void setAtomHost(const String &host);

}  // namespace PrinterMode
