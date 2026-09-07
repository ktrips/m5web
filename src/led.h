#pragma once

#include <Arduino.h>

// Drives the ATOM Lite's onboard RGB LED (single SK6812, G27) as a status
// indicator — separate from the M5StickV's own LED (see maixpy/), which
// signals capture/send progress on the camera side instead.
//
// Two independent signals share the one LED:
//   - notifyNewImage(): a brief 3x blink, in the current mode color (green
//     for 俳句, blue for ポエム — see Haiku::poemType(), set from the
//     m5webページ's 「俳句設定」card via /api/haiku/settings, and restored
//     from NVS at boot) via setModeColor(), when a new image lands in the
//     gallery (M5StickV capture or phone upload), whether or not it ends
//     up auto-printed.
//   - solid WHITE whenever there's an image that can be printed: either the
//     gallery holds at least one saved photo (always reprintable via the
//     web UI), or a CameraLink frame is awaiting a print/discard decision
//     in preview mode. Off only when neither is true. A running blink
//     temporarily overrides this solid state, then restores it. Kept a
//     plain white (not the mode color) so "there's something to print" is
//     an unambiguous signal on its own — see ATOM Lite本体のボタン・RGB LED
//     in README.md.
namespace Led {

void begin();
void poll();  // call every iteration of the main loop; drives blink timing

void notifyNewImage();
void setCameraPending(bool pending);     // CameraLink preview-mode confirmation awaited
void setGalleryNonEmpty(bool nonEmpty);  // at least one photo saved in the gallery
void setModeColor(bool haikuMode);       // true = green (俳句), false = blue (ポエム)

}  // namespace Led
