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
//   | labelLen u8 | label (labelLen bytes, UTF-8, no NUL)
//   | width*height grayscale bytes (row-major, 0-255)
//   | 1 checksum byte (sum of all label bytes + all pixel bytes, mod 256)
// width must equal Printer::kPrintWidthDots (384); height is capped well
// below the phone-upload path's limit because a full frame is now kept in
// RAM (dithered, 1bpp) for on-page viewing regardless of mode. `label` is a
// short human-readable description of what the M5StickV's on-device
// detection found in the shot (currently just "Face"/"Face x2"/... from a
// Haar-cascade face detector, empty if nothing was detected) — shown as a
// caption under the image on the m5web page and in the gallery.
namespace CameraLink {

// kAuto prints every received photo immediately; kPreview holds it for
// review on the m5web page instead. Independent of this: the "俳句設定"
// card's own auto-haiku setting (see haiku.h's AutoMode) decides, entirely
// browser-side, whether a haiku/poem also gets generated (and optionally
// printed) from the same frame — this module has no notion of haiku at
// all beyond bumping frameSeq so the browser's /api/camera/status poll can
// notice a new frame either way.
enum class Mode { kAuto, kPreview };

// Matches maixpy/m5web_capture.py's MAX_LABEL_LEN; longer labels are
// truncated on receipt (the byte count on the wire is still consumed in
// full so the stream doesn't desync).
constexpr uint8_t kMaxLabelLen = 31;

struct Status {
    Mode mode;
    bool frameReady;     // a frame has been received at least once
    bool pendingPrint;   // true only in preview mode, until confirmed/discarded
    uint16_t width;
    uint16_t height;
    uint32_t frameSeq;   // increments each time a new frame is stored
    int8_t brightness;   // default adjustment applied to future frames, -100..100
    int8_t contrast;     // default adjustment applied to future frames, -100..100
    uint16_t rotationDeg;  // default rotation applied to future frames, one of 0/90/180/270
    char label[kMaxLabelLen + 1];  // "" if nothing was detected
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

// Default rotation (0/90/180/270, wraps) applied to every frame *received*
// from this point on — like setAdjust(), never retroactive to an
// already-buffered/pending frame. Each call adds another 90° clockwise on
// top of the current default, persisted across reboots. Distinct from
// rotate() below, which rotates the currently-held frame immediately
// (used for one-off adjustment of a photo right before printing it).
void rotateDefaultBy90();

Status status();

// Prints the last stored frame — the currently-pending one in preview
// mode (also clearing pendingPrint), or a plain reprint of whatever was
// last received/printed otherwise. Returns false if no frame has arrived
// yet (frameReady is false). Callable any time, from the web UI or the
// ATOM Lite's physical button (see main.cpp).
bool printLastFrame();

// Preview mode only: drop the currently pending frame without printing it.
void discardPending();

// Rotates the last stored frame 90° clockwise, in place. A received frame
// is always exactly kPrintWidthDots wide (see the wire protocol above), so
// a plain rotate would leave it kPrintWidthDots *tall* instead — not
// printable (Printer::beginRaster requires exactly kPrintWidthDots wide).
// Rescaled back to kPrintWidthDots wide (aspect-preserving, nearest-
// neighbor) so the result stays printable, same as every other stored
// frame; repeatable — each call adds another 90° on top of the last.
// Returns false (no-op) if no frame has arrived yet. Bumps frameSeq so the
// web UI's next poll picks up the new image/dimensions.
bool rotate();

// Raw packed 1bpp bitmap (MSB-first, 1=black) of the last stored frame, for
// serving to the web UI. nullptr/0 if no frame has arrived yet.
const uint8_t *frameData();
size_t frameDataLen();

}  // namespace CameraLink
