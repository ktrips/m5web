#pragma once

#include <Arduino.h>

// CamS3's own shutter, shared by both PrinterMode modes (see
// printer_mode.h) — the 撮影方式 setting here (auto/preview) is
// independent of and applies on top of whichever PrinterMode is active:
//   kAuto:    capture and immediately commit — prints locally
//             (PrinterMode::kDirect) or forwards to ATOM
//             (PrinterMode::kViaAtom), matching this project's original
//             per-mode behavior.
//   kPreview: capture and hold for confirmation on CamS3's own 撮影 card
//             first; confirmPrint() then does the direct-mode print or
//             the ATOM-mode forward, whichever PrinterMode is active at
//             confirm time.
namespace OwnCamera {

enum class Mode { kAuto, kPreview };

struct Status {
    Mode mode;
    bool frameReady;    // a frame has been captured at least once
    bool pendingPrint;  // true only in preview mode, until confirmed/discarded
    uint16_t width;
    uint16_t height;
    uint32_t frameSeq;  // increments each time a new preview frame is stored
};

void begin();  // restores the persisted mode from NVS

Mode mode();
void setMode(Mode m);  // persisted across reboots

// kAuto path: captures and immediately commits (prints locally or
// forwards to ATOM depending on PrinterMode::mode()). `error` explains a
// failure at any step.
bool captureAndCommitNow(String &error);

// kPreview path: captures and decodes into an in-RAM preview buffer,
// holding both it and the original JPEG file for confirmPrint()/
// discardPending(). Does NOT print or forward yet.
bool captureForPreview(String &error);

Status status();

// Preview mode only: commits the held frame — prints locally
// (PrinterMode::kDirect) or forwards the original JPEG to ATOM
// (PrinterMode::kViaAtom), same as captureAndCommitNow() would have done
// right after capture. Returns false (no-op) if there's no pending frame.
bool confirmPrint(String &error);

// Preview mode only: drops the held frame without printing/forwarding it.
void discardPending();

// Raw packed 1bpp bitmap (MSB-first, 1=black) of the held preview frame,
// for serving to the web UI (/api/camera/frame). nullptr/0 if none.
const uint8_t *frameData();
size_t frameDataLen();

}  // namespace OwnCamera
