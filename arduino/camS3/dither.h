#pragma once

#include <Arduino.h>

#include "printer.h"

// Streaming Floyd–Steinberg dithering, one row at a time, so a whole frame
// never has to sit in RAM. Mirrors the same algorithm (and threshold/error
// weights) as the client-side JS ditherFloydSteinberg() in data/index.html
// — used here for frames arriving over CameraLink instead of a browser
// upload.
namespace Dither {

class RowDitherer {
   public:
    RowDitherer() { reset(); }

    // Call once before the first row of a new frame.
    void reset();

    // grayRow: Printer::kPrintWidthDots bytes, 0-255.
    // packedOut: Printer::kPrintWidthBytes bytes, MSB-first, 1 = black.
    void processRow(const uint8_t *grayRow, uint8_t *packedOut);

   private:
    // Error diffused down from the row above, carried into the row this
    // processRow() call is about to produce.
    float carryIn_[Printer::kPrintWidthDots];
};

}  // namespace Dither
