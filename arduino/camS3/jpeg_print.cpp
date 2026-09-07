#include "jpeg_print.h"

#include <HTTPClient.h>
#include <TJpg_Decoder.h>
#include <WiFiClient.h>
#include <string.h>

#include "caption.h"
#include "clock.h"
#include "default_adjust.h"
#include "dither.h"
#include "gallery.h"
#include "printer.h"
#include "storage.h"

namespace JpegPrint {

namespace {

// Same idea as web_server.cpp's own kMaxHeightDots for the phone-upload
// path (~250mm) — kept as a separate constant here, matching this
// project's existing per-module pattern.
constexpr uint16_t kMaxHeightDots = 2000;

// Temp file for fetchAndPrint()'s downloaded JPEG — deliberately separate
// from web_server.cpp's own kExternalPhotoTmpPath/own_camera.cpp's temp
// file even though only one of the three is ever in flight at a time (the
// ESP32 WebServer handles one request at a time): keeps this module fully
// self-contained.
constexpr const char *kUrlFetchTmpPath = "/tmp_ext_photo_url.jpg";

// Matches web_server.cpp's kMaxExternalPhotoBytes for the direct-upload
// path — kept in sync by hand, since a fetched photo should face the same
// "please pre-resize/compress" limit an uploaded one does.
constexpr size_t kMaxExternalPhotoBytes = 400 * 1024;

// Generous but bounded: a slow/stalled remote server shouldn't be able to
// tie up this board's single-threaded WebServer loop indefinitely.
constexpr unsigned long kFetchTimeoutMs = 20000;

// Every MCU a JPEG decoder emits is at most 16 pixels tall (8x8 or 16x16
// depending on chroma subsampling) — sizing the band buffer to this many
// rows guarantees one full row of MCUs always fits, so the callback below
// never needs to hold more than one band in RAM at a time.
constexpr uint16_t kBandRows = 16;

// Upper bound on the *decoded* (post-TJpgDec-scale) width, chosen to keep
// the band buffer (kBandRows * this, see gBandBufW below) comfortably
// small. TJpgDec's scale factors are powers of 2 (1/1, 1/2, 1/4, 1/8), so
// this is hit by picking the smallest scale that brings the original
// width under the cap.
constexpr uint16_t kDecodeWidthCap = 800;

// A few extra columns of headroom in case the library's actual decoded
// width comes out slightly larger than our own floor-divided estimate;
// writes past gDecodedW are still clipped against this wider buffer
// instead of overrunning it.
constexpr uint16_t kBandWidthPad = 16;

uint16_t gDecodedW = 0;   // nominal decoded width, used for resize-ratio math
uint16_t gDecodedH = 0;
uint16_t gBandBufW = 0;   // actual allocated buffer width (gDecodedW + kBandWidthPad)
uint16_t gTargetH = 0;    // final printed/gallery height (aspect-preserving from gDecodedW/H)
uint16_t gBandBaseY = 0;  // first source row currently held in gBandBuf
uint8_t *gBandBuf = nullptr;
uint32_t gNextOutputRow = 0;
uint16_t gGalleryId = 0;
Dither::RowDitherer gDitherer;

// Where flushAvailableRows() below sends each decoded/dithered row —
// shared TJpg_Decoder callback state, since the library calls back into
// the one registered callback regardless of which public function (
// printFromFile()/fetchAndPrint() vs. decodeToBuffer()) started the
// decode. kBuffer mode is used by decodeToBuffer() (own_camera.cpp's
// preview-mode capture) to get a dithered bitmap without printing or
// touching the gallery.
enum class Sink { kPrint, kBuffer };
Sink gSink = Sink::kPrint;
uint8_t *gOutBuf = nullptr;      // valid only while gSink == kBuffer
uint16_t gOutBufCapHeight = 0;   // rows gOutBuf can hold; extra decoded rows are silently dropped

int8_t gBrightness = 0;
int8_t gContrast = 0;

// Same formula as data/index.html's toGrayscale(). Applied in place, per
// output row, before dithering.
void applyBrightnessContrast(uint8_t *row, uint16_t width) {
    if (gBrightness == 0 && gContrast == 0) return;
    float factor = (259.0f * (gContrast + 255)) / (255.0f * (259 - gContrast));
    for (uint16_t x = 0; x < width; x++) {
        float g = factor * ((float)row[x] - 128.0f) + 128.0f + gBrightness;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        row[x] = (uint8_t)g;
    }
}

// Fast, direct RGB565 -> grayscale (no full RGB888 unpack via floats —
// this runs once per decoded pixel, so keep it cheap). Weights approximate
// standard luminance (0.299/0.587/0.114) applied to each 5/6/5-bit
// channel scaled up to 8 bits.
uint8_t rgb565ToGray(uint16_t px) {
    uint16_t r5 = (px >> 11) & 0x1F;
    uint16_t g6 = (px >> 5) & 0x3F;
    uint16_t b5 = px & 0x1F;
    uint16_t r8 = (r5 * 527 + 23) >> 6;
    uint16_t g8 = (g6 * 259 + 33) >> 6;
    uint16_t b8 = (b5 * 527 + 23) >> 6;
    return (uint8_t)((r8 * 299 + g8 * 587 + b8 * 114) / 1000);
}

// Emits every output row whose corresponding source row falls within
// [gBandBaseY, gBandBaseY + kBandRows) — i.e. everything the currently
// buffered band can satisfy — advancing gNextOutputRow past each one.
// Nearest-neighbor in both directions: cheap, and photo quality here is
// bounded by 1bpp dithering anyway, so a fancier resample wouldn't be
// worth the extra RAM/CPU.
void flushAvailableRows() {
    while (gNextOutputRow < gTargetH) {
        uint32_t sy = ((uint32_t)gNextOutputRow * gDecodedH) / gTargetH;
        if (sy >= (uint32_t)gBandBaseY + kBandRows) break;  // needs a band we haven't decoded yet
        const uint8_t *srcRow = gBandBuf + (size_t)(sy - gBandBaseY) * gBandBufW;

        uint8_t grayRow[Printer::kPrintWidthDots];
        for (uint16_t ox = 0; ox < Printer::kPrintWidthDots; ox++) {
            uint32_t sx = ((uint32_t)ox * gDecodedW) / Printer::kPrintWidthDots;
            if (sx >= gDecodedW) sx = gDecodedW - 1;
            grayRow[ox] = srcRow[sx];
        }

        applyBrightnessContrast(grayRow, Printer::kPrintWidthDots);
        uint8_t packed[Printer::kPrintWidthBytes];
        gDitherer.processRow(grayRow, packed);
        if (gSink == Sink::kPrint) {
            Printer::feedRasterChunk(packed, sizeof(packed));
            if (gGalleryId != 0 && !Gallery::feedSave(packed, sizeof(packed))) {
                Gallery::cancelSave();
                gGalleryId = 0;
            }
        } else if (gOutBuf && gNextOutputRow < gOutBufCapHeight) {
            memcpy(gOutBuf + (size_t)gNextOutputRow * Printer::kPrintWidthBytes, packed, sizeof(packed));
        }
        gNextOutputRow++;
    }
}

// TJpg_Decoder's per-MCU-block output callback — (x,y) is the block's
// top-left corner in decoded-image coordinates, (w,h) its size (up to
// 16x16, smaller at the right/bottom edges), bitmap its w*h RGB565
// pixels in raster order. Blocks arrive in raster (top-to-bottom,
// left-to-right) MCU order — guaranteed by the JPEG format itself — so a
// block whose y has moved past the currently buffered band means that
// band is complete and can be flushed.
bool jpegOutputCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t *bitmap) {
    if ((uint16_t)y >= gBandBaseY + kBandRows) {
        flushAvailableRows();
        gBandBaseY = (uint16_t)((y / kBandRows) * kBandRows);
        memset(gBandBuf, 0, (size_t)gBandBufW * kBandRows);
    }
    for (uint16_t dy = 0; dy < h; dy++) {
        uint16_t sy = y + dy;
        if (sy >= gDecodedH) break;
        uint16_t bandRow = sy - gBandBaseY;
        if (bandRow >= kBandRows) continue;  // shouldn't happen for a well-formed JPEG, but stay safe
        for (uint16_t dx = 0; dx < w; dx++) {
            uint16_t sx = x + dx;
            if (sx >= gBandBufW) continue;
            gBandBuf[(size_t)bandRow * gBandBufW + sx] = rgb565ToGray(bitmap[(size_t)dy * w + dx]);
        }
    }
    return true;  // keep decoding
}

}  // namespace

bool printFromFile(const String &path, const String &label, const String &location, String &error) {
    uint16_t origW = 0, origH = 0;
    if (TJpgDec.getFsJpgSize(&origW, &origH, path, Storage::fs()) != JDR_OK || origW == 0 || origH == 0) {
        error = "not a valid JPEG";
        return false;
    }

    gBrightness = DefaultAdjust::brightness();
    gContrast = DefaultAdjust::contrast();

    uint8_t scale = 1;
    while ((origW / scale) > kDecodeWidthCap && scale < 8) scale *= 2;
    TJpgDec.setJpgScale(scale);
    TJpgDec.setSwapBytes(false);  // we read bitmap[] as plain uint16_t RGB565, not raw wire bytes
    TJpgDec.setCallback(jpegOutputCallback);

    gDecodedW = origW / scale;
    gDecodedH = origH / scale;
    gBandBufW = gDecodedW + kBandWidthPad;
    gTargetH = (uint16_t)(((uint32_t)origH * Printer::kPrintWidthDots) / origW);
    if (gTargetH == 0) gTargetH = 1;
    if (gTargetH > kMaxHeightDots) {
        error = "image aspect ratio too tall for a " + String(Printer::kPrintWidthDots) + "-dot-wide print";
        return false;
    }

    gBandBuf = (uint8_t *)malloc((size_t)gBandBufW * kBandRows);
    if (!gBandBuf) {
        error = "out of memory decoding this image";
        return false;
    }
    memset(gBandBuf, 0, (size_t)gBandBufW * kBandRows);
    gBandBaseY = 0;
    gNextOutputRow = 0;
    gDitherer.reset();

    bool synced = Clock::isSynced();
    String caption;
    if (label.length() > 0) caption += label;
    if (synced) {
        if (caption.length() > 0) caption += " ";
        caption += Clock::nowDateTime();
    }
    if (location.length() > 0) {
        if (caption.length() > 0) caption += " ";
        caption += location;
    }
    uint16_t bandHeight = (caption.length() > 0 && gTargetH > Caption::kBandHeight) ? Caption::kBandHeight : 0;

    Printer::reset();
    Printer::beginRaster(Printer::kPrintWidthDots, gTargetH + bandHeight);
    gGalleryId = Gallery::beginSave(gTargetH, label);

    JRESULT decodeResult = TJpgDec.drawFsJpg(0, 0, path, Storage::fs());
    if (decodeResult == JDR_OK) flushAvailableRows();  // last partial band, if any

    free(gBandBuf);
    gBandBuf = nullptr;

    if (decodeResult != JDR_OK) {
        Printer::endRaster();  // close out the raster job cleanly even though it's incomplete
        if (gGalleryId != 0) Gallery::cancelSave();
        error = "JPEG decode failed";
        return false;
    }

    if (bandHeight > 0) {
        uint8_t band[Printer::kPrintWidthBytes * Caption::kBandHeight];
        Caption::stamp(band, Printer::kPrintWidthBytes, caption.c_str());
        Printer::feedRasterChunk(band, sizeof(band));
    }
    Printer::endRaster();
    if (gGalleryId != 0) Gallery::endSave();
    return true;
}

bool decodeToBuffer(const String &path, uint8_t *outBuf, uint16_t &outHeight, String &error) {
    Serial.println("[jpeg_print] decodeToBuffer: getFsJpgSize...");
    uint16_t origW = 0, origH = 0;
    if (TJpgDec.getFsJpgSize(&origW, &origH, path, Storage::fs()) != JDR_OK || origW == 0 || origH == 0) {
        error = "not a valid JPEG";
        return false;
    }
    Serial.printf("[jpeg_print] decodeToBuffer: size=%ux%u\n", origW, origH);

    gBrightness = DefaultAdjust::brightness();
    gContrast = DefaultAdjust::contrast();

    uint8_t scale = 1;
    while ((origW / scale) > kDecodeWidthCap && scale < 8) scale *= 2;
    TJpgDec.setJpgScale(scale);
    TJpgDec.setSwapBytes(false);
    TJpgDec.setCallback(jpegOutputCallback);

    gDecodedW = origW / scale;
    gDecodedH = origH / scale;
    gBandBufW = gDecodedW + kBandWidthPad;
    gTargetH = (uint16_t)(((uint32_t)origH * Printer::kPrintWidthDots) / origW);
    if (gTargetH == 0) gTargetH = 1;
    if (gTargetH > kPreviewMaxHeightDots) gTargetH = kPreviewMaxHeightDots;  // preview is capped, unlike a real print

    gBandBuf = (uint8_t *)malloc((size_t)gBandBufW * kBandRows);
    if (!gBandBuf) {
        error = "out of memory decoding this image";
        return false;
    }
    memset(gBandBuf, 0, (size_t)gBandBufW * kBandRows);
    gBandBaseY = 0;
    gNextOutputRow = 0;
    gDitherer.reset();

    gSink = Sink::kBuffer;
    gOutBuf = outBuf;
    gOutBufCapHeight = kPreviewMaxHeightDots;

    Serial.println("[jpeg_print] decodeToBuffer: drawFsJpg...");
    JRESULT decodeResult = TJpgDec.drawFsJpg(0, 0, path, Storage::fs());
    Serial.printf("[jpeg_print] decodeToBuffer: drawFsJpg returned %d\n", (int)decodeResult);
    if (decodeResult == JDR_OK) flushAvailableRows();

    free(gBandBuf);
    gBandBuf = nullptr;
    gSink = Sink::kPrint;  // always restore the default before returning — printFromFile()/fetchAndPrint() assume it
    gOutBuf = nullptr;

    if (decodeResult != JDR_OK) {
        error = "JPEG decode failed";
        return false;
    }
    outHeight = gTargetH;
    return true;
}

bool fetchAndPrint(const String &url, const String &label, const String &location, String &error) {
    if (!url.startsWith("http://")) {
        if (url.startsWith("https://")) {
            error =
                "https:// URLs aren't supported — this board's HTTPS/TLS support is unreliable; "
                "host the photo over plain http:// instead (e.g. a LAN file server)";
        } else {
            error = "url must start with http://";
        }
        return false;
    }

    WiFiClient client;
    HTTPClient http;
    if (!http.begin(client, url)) {
        error = "could not start the request — check the URL";
        return false;
    }
    int code = http.GET();
    if (code != HTTP_CODE_OK) {
        http.end();
        error = "fetch failed: HTTP " + String(code);
        return false;
    }

    int contentLength = http.getSize();  // -1 if the server didn't declare one (chunked transfer)
    if (contentLength > 0 && (size_t)contentLength > kMaxExternalPhotoBytes) {
        http.end();
        error = "remote image too large (max " + String(kMaxExternalPhotoBytes / 1024) + "KB)";
        return false;
    }

    File f = Storage::fs().open(kUrlFetchTmpPath, "w");
    if (!f) {
        http.end();
        error = "could not open temp file (storage full?)";
        return false;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t written = 0;
    uint8_t buf[512];
    unsigned long start = millis();
    bool tooLarge = false;
    while (http.connected() && (contentLength < 0 || written < (size_t)contentLength)) {
        if (millis() - start > kFetchTimeoutMs) break;
        size_t avail = stream->available();
        if (avail == 0) {
            if (!http.connected()) break;
            delay(2);
            continue;
        }
        size_t want = sizeof(buf);
        if (avail < want) want = avail;
        int n = stream->readBytes(buf, want);
        if (n <= 0) break;
        written += n;
        if (written > kMaxExternalPhotoBytes) {
            tooLarge = true;
            break;
        }
        f.write(buf, n);
    }
    f.close();
    http.end();

    if (tooLarge) {
        Storage::fs().remove(kUrlFetchTmpPath);
        error = "remote image too large (max " + String(kMaxExternalPhotoBytes / 1024) + "KB)";
        return false;
    }
    if (written == 0) {
        Storage::fs().remove(kUrlFetchTmpPath);
        error = "downloaded photo was empty";
        return false;
    }

    bool ok = printFromFile(kUrlFetchTmpPath, label, location, error);
    Storage::fs().remove(kUrlFetchTmpPath);
    return ok;
}

}  // namespace JpegPrint
