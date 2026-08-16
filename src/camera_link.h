#pragma once

#include <Arduino.h>

// Receives a grayscale camera frame from an M5StickV over a second, direct
// UART link (no WiFi/HTTP involved) and either prints it immediately or
// holds it for review on the m5web page, depending on the configured mode.
// Pairs with maixpy/m5web_capture.py running on the M5StickV.
//
// Wiring (Grove 4-pin, GND + two signal wires — leave the VCC pins
// unconnected, each board has its own power supply):
//   M5StickV G34 (UART1 TX) -> ATOM Lite G32 (RX)
//   M5StickV G35 (UART1 RX) <- ATOM Lite G26 (TX, currently unused by the
//                               protocol but wired for symmetry/future use)
//   GND                     -- GND
//
// Wire protocol, sent once per frame, no acknowledgement:
//   "M5PV" (4 bytes) | width u16 LE | height u16 LE
//   | width*height grayscale bytes (row-major, 0-255)
//   | 1 checksum byte (sum of all pixel bytes mod 256)
// width must equal Printer::kPrintWidthDots (384); height is capped well
// below the phone-upload path's limit because a full frame is now kept in
// RAM (dithered, 1bpp) for on-page viewing regardless of mode.
namespace CameraLink {

enum class Mode { kAuto, kPreview };

struct Status {
    Mode mode;
    bool frameReady;     // a frame has been received at least once
    bool pendingPrint;   // true only in preview mode, until confirmed/discarded
    uint16_t width;
    uint16_t height;
    uint32_t frameSeq;   // increments each time a new frame is stored
    int8_t brightness;   // default adjustment applied to future frames, -100..100
    int8_t contrast;     // default adjustment applied to future frames, -100..100
};

void begin();
void poll();  // call every iteration of the main loop

Mode mode();
void setMode(Mode m);  // persisted across reboots

// Brightness/contrast (clamped to -100..100, matching the phone-upload
// UI's sliders) applied to every row of a frame before dithering.
// Persisted across reboots. Only affects frames received *after* the
// change — an already-buffered/pending frame is not retroactively
// reprocessed. Takes `int` (not int8_t) so out-of-range input from an API
// caller gets clamped rather than silently wrapping around.
void setAdjust(int brightness, int contrast);

Status status();

// Prints the last stored frame — the currently-pending one in preview
// mode (also clearing pendingPrint), or a plain reprint of whatever was
// last received/printed otherwise. Returns false if no frame has arrived
// yet (frameReady is false). Callable any time, from the web UI or the
// ATOM Lite's physical button (see main.cpp).
bool printLastFrame();

// Preview mode only: drop the currently pending frame without printing it.
void discardPending();

// Raw packed 1bpp bitmap (MSB-first, 1=black) of the last stored frame, for
// serving to the web UI. nullptr/0 if no frame has arrived yet.
const uint8_t *frameData();
size_t frameDataLen();

}  // namespace CameraLink
