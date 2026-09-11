#pragma once

#include <Arduino.h>

// Lets the ATOM Lite's own physical button remote-control a CamS3 running
// in either of its two modes (see arduino/camS3/printer_mode.h) — see
// README.md's「ATOM Lite本体のボタン・RGB LED」section:
//   single-click -> triggerShutter(): CamS3 takes a photo, exactly as if
//     its own web UI's 撮影ボタン had been pressed. Works the same way
//     regardless of CamS3's own mode (direct-connect or via-ATOM) — CamS3
//     decides what "take a photo" means on its own side.
//   double-click -> reprintLatest(), alongside the existing
//     CameraLink::printLastFrame() call: asks CamS3 to reprint its own
//     newest gallery entry. Only meaningful when CamS3 is in direct-
//     connect mode (it has no gallery otherwise) — a 404 there is
//     expected and silently ignored, not an error worth surfacing. Always
//     goes over WiFi (see ConnMode below) regardless of the configured
//     connection mode — a single wired pin can carry one signal
//     ("shutter"), not a second distinct "reprint" one.
//
// triggerShutter() goes one of two ways, selected by ConnMode (persisted,
// see setConnMode()):
//   kWifi  (default) — HTTP GET http://<host()>/shutter, same as always.
//     Needs both boards on the same WiFi network and CamS3 reachable by
//     mDNS/IP; see README.md's troubleshooting notes for what to check if
//     this silently does nothing.
//   kWired — pulses kWiredTriggerPin LOW for kWiredPulseMs, wired straight
//     to CamS3's GPIO0 (same idea as arduino/atomMatrix/atomMatrix.ino,
//     just from the ATOM Lite itself instead of a third board — see
//     README.md's「ATOM Lite⇔CamS3 直結シャッター」section for wiring and
//     the GPIO0 boot-strapping caveat that applies here too). No WiFi
//     involved at all, and no wire back from CamS3 either — a wired
//     triggerShutter() returning true only means the pulse was sent, not
//     that CamS3 actually captured/printed anything (check CamS3's own
//     LED/serial log for that, exactly as with atomMatrix.ino).
//
// Both HTTP calls are blocking (this board's WebServer loop pauses for
// the duration, same tradeoff every other network call in this codebase
// already makes — see e.g. openai.cpp) with a generous timeout, since a
// capture+print/forward round trip on CamS3's side can take a few
// seconds. The wired pulse blocks for only kWiredPulseMs (~250ms) — much
// shorter, one of the reasons kWired exists as an option.
namespace CamS3Remote {

enum class ConnMode { kWifi, kWired };

void begin();

String host();
void setHost(const String &host);  // persisted; "" resets to the default "m5cam.local"

ConnMode connMode();
void setConnMode(ConnMode m);  // persisted

// Returns false (with nothing else to show for it — this is a fire-and-
// forget remote trigger) if CamS3 can't be reached or reports an error
// (kWifi), or is always true for kWired (see ConnMode above — there's no
// way to know if it actually landed).
bool triggerShutter();
bool reprintLatest();

}  // namespace CamS3Remote
