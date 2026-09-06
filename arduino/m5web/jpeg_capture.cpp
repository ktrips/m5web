#include "jpeg_capture.h"

#include <JPEGENC.h>

#include "camera_link.h"
#include "printer.h"

namespace JpegCapture {

namespace {

// Caps the downscaled output size, same idea as jpeg_print.cpp's
// kDecodeWidthCap on the decode side. The encode itself no longer needs
// a full dstW*dstH buffer (see the per-MCU-band loop below), but this
// still bounds the JPEG's own dimensions and encode time.
constexpr uint16_t kMaxOutputHeight = 300;

// Cap for the encoded JPEG itself. With the box-averaging downscale
// below (always >=3x reduction per side) the worst case is under
// ~34KB (dstW*dstH <= 128*264 pixels, and JPEG rarely exceeds ~1
// byte/pixel even on incompressible input) — 64KB leaves comfortable
// margin. This board's heap couldn't reliably satisfy a 128KB single
// allocation (fragmented by WiFi/webserver usage), so stay modest
// here rather than padding further.
constexpr size_t kJpegBufCap = 64 * 1024;

uint8_t *gJpegBuf = nullptr;  // allocated once on first use, reused across calls

// Averages the packed 1bpp CameraLink frame (same bit layout
// camera_link.cpp's own getBit() reads: MSB-first, 1=black) over the
// source box [sx0,sx1) x [sy0,sy1) into one grayscale value. A single
// nearest-neighbor sample here would just pick one dither dot's color
// and hand JPEG a bilevel noise pattern that its DCT can't compress —
// averaging over the box reconstructs the smooth tone the dithering
// was standing in for, which is what actually makes this compress well.
uint8_t areaAverage(const uint8_t *frame, uint16_t bytesPerRow, uint16_t sx0, uint16_t sx1, uint16_t sy0,
                     uint16_t sy1) {
    uint32_t sum = 0;
    uint32_t count = 0;
    for (uint16_t y = sy0; y < sy1; y++) {
        const uint8_t *row = frame + (size_t)y * bytesPerRow;
        for (uint16_t x = sx0; x < sx1; x++) {
            bool black = (row[x >> 3] >> (7 - (x & 7))) & 1;
            sum += black ? 0 : 255;
            count++;
        }
    }
    return count ? (uint8_t)(sum / count) : 255;
}

}  // namespace

bool encodeCurrentFrame(const uint8_t *&outBuf, size_t &outLen, String &error) {
    const uint8_t *frame = CameraLink::frameData();
    size_t frameLen = CameraLink::frameDataLen();
    if (!frame || frameLen == 0) {
        error = "no frame yet";
        return false;
    }

    CameraLink::Status s = CameraLink::status();
    uint16_t srcW = s.width;  // always Printer::kPrintWidthDots
    uint16_t srcH = s.height;
    uint16_t bytesPerRow = Printer::kPrintWidthBytes;

    uint16_t scale = 1;
    while ((srcH / scale) > kMaxOutputHeight) scale++;
    // Force at least a 3x reduction in both dimensions even for an
    // already-short frame, so areaAverage() below always has a real
    // box (>=3x3 source pixels) to smooth per output pixel — without
    // this, a short frame would pass through at native resolution with
    // its dither pattern intact, which is what blew the JPEG output
    // buffer even at reduced quality.
    if (scale < 3) scale = 3;
    uint16_t dstW = srcW / scale;
    uint16_t dstH = srcH / scale;
    // JPEG's DCT works in 8x8 blocks — round down to a multiple of 8 so
    // the encoder never has to pad an odd edge itself.
    dstW -= dstW % 8;
    dstH -= dstH % 8;
    if (dstW == 0) dstW = 8;
    if (dstH == 0) dstH = 8;

    // Holds one 8-row MCU band across the full downscaled width — the
    // most this encode needs allocated at once. addFrame() would need
    // the whole dstW*dstH image resident instead, which on a tall
    // capture can run to several hundred KB and doesn't survive this
    // board's fragmented heap.
    uint8_t *band = (uint8_t *)malloc((size_t)dstW * 8);
    if (!band) {
        error = "out of memory (row buffer)";
        return false;
    }

    if (!gJpegBuf) gJpegBuf = (uint8_t *)malloc(kJpegBufCap);
    if (!gJpegBuf) {
        free(band);
        error = "out of memory (JPEG output buffer)";
        return false;
    }

    JPEGENC jpg;
    JPEGENCODE enc;
    int rc = jpg.open(gJpegBuf, kJpegBufCap);
    if (rc == JPEGE_SUCCESS) {
        rc = jpg.encodeBegin(&enc, dstW, dstH, JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_444, JPEGE_Q_MED);
    }
    for (uint16_t ry = 0; rc == JPEGE_SUCCESS && ry < dstH; ry += 8) {
        for (uint8_t row = 0; row < 8; row++) {
            uint16_t oy = ry + row;
            uint16_t sy0 = (uint16_t)(((uint32_t)oy * srcH) / dstH);
            uint16_t sy1 = (uint16_t)(((uint32_t)(oy + 1) * srcH) / dstH);
            if (sy1 <= sy0) sy1 = sy0 + 1;
            if (sy1 > srcH) sy1 = srcH;
            for (uint16_t ox = 0; ox < dstW; ox++) {
                uint16_t sx0 = (uint16_t)(((uint32_t)ox * srcW) / dstW);
                uint16_t sx1 = (uint16_t)(((uint32_t)(ox + 1) * srcW) / dstW);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sx1 > srcW) sx1 = srcW;
                band[(size_t)row * dstW + ox] = areaAverage(frame, bytesPerRow, sx0, sx1, sy0, sy1);
            }
        }
        // addMCU tracks its own x/y via `enc`; each call just needs a
        // pointer to that MCU's top-left pixel in our band buffer.
        for (uint16_t mx = 0; rc == JPEGE_SUCCESS && mx < dstW; mx += 8) {
            rc = jpg.addMCU(&enc, &band[mx], dstW);
        }
    }
    int size = (rc == JPEGE_SUCCESS) ? jpg.close() : 0;
    free(band);

    if (rc != JPEGE_SUCCESS || size <= 0) {
        error = "JPEG encode failed (rc=" + String(rc) + ", size=" + String(size) + ")";
        return false;
    }

    outBuf = gJpegBuf;
    outLen = (size_t)size;
    return true;
}

}  // namespace JpegCapture
