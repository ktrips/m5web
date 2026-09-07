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
//     expected and silently ignored, not an error worth surfacing.
// Both are blocking HTTP calls (this board's WebServer loop pauses for
// the duration, same tradeoff every other network call in this codebase
// already makes — see e.g. openai.cpp) with a generous timeout, since a
// capture+print/forward round trip on CamS3's side can take a few
// seconds.
namespace CamS3Remote {

void begin();

String host();
void setHost(const String &host);  // persisted; "" resets to the default "m5cam.local"

// Returns false (with nothing else to show for it — this is a fire-and-
// forget remote trigger) if CamS3 can't be reached or reports an error.
bool triggerShutter();
bool reprintLatest();

}  // namespace CamS3Remote
