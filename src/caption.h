#pragma once

#include <Arduino.h>

#include "font.h"

// Stamps a short caption (a face-detection label and/or the print time)
// directly onto a printed image — a white band with black text appended
// as extra rows right after the image, rather than a separate ESC/POS
// text line, so it reads as part of the same printout. Appended rather
// than overlaid onto the image's own last rows so it never covers real
// photo content (e.g. a face positioned near the bottom of the frame).
// Used by CameraLink, Gallery, and the phone-upload print path in
// web_server.cpp wherever they print a frame.
namespace Caption {

constexpr uint8_t kScale = 2;  // 10x14 dots/char — legible at 203dpi without dominating the photo
constexpr uint8_t kPadTop = 2;
constexpr uint8_t kPadBottom = 2;

// Height in dots of the band this module draws — fixed (doesn't depend on
// the caption text), and a compile-time constant so callers can size a
// stack buffer for it directly. Callers clear exactly this many rows at
// the bottom of the image and hand them to stamp() before printing.
constexpr uint16_t kBandHeight = kPadTop + (uint16_t)Font::kGlyphHeight * kScale + kPadBottom;

// Clears `band` (packed 1bpp, MSB-first, kBandHeight rows, row stride
// widthBytes) to white and stamps `text` onto it, left-aligned with a
// small margin. No-op (band left untouched) if text is null/empty — call
// sites should skip reserving the band at all in that case.
void stamp(uint8_t *band, uint16_t widthBytes, const char *text);

// Combines a detection label and/or a timestamp into one caption string,
// space-separated ("FACE 14:32"); either piece may be null/empty and is
// simply omitted. Returns "" if both are empty — callers should skip
// reserving a band at all in that case.
String combine(const char *label, const char *timeStr);

}  // namespace Caption
