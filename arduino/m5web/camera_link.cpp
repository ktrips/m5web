#include "camera_link.h"

#include <Preferences.h>

#include "dither.h"
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

enum class RecvState { kWaitMagic, kReadWidth, kReadHeight, kReadRow, kReadChecksum };

RecvState recvState = RecvState::kWaitMagic;
uint8_t magicMatched = 0;
uint8_t fieldByteIndex = 0;  // byte 0/1 within the width or height field

uint16_t incomingWidth = 0;
uint16_t incomingHeight = 0;
uint16_t rowsReceived = 0;
uint16_t rowByteIndex = 0;

uint8_t rowBuf[Printer::kPrintWidthDots];
uint32_t checksumAccum = 0;
Dither::RowDitherer ditherer;

Preferences prefs;
Mode currentMode = Mode::kAuto;

uint8_t frameBuffer[(size_t)Printer::kPrintWidthBytes * kMaxHeightDots];
uint16_t frameHeight = 0;
bool frameReady = false;
bool pendingPrint = false;
uint32_t frameSeq = 0;

void printStoredFrame() {
    Printer::reset();
    Printer::beginRaster(Printer::kPrintWidthDots, frameHeight);
    Printer::feedRasterChunk(frameBuffer, (size_t)Printer::kPrintWidthBytes * frameHeight);
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
    frameReady = true;
    frameSeq++;
    if (currentMode == Mode::kAuto && checksumOk) {
        pendingPrint = false;
        printStoredFrame();
    } else {
        // Preview mode, or a corrupted frame in auto mode: never print
        // without either an explicit checksum pass or a human confirming.
        pendingPrint = true;
    }
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
                    startIncomingFrame();
                    recvState = RecvState::kReadRow;
                }
            }
            break;

        case RecvState::kReadRow:
            rowBuf[rowByteIndex++] = b;
            checksumAccum += b;
            if (rowByteIndex == incomingWidth) {
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
    currentMode = (prefs.getString("mode", "auto") == "preview") ? Mode::kPreview : Mode::kAuto;
}

void poll() {
    while (Serial1.available()) {
        handleByte((uint8_t)Serial1.read());
    }
}

Mode mode() { return currentMode; }

void setMode(Mode m) {
    currentMode = m;
    prefs.putString("mode", m == Mode::kPreview ? "preview" : "auto");
}

Status status() {
    return Status{currentMode, frameReady, pendingPrint, Printer::kPrintWidthDots, frameHeight, frameSeq};
}

bool confirmPrint() {
    if (!pendingPrint) return false;
    printStoredFrame();
    pendingPrint = false;
    return true;
}

void discardPending() { pendingPrint = false; }

const uint8_t *frameData() { return frameReady ? frameBuffer : nullptr; }

size_t frameDataLen() {
    return frameReady ? (size_t)Printer::kPrintWidthBytes * frameHeight : 0;
}

}  // namespace CameraLink
