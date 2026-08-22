#include "camera_link.h"

#include <Preferences.h>
#include <string.h>

#include "caption.h"
#include "clock.h"
#include "dither.h"
#include "gallery.h"
#include "led.h"
#include "printer.h"

namespace CameraLink {

namespace {

constexpr uint8_t kRxPin = 32;
constexpr uint8_t kTxPin = 26;
constexpr uint32_t kBaud = 115200;

// Capped well below the phone-upload path's limit: a full frame is kept in
// RAM (dithered, 1bpp) so it can be viewed on the web page in either mode.
// 48 bytes/row * 800 rows = 38400 bytes.
constexpr uint16_t kMaxHeightDots = 800;

const uint8_t kMagic[4] = {'M', '5', 'P', 'V'};

enum class RecvState { kWaitMagic, kReadWidth, kReadHeight, kReadLabelLen, kReadLabel, kReadRow, kReadChecksum };

RecvState recvState = RecvState::kWaitMagic;
uint8_t magicMatched = 0;
uint8_t fieldByteIndex = 0;  // byte 0/1 within the width or height field

uint16_t incomingWidth = 0;
uint16_t incomingHeight = 0;
uint16_t rowsReceived = 0;
uint16_t rowByteIndex = 0;

char incomingLabel[kMaxLabelLen + 1];
uint8_t incomingLabelStoreLen = 0;  // bytes actually kept (<= kMaxLabelLen)
uint8_t incomingLabelWireLen = 0;   // bytes the sender declared, always fully consumed
uint8_t labelBytesRead = 0;

uint8_t rowBuf[Printer::kPrintWidthDots];
uint32_t checksumAccum = 0;
Dither::RowDitherer ditherer;

Preferences prefs;
Mode currentMode = Mode::kAuto;
int8_t currentBrightness = 0;  // -100..100, same range/meaning as the JS slider
int8_t currentContrast = 0;
uint16_t currentRotationDeg = 0;  // 0/90/180/270, applied to every frame as it's received

uint8_t frameBuffer[(size_t)Printer::kPrintWidthBytes * kMaxHeightDots];
uint16_t frameHeight = 0;
char frameLabel[kMaxLabelLen + 1] = "";
bool frameReady = false;
bool pendingPrint = false;
uint32_t frameSeq = 0;

bool getBit(const uint8_t *buf, uint16_t bytesPerRow, uint16_t x, uint16_t y) {
    uint8_t byte = buf[(size_t)y * bytesPerRow + (x >> 3)];
    return (byte >> (7 - (x & 7))) & 1;
}

void setBit(uint8_t *buf, uint16_t bytesPerRow, uint16_t x, uint16_t y, bool black) {
    if (!black) return;  // caller pre-zeroes the buffer, so white just means "leave it"
    buf[(size_t)y * bytesPerRow + (x >> 3)] |= 0x80 >> (x & 7);
}

// Rotates frameBuffer 90° clockwise, rescaled back to kPrintWidthDots wide
// so the result stays printable — see the rotate() doc comment in
// camera_link.h for why a plain rotate alone isn't printable. Shared by
// rotate() (manual, one-shot, bumps frameSeq) and finishIncomingFrame()
// (applies currentRotationDeg/90 times to every newly-received frame, no
// frameSeq bump needed since the frame isn't "ready" yet at that point).
//
// The scratch buffer below is heap-allocated per call (freed before
// returning), not a static array: the transform reads and writes
// different (x,y) positions so it can't safely run in place over
// frameBuffer, but a second permanent kPrintWidthBytes*kMaxHeightDots
// buffer alongside frameBuffer overflowed the ATOM Lite's DRAM at link
// time (both are 38400 bytes; rotation is an infrequent, user-triggered
// action, so paying a malloc/free per call is a fine trade for not
// reserving that memory all the time).
void rotateBufferOnce() {
    const uint16_t bytesPerRow = Printer::kPrintWidthBytes;
    const uint16_t w0 = Printer::kPrintWidthDots;  // original width, always 384
    const uint16_t h0 = frameHeight;                // original height

    // A 90°-CW rotation alone would turn w0 x h0 into h0 x w0 — not
    // printable. Rescaled back to width w0 (aspect-preserving) instead, so
    // hf is h0's rotation-and-rescale counterpart: hf = w0 * (w0 / h0).
    uint32_t hf32 = ((uint32_t)w0 * w0) / h0;
    uint16_t hf = (hf32 < 1) ? 1 : (hf32 > kMaxHeightDots ? kMaxHeightDots : (uint16_t)hf32);

    size_t scratchLen = (size_t)bytesPerRow * hf;
    uint8_t *scratch = (uint8_t *)malloc(scratchLen);
    if (!scratch) {
        Serial.println("[camera_link] rotate: out of memory, skipping");
        return;
    }

    // Nearest-neighbor, inverse-mapped: for each output pixel, find the
    // original pixel it came from, combining the rotate and the rescale
    // into one pass. No source grayscale survives past dithering, so this
    // (same bit-level approach as m5paper.ino's own frame scaling) is the
    // best achievable quality for a repeated rotate.
    memset(scratch, 0, scratchLen);
    for (uint16_t yf = 0; yf < hf; yf++) {
        uint16_t x = (uint16_t)(((uint32_t)yf * w0) / hf);  // 0..w0-1
        for (uint16_t xf = 0; xf < w0; xf++) {
            uint16_t xr = (uint16_t)(((uint32_t)xf * h0) / w0);  // 0..h0-1
            uint16_t y = (h0 - 1) - xr;
            setBit(scratch, bytesPerRow, xf, yf, getBit(frameBuffer, bytesPerRow, x, y));
        }
    }

    memcpy(frameBuffer, scratch, scratchLen);
    free(scratch);
    frameHeight = hf;
}

const char *modeName(Mode m) {
    switch (m) {
        case Mode::kPreview: return "preview";
        default: return "auto";
    }
}

Mode modeFromName(const String &s) {
    if (s == "preview") return Mode::kPreview;
    return Mode::kAuto;
}

int8_t clampAdjust(int v) {
    if (v < -100) return -100;
    if (v > 100) return 100;
    return (int8_t)v;
}

// Same brightness/contrast formula as data/index.html's toGrayscale(), so a
// camera frame and a phone-uploaded photo with the same slider values look
// the same. Applied in place, before dithering.
void applyBrightnessContrast(uint8_t *row, uint16_t width) {
    if (currentBrightness == 0 && currentContrast == 0) return;
    float factor = (259.0f * (currentContrast + 255)) / (255.0f * (259 - currentContrast));
    for (uint16_t x = 0; x < width; x++) {
        float g = factor * ((float)row[x] - 128.0f) + 128.0f + currentBrightness;
        if (g < 0) g = 0;
        if (g > 255) g = 255;
        row[x] = (uint8_t)g;
    }
}

void printStoredFrame() {
    // Appended as extra rows after the image, not overlaid onto its own
    // last rows — frameBuffer (also served to the web UI via
    // /api/camera/frame) stays untouched, and a caption never covers real
    // photo content even if e.g. a detected face sits near the bottom.
    String caption = Caption::combine(frameLabel, Clock::nowDateTime().c_str());
    uint16_t bandHeight = (caption.length() > 0 && frameHeight > Caption::kBandHeight) ? Caption::kBandHeight : 0;

    Printer::reset();
    Printer::beginRaster(Printer::kPrintWidthDots, frameHeight + bandHeight);
    Printer::feedRasterChunk(frameBuffer, (size_t)Printer::kPrintWidthBytes * frameHeight);

    if (bandHeight > 0) {
        uint8_t band[Printer::kPrintWidthBytes * Caption::kBandHeight];
        Caption::stamp(band, Printer::kPrintWidthBytes, caption.c_str());
        Printer::feedRasterChunk(band, sizeof(band));
    }

    Printer::endRaster();
}

void resetReceiver() {
    recvState = RecvState::kWaitMagic;
    magicMatched = 0;
    fieldByteIndex = 0;
    rowsReceived = 0;
    rowByteIndex = 0;
}

void startIncomingFrame() {
    ditherer.reset();
    rowsReceived = 0;
    rowByteIndex = 0;
    checksumAccum = 0;
}

void finishIncomingFrame(uint8_t receivedChecksum) {
    bool checksumOk = (uint8_t)(checksumAccum & 0xFF) == receivedChecksum;
    if (!checksumOk) {
        Serial.println("[camera_link] checksum mismatch — holding frame for review instead of auto-printing");
    }
    frameHeight = incomingHeight;
    memcpy(frameLabel, incomingLabel, sizeof(frameLabel));
    for (uint16_t applied = 0; applied < currentRotationDeg; applied += 90) rotateBufferOnce();
    frameReady = true;
    frameSeq++;
    Serial.printf("[camera_link] frame received: %ux%u label=\"%s\" (mode=%s, checksum=%s)\n",
                  Printer::kPrintWidthDots, frameHeight, frameLabel, modeName(currentMode),
                  checksumOk ? "ok" : "FAIL");
    Gallery::save(frameBuffer, (size_t)Printer::kPrintWidthBytes * frameHeight, frameHeight, frameLabel);
    Led::notifyNewImage();
    if (currentMode == Mode::kAuto && checksumOk) {
        pendingPrint = false;
        printStoredFrame();
        Serial.println("[camera_link] auto-printed");
    } else {
        // Preview mode, or a corrupted frame in auto mode: never print
        // without either an explicit checksum pass or a human confirming.
        pendingPrint = true;
    }
    Led::setCameraPending(pendingPrint);
    resetReceiver();
}

void handleByte(uint8_t b) {
    switch (recvState) {
        case RecvState::kWaitMagic:
            if (b == kMagic[magicMatched]) {
                magicMatched++;
                if (magicMatched == 4) {
                    recvState = RecvState::kReadWidth;
                    fieldByteIndex = 0;
                }
            } else {
                magicMatched = (b == kMagic[0]) ? 1 : 0;
            }
            break;

        case RecvState::kReadWidth:
            if (fieldByteIndex == 0) {
                incomingWidth = b;
                fieldByteIndex = 1;
            } else {
                incomingWidth |= ((uint16_t)b << 8);
                fieldByteIndex = 0;
                recvState = RecvState::kReadHeight;
            }
            break;

        case RecvState::kReadHeight:
            if (fieldByteIndex == 0) {
                incomingHeight = b;
                fieldByteIndex = 1;
            } else {
                incomingHeight |= ((uint16_t)b << 8);
                fieldByteIndex = 0;
                if (incomingWidth != Printer::kPrintWidthDots || incomingHeight == 0 ||
                    incomingHeight > kMaxHeightDots) {
                    Serial.println("[camera_link] bad frame header, dropping");
                    resetReceiver();
                } else {
                    // Reset here (before the label) so checksumAccum starts fresh and
                    // picks up label bytes below, in addition to the pixel bytes.
                    startIncomingFrame();
                    recvState = RecvState::kReadLabelLen;
                }
            }
            break;

        case RecvState::kReadLabelLen:
            incomingLabelWireLen = b;
            incomingLabelStoreLen = (b > kMaxLabelLen) ? kMaxLabelLen : b;
            labelBytesRead = 0;
            if (incomingLabelWireLen == 0) {
                incomingLabel[0] = '\0';
                recvState = RecvState::kReadRow;
            } else {
                recvState = RecvState::kReadLabel;
            }
            break;

        case RecvState::kReadLabel:
            if (labelBytesRead < incomingLabelStoreLen) incomingLabel[labelBytesRead] = (char)b;
            checksumAccum += b;
            labelBytesRead++;
            if (labelBytesRead == incomingLabelWireLen) {
                incomingLabel[incomingLabelStoreLen] = '\0';
                recvState = RecvState::kReadRow;
            }
            break;

        case RecvState::kReadRow:
            rowBuf[rowByteIndex++] = b;
            checksumAccum += b;
            if (rowByteIndex == incomingWidth) {
                applyBrightnessContrast(rowBuf, incomingWidth);
                uint8_t *dst = frameBuffer + (size_t)rowsReceived * Printer::kPrintWidthBytes;
                ditherer.processRow(rowBuf, dst);
                rowByteIndex = 0;
                rowsReceived++;
                if (rowsReceived == incomingHeight) {
                    recvState = RecvState::kReadChecksum;
                }
            }
            break;

        case RecvState::kReadChecksum:
            finishIncomingFrame(b);
            break;
    }
}

}  // namespace

void begin() {
    Serial1.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin);
    prefs.begin("m5web_cam", false);
    currentMode = modeFromName(prefs.getString("mode", "auto"));
    currentBrightness = clampAdjust(prefs.getInt("brightness", 0));
    currentContrast = clampAdjust(prefs.getInt("contrast", 0));
    currentRotationDeg = prefs.getUShort("rotationDeg", 0) % 360;
}

void poll() {
    while (Serial1.available()) {
        handleByte((uint8_t)Serial1.read());
    }
}

Mode mode() { return currentMode; }

void setMode(Mode m) {
    currentMode = m;
    prefs.putString("mode", modeName(m));
    Serial.printf("[camera_link] mode set to %s\n", modeName(m));
}

void setAdjust(int brightness, int contrast) {
    currentBrightness = clampAdjust(brightness);
    currentContrast = clampAdjust(contrast);
    prefs.putInt("brightness", currentBrightness);
    prefs.putInt("contrast", currentContrast);
    Serial.printf("[camera_link] brightness=%d contrast=%d\n", currentBrightness, currentContrast);
}

Status status() {
    Status s{currentMode,       frameReady, pendingPrint, Printer::kPrintWidthDots,
              frameHeight,      frameSeq,   currentBrightness, currentContrast,
              currentRotationDeg};
    memcpy(s.label, frameLabel, sizeof(s.label));
    return s;
}

bool printLastFrame() {
    if (!frameReady) return false;
    printStoredFrame();
    pendingPrint = false;  // resolves the pending-review state, if any
    Led::setCameraPending(false);
    return true;
}

void discardPending() {
    pendingPrint = false;
    Led::setCameraPending(false);
}

bool rotate() {
    if (!frameReady) return false;
    uint16_t before = frameHeight;
    rotateBufferOnce();
    frameSeq++;
    Serial.printf("[camera_link] rotated 90deg CW: %ux%u -> %ux%u\n", Printer::kPrintWidthDots, before,
                  Printer::kPrintWidthDots, frameHeight);
    return true;
}

void rotateDefaultBy90() {
    currentRotationDeg = (currentRotationDeg + 90) % 360;
    prefs.putUShort("rotationDeg", currentRotationDeg);
    Serial.printf("[camera_link] default rotation set to %u deg\n", currentRotationDeg);
}

const uint8_t *frameData() { return frameReady ? frameBuffer : nullptr; }

size_t frameDataLen() {
    return frameReady ? (size_t)Printer::kPrintWidthBytes * frameHeight : 0;
}

}  // namespace CameraLink
