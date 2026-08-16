#pragma once

#include <Arduino.h>

// Drives the ATOM Lite's onboard RGB LED (single SK6812, G27) as a status
// indicator — separate from the M5StickV's own LED (see maixpy/), which
// signals capture/send progress on the camera side instead.
//
// Two independent signals share the one LED:
//   - notifyNewImage(): a brief 3x green blink when a new image lands in
//     the gallery (M5StickV capture or phone upload), whether or not it
//     ends up auto-printed.
//   - solid green whenever there's an image that can be printed: either the
//     gallery holds at least one saved photo (always reprintable via the
//     web UI), or a CameraLink frame is awaiting a print/discard decision
//     in preview mode. Off only when neither is true. A running blink
//     temporarily overrides the solid state, then restores it.
namespace Led {

void begin();
void poll();  // call every iteration of the main loop; drives blink timing

void notifyNewImage();
void setCameraPending(bool pending);     // CameraLink preview-mode confirmation awaited
void setGalleryNonEmpty(bool nonEmpty);  // at least one photo saved in the gallery

}  // namespace Led
