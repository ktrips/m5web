#pragma once

#include <Arduino.h>

// Drives two plain GPIO outputs wired to an optional companion ATOM Matrix
// (see arduino/atomMatrix/atomMatrix.ino's checkLiteSignals()), so its 5x5
// LED matrix can mirror the ATOM Lite's own CamS3-shutter button as a
// flash + result indicator — useful when the ATOM Lite itself is tucked
// away and the Matrix sits somewhere more visible. Entirely optional: with
// nothing wired to these pins, driving them does nothing observable.
//
// Two independent signals, matching main.cpp's triggerCamS3Shutter():
//   - setFlash(true) right before asking CamS3 to take a photo (over WiFi
//     or the wired GPIO25 link — see cams3_remote.h), setFlash(false)
//     right after — ATOM Matrix shows solid white for exactly this window.
//   - pulseSuccess() once, only if that trigger succeeded — ATOM Matrix
//     blinks green a few times. Same caveat as everywhere else this
//     project signals success from a fire-and-forget action: over the
//     wired CamS3 link this only means "the pulse was sent", not that
//     CamS3 actually captured/printed anything.
//
// Wiring (2 wires + shared ground, same pin numbers used on the ATOM
// Matrix end purely for wiring clarity — see README.md's「ATOM Lite⇔ATOM
// Matrix フラッシュ表示」section):
//   ATOM Lite G21 (kFlashPin)   -> ATOM Matrix G21
//   ATOM Lite G22 (kSuccessPin) -> ATOM Matrix G22
//   ATOM Lite GND               -> ATOM Matrix GND
namespace MatrixSignal {

void begin();
void setFlash(bool on);
void pulseSuccess();

}  // namespace MatrixSignal
