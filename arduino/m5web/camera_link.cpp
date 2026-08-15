#include "camera_link.h"

#include <Arduino.h>

#include "dither.h"
#include "printer.h"

namespace CameraLink {

namespace {

constexpr uint8_t kRxPin = 32;
constexpr uint8_t kTxPin = 26;
constexpr uint32_t kBaud = 115200;
constexpr uint16_t kMaxHeightDots = 2000;  // mirrors web_server's cap

const uint8_t kMagic[4] = {'M', '5', 'P', 'V'};

enum class State { kWaitMagic, kReadWidth, kReadHeight, kReadRow, kReadChecksum };

State state = State::kWaitMagic;
uint8_t magicMatched = 0;
uint8_t fieldByteIndex = 0;  // byte 0/1 within the width or height field

uint16_t frameWidth = 0;
uint16_t frameHeight = 0;
uint16_t rowsReceived = 0;
uint16_t rowByteIndex = 0;

uint8_t rowBuf[Printer::kPrintWidthDots];
uint8_t packedBuf[Printer::kPrintWidthBytes];
uint32_t checksumAccum = 0;

bool frameActive = false;
Dither::RowDitherer ditherer;

void resetToIdle() {
    state = State::kWaitMagic;
    magicMatched = 0;
    fieldByteIndex = 0;
    rowsReceived = 0;
    rowByteIndex = 0;
    frameActive = false;
}

void startFrame() {
    ditherer.reset();
    Printer::reset();
    Printer::beginRaster(frameWidth, frameHeight);
    frameActive = true;
    rowsReceived = 0;
    rowByteIndex = 0;
    checksumAccum = 0;
}

void finishFrame(uint8_t receivedChecksum) {
    if (frameActive) {
        Printer::endRaster();
        if ((uint8_t)(checksumAccum & 0xFF) != receivedChecksum) {
            Serial.println("[camera_link] checksum mismatch — frame may be corrupted");
        }
    }
    resetToIdle();
}

void handleByte(uint8_t b) {
    switch (state) {
        case State::kWaitMagic:
            if (b == kMagic[magicMatched]) {
                magicMatched++;
                if (magicMatched == 4) {
                    state = State::kReadWidth;
                    fieldByteIndex = 0;
                }
            } else {
                magicMatched = (b == kMagic[0]) ? 1 : 0;
            }
            break;

        case State::kReadWidth:
            if (fieldByteIndex == 0) {
                frameWidth = b;
                fieldByteIndex = 1;
            } else {
                frameWidth |= ((uint16_t)b << 8);
                fieldByteIndex = 0;
                state = State::kReadHeight;
            }
            break;

        case State::kReadHeight:
            if (fieldByteIndex == 0) {
                frameHeight = b;
                fieldByteIndex = 1;
            } else {
                frameHeight |= ((uint16_t)b << 8);
                fieldByteIndex = 0;
                if (frameWidth != Printer::kPrintWidthDots || frameHeight == 0 ||
                    frameHeight > kMaxHeightDots) {
                    Serial.println("[camera_link] bad frame header, dropping");
                    resetToIdle();
                } else {
                    startFrame();
                    state = State::kReadRow;
                }
            }
            break;

        case State::kReadRow:
            rowBuf[rowByteIndex++] = b;
            checksumAccum += b;
            if (rowByteIndex == frameWidth) {
                ditherer.processRow(rowBuf, packedBuf);
                Printer::feedRasterChunk(packedBuf, Printer::kPrintWidthBytes);
                rowByteIndex = 0;
                rowsReceived++;
                if (rowsReceived == frameHeight) {
                    state = State::kReadChecksum;
                }
            }
            break;

        case State::kReadChecksum:
            finishFrame(b);
            break;
    }
}

}  // namespace

void begin() {
    Serial1.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin);
}

void poll() {
    while (Serial1.available()) {
        handleByte((uint8_t)Serial1.read());
    }
}

}  // namespace CameraLink
