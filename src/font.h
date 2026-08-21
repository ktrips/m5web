#pragma once

#include <Arduino.h>

// Minimal 5x7 dot-matrix font (space, 0-9, A-Z, "/" and ":" only —
// lowercase is folded to uppercase, anything else renders blank) for
// stamping short captions
// directly onto a printed 1bpp raster image (e.g. a face-detection label
// from CameraLink/Gallery) — as opposed to Printer::printText()'s separate
// ESC/POS text line, this draws into the image itself.
namespace Font {

constexpr uint8_t kGlyphWidth = 5;
constexpr uint8_t kGlyphHeight = 7;

// Draws `text` (uppercased; unsupported characters render blank) into a
// packed 1bpp bitmap (MSB-first, 1=black, row stride `widthBytes` bytes,
// `bitmapHeight` rows total) with its top-left at (x, y). `scale` repeats
// each font pixel scale x scale times (1 = native 5x7, 2 = 10x14, ...).
// Only ever sets bits (never clears), and silently clips anything that
// would fall outside the bitmap.
void drawText(uint8_t *bitmap, uint16_t widthBytes, uint16_t bitmapHeight, uint16_t x, uint16_t y,
              const char *text, uint8_t scale = 1);

// Width in dots of `text` rendered at the given scale, including the
// 1-dot (unscaled) inter-character gap — handy for centering/right-aligning.
uint16_t textWidth(const char *text, uint8_t scale = 1);

}  // namespace Font
