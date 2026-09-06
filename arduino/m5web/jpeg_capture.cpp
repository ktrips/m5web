#include "jpeg_capture.h"

#include <JPEGENC.h>

#include "camera_link.h"
#include "printer.h"

namespace JpegCapture {

namespace {

// Keeps the grayscale-input buffer this encode needs comfortably within
// this board's limited heap — the source frame (up to CameraLink's own
// internal height cap) is downscaled to at most this many rows before
// encoding, same idea as jpeg_print.cpp's kDecodeWidthCap on the decode
// side. A tall source frame is downscaled in BOTH dimensions by the same
// integer factor (see the scale calculation below), so the actual buffer
// shrinks well below the worst-case 384*kMaxOutputHeight in practice.
constexpr uint16_t kMaxOutputHeight = 300;

// Generous cap for the encoded JPEG itself — a dithered black/white
// image compresses less predictably than a smooth photo (more
// high-frequency noise for the DCT to encode), so this is sized well
// above the grayscale input it's encoding, not tightly to it.
constexpr size_t kJpegBufCap = 64 * 1024;

uint8_t *gJpegBuf = nullptr;  // allocated once on first use, reused across calls

// Nearest-neighbor sample of the packed 1bpp CameraLink frame at
// (x, y) in *source* (pre-downscale) coordinates — same bit layout
// camera_link.cpp's own getBit() reads (MSB-first, 1=black), duplicated
// here rather than exported since it's a one-line lookup.
uint8_t sampleGray(const uint8_t *frame, uint16_t bytesPerRow, uint16_t x, uint16_t y) {
    uint8_t byte = frame[(size_t)y * bytesPerRow + (x >> 3)];
    bool black = (byte >> (7 - (x & 7))) & 1;
    return black ? 0 : 255;
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
    uint16_t dstW = srcW / scale;
    uint16_t dstH = srcH / scale;
    // JPEG's DCT works in 8x8 blocks — round down to a multiple of 8 so
    // the encoder never has to pad an odd edge itself.
    dstW -= dstW % 8;
    dstH -= dstH % 8;
    if (dstW == 0) dstW = 8;
    if (dstH == 0) dstH = 8;

    uint8_t *gray = (uint8_t *)malloc((size_t)dstW * dstH);
    if (!gray) {
        error = "out of memory (grayscale buffer)";
        return false;
    }
    for (uint16_t oy = 0; oy < dstH; oy++) {
        uint16_t sy = (uint16_t)(((uint32_t)oy * srcH) / dstH);
        if (sy >= srcH) sy = srcH - 1;
        for (uint16_t ox = 0; ox < dstW; ox++) {
            uint16_t sx = (uint16_t)(((uint32_t)ox * srcW) / dstW);
            if (sx >= srcW) sx = srcW - 1;
            gray[(size_t)oy * dstW + ox] = sampleGray(frame, bytesPerRow, sx, sy);
        }
    }

    if (!gJpegBuf) gJpegBuf = (uint8_t *)malloc(kJpegBufCap);
    if (!gJpegBuf) {
        free(gray);
        error = "out of memory (JPEG output buffer)";
        return false;
    }

    JPEGENC jpg;
    int rc = jpg.open(gJpegBuf, kJpegBufCap);
    if (rc == JPEGE_SUCCESS) {
        rc = jpg.encodeBegin(dstW, dstH, JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_NONE, JPEGE_Q_HIGH);
    }
    if (rc == JPEGE_SUCCESS) {
        rc = jpg.addFrame(gray, dstW);
    }
    int size = (rc == JPEGE_SUCCESS) ? jpg.close() : 0;
    free(gray);

    if (rc != JPEGE_SUCCESS || size <= 0) {
        error = "JPEG encode failed";
        return false;
    }

    outBuf = gJpegBuf;
    outLen = (size_t)size;
    return true;
}

}  // namespace JpegCapture
