#include "dither.h"

#include <string.h>

namespace Dither {

namespace {
constexpr uint16_t kWidth = Printer::kPrintWidthDots;
}

void RowDitherer::reset() {
    memset(carryIn_, 0, sizeof(carryIn_));
}

void RowDitherer::processRow(const uint8_t *grayRow, uint8_t *packedOut) {
    float working[kWidth];
    for (uint16_t x = 0; x < kWidth; x++) {
        working[x] = (float)grayRow[x] + carryIn_[x];
    }

    float downNext[kWidth];
    memset(downNext, 0, sizeof(downNext));
    memset(packedOut, 0, Printer::kPrintWidthBytes);

    for (uint16_t x = 0; x < kWidth; x++) {
        float old = working[x];
        bool black = old < 128;
        if (black) packedOut[x >> 3] |= 0x80 >> (x & 7);
        float err = old - (black ? 0.0f : 255.0f);
        if (x + 1 < kWidth) working[x + 1] += err * 7.0f / 16.0f;
        if (x > 0) downNext[x - 1] += err * 3.0f / 16.0f;
        downNext[x] += err * 5.0f / 16.0f;
        if (x + 1 < kWidth) downNext[x + 1] += err * 1.0f / 16.0f;
    }

    memcpy(carryIn_, downNext, sizeof(downNext));
}

}  // namespace Dither
