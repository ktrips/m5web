#pragma once

#include <Arduino.h>

// Drives the ATOM Lite's onboard RGB LED (single SK6812, G27) as a status
// indicator — separate from the M5StickV's own LED (see maixpy/), which
// signals capture/send progress on the camera side instead.
//
// Two independent signals share the one LED:
//   - notifyNewImage(): a brief 3x blink when a new image lands in the
//     gallery (M5StickV capture or phone upload), whether or not it ends
//     up auto-printed.
//   - solid color whenever there's an image that can be printed: either the
//     gallery holds at least one saved photo (always reprintable via the
//     web UI), or a CameraLink frame is awaiting a print/discard decision
//     in preview mode. Off only when neither is true. A running blink
//     temporarily overrides the solid state, then restores it.
//
// Both signals share a single mode color — green for 俳句 mode, blue for
// ポエム mode (see Haiku::poemType(), and the double-click handling in
// main.cpp that toggles it) — set via setModeColor() and otherwise left
// alone, so glancing at the LED tells you both "is something printable"
// (lit or not) and "which poem mode is active" (which color) at once.
namespace Led {

void begin();
void poll();  // call every iteration of the main loop; drives blink timing

void notifyNewImage();
void setCameraPending(bool pending);     // CameraLink preview-mode confirmation awaited
void setGalleryNonEmpty(bool nonEmpty);  // at least one photo saved in the gallery
void setModeColor(bool haikuMode);       // true = green (俳句), false = blue (ポエム)

}  // namespace Led
