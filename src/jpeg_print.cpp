#include "jpeg_print.h"

#include <LittleFS.h>
#include <TJpg_Decoder.h>
#include <string.h>

#include "caption.h"
#include "clock.h"
#include "dither.h"
#include "gallery.h"
#include "led.h"
#include "printer.h"

namespace JpegPrint {

namespace {

// Same idea as web_server.cpp's own kMaxHeightDots for the phone-upload
// path (~250mm) — kept as a separate constant here (like CameraLink's own
// independent kMaxHeightDots=800) rather than shared, matching this
// project's existing per-module pattern.
constexpr uint16_t kMaxHeightDots = 2000;

// Every MCU a JPEG decoder emits is at most 16 pixels tall (8x8 or 16x16
// depending on chroma subsampling) — sizing the band buffer to this many
// rows guarantees one full row of MCUs always fits, so the callback below
// never needs to hold more than one band in RAM at a time.
constexpr uint16_t kBandRows = 16;

// Upper bound on the *decoded* (post-TJpgDec-scale) width, chosen to keep
// the band buffer (kBandRows * this, see gBandBufW below) comfortably
// small — 800*16 = 12800 bytes plus a small safety pad. TJpgDec's scale
// factors are powers of 2 (1/1, 1/2, 1/4, 1/8), so this is hit by picking
// the smallest scale that brings the original width under the cap.
constexpr uint16_t kDecodeWidthCap = 800;

// A few extra columns of headroom in case the library's actual decoded
// width (origW/scale, rounded however TJpg_Decoder's IDCT scaling
// actually rounds — not independently verified here) comes out slightly
// larger than our own floor-divided estimate; writes past gDecodedW are
// still clipped against this wider buffer instead of overrunning it.
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
// Nearest-neighbor in both directions (matches the simple resize already
// used elsewhere in this codebase, e.g. camera_link.cpp's rotate
// rescale): cheap, and photo quality here is bounded by 1bpp dithering
// anyway, so a fancier resample wouldn't be worth the extra RAM/CPU.
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

        uint8_t packed[Printer::kPrintWidthBytes];
        gDitherer.processRow(grayRow, packed);
        Printer::feedRasterChunk(packed, sizeof(packed));
        if (gGalleryId != 0 && !Gallery::feedSave(packed, sizeof(packed))) {
            Gallery::cancelSave();
            gGalleryId = 0;
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
    if (TJpgDec.getFsJpgSize(&origW, &origH, path, LittleFS) != JDR_OK || origW == 0 || origH == 0) {
        error = "not a valid JPEG";
        return false;
    }

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

    JRESULT decodeResult = TJpgDec.drawFsJpg(0, 0, path, LittleFS);
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
    if (gGalleryId != 0) {
        Gallery::endSave();
        Led::notifyNewImage();
    }
    return true;
}

}  // namespace JpegPrint
