#include "own_camera.h"

#include <Preferences.h>
#include <string.h>

#include "caption.h"
#include "camS3_actions.h"
#include "clock.h"
#include "esp_camera.h"
#include "gallery.h"
#include "jpeg_print.h"
#include "printer.h"
#include "printer_mode.h"
#include "storage.h"

namespace OwnCamera {

namespace {
constexpr const char *kCaptureTmpPath = "/tmp_own_capture.jpg";

Preferences prefs;
Mode currentMode = Mode::kPreview;

// Holds the current preview frame's dithered bitmap once captured in
// kPreview mode — same packed-1bpp format Printer/Gallery consume, sized
// like m5web's own CameraLink::frameBuffer (48 bytes/row * 800 rows =
// ~38KB). Reused across captures; only meaningful while frameReady is
// true. The original JPEG (kCaptureTmpPath) is kept alongside it while
// pendingPrint is true, in case confirmPrint() needs to forward it to
// ATOM rather than print it locally.
uint8_t previewBuf[(size_t)Printer::kPrintWidthBytes * JpegPrint::kPreviewMaxHeightDots];
uint16_t previewHeight = 0;
bool frameReady = false;
bool pendingPrint = false;
uint32_t frameSeq = 0;

const char *modeName(Mode m) { return m == Mode::kPreview ? "preview" : "auto"; }
Mode modeFromName(const String &s) { return s == "auto" ? Mode::kAuto : Mode::kPreview; }

// Streams the held preview buffer straight to the printer + gallery, with
// a timestamp caption — same shape as m5web's CameraLink::printStoredFrame()
// + Gallery::save(), just reusing the bitmap already decoded into
// previewBuf instead of re-decoding a JPEG. Direct-connect mode only.
bool printPreviewBuf() {
    String caption = Caption::combine("", Clock::nowDateTime().c_str());
    uint16_t bandHeight = (caption.length() > 0 && previewHeight > Caption::kBandHeight) ? Caption::kBandHeight : 0;

    Printer::reset();
    Printer::beginRaster(Printer::kPrintWidthDots, previewHeight + bandHeight);
    Printer::feedRasterChunk(previewBuf, (size_t)Printer::kPrintWidthBytes * previewHeight);
    if (bandHeight > 0) {
        uint8_t band[Printer::kPrintWidthBytes * Caption::kBandHeight];
        Caption::stamp(band, Printer::kPrintWidthBytes, caption.c_str());
        Printer::feedRasterChunk(band, sizeof(band));
    }
    Printer::endRaster();

    if (Gallery::save(previewBuf, (size_t)Printer::kPrintWidthBytes * previewHeight, previewHeight) == 0) {
        Serial.println("[own_camera] gallery full or save failed — printed anyway");
    }
    return true;
}

// Captures one JPEG frame into kCaptureTmpPath. Returns false with
// `error` set on failure; the caller owns deleting the file afterward.
bool captureToTmpFile(String &error) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        error = "capture failed (esp_camera_fb_get returned null) — check camera init log";
        return false;
    }

    File f = Storage::fs().open(kCaptureTmpPath, "w");
    if (!f) {
        esp_camera_fb_return(fb);
        error = "could not open temp file (storage full?)";
        return false;
    }
    size_t written = f.write(fb->buf, fb->len);
    f.close();
    size_t fbLen = fb->len;
    esp_camera_fb_return(fb);

    if (written != fbLen) {
        Storage::fs().remove(kCaptureTmpPath);
        error = "failed writing captured frame to storage (out of space?)";
        return false;
    }
    return true;
}

}  // namespace

void begin() {
    prefs.begin("m5cam_owncam", false);
    currentMode = modeFromName(prefs.getString("mode", "preview"));
}

Mode mode() { return currentMode; }

void setMode(Mode m) {
    currentMode = m;
    prefs.putString("mode", modeName(m));
    Serial.printf("[own_camera] mode set to %s\n", modeName(m));
}

bool captureAndCommitNow(String &error) {
    if (!captureToTmpFile(error)) return false;

    bool ok;
    if (PrinterMode::mode() == PrinterMode::Mode::kDirect) {
        ok = JpegPrint::printFromFile(kCaptureTmpPath, "", "", error);
    } else {
        ok = sendJpegFileToAtom(kCaptureTmpPath, error);  // defined in camS3.ino — see camS3_actions.h
    }
    Storage::fs().remove(kCaptureTmpPath);
    return ok;
}

bool captureForPreview(String &error) {
    if (!captureToTmpFile(error)) return false;

    uint16_t decodedHeight = 0;
    if (!JpegPrint::decodeToBuffer(kCaptureTmpPath, previewBuf, decodedHeight, error)) {
        Storage::fs().remove(kCaptureTmpPath);
        return false;
    }

    // kCaptureTmpPath deliberately NOT removed here — confirmPrint() needs
    // it if PrinterMode is kViaAtom (see below); discardPending()/the next
    // capture clean it up instead.
    previewHeight = decodedHeight;
    frameReady = true;
    pendingPrint = true;
    frameSeq++;
    Serial.printf("[own_camera] preview frame ready: %ux%u\n", Printer::kPrintWidthDots, previewHeight);
    return true;
}

Status status() {
    return Status{currentMode, frameReady, pendingPrint, Printer::kPrintWidthDots, previewHeight, frameSeq};
}

bool confirmPrint(String &error) {
    if (!pendingPrint) return false;
    bool ok;
    if (PrinterMode::mode() == PrinterMode::Mode::kDirect) {
        ok = printPreviewBuf();
    } else {
        ok = sendJpegFileToAtom(kCaptureTmpPath, error);  // defined in camS3.ino — see camS3_actions.h
    }
    Storage::fs().remove(kCaptureTmpPath);
    pendingPrint = false;
    return ok;
}

void discardPending() {
    Storage::fs().remove(kCaptureTmpPath);
    pendingPrint = false;
}

const uint8_t *frameData() { return frameReady ? previewBuf : nullptr; }

size_t frameDataLen() { return frameReady ? (size_t)Printer::kPrintWidthBytes * previewHeight : 0; }

}  // namespace OwnCamera
