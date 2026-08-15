#pragma once

#include <Arduino.h>

// Driver for the 58mm thermal printer bundled with the M5Stack ATOM Printer
// Kit. Talks ESC/POS-compatible commands over UART2 (9600 8N1, TX=G23,
// RX=G33 on ATOM Lite) as documented at
// https://docs.m5stack.com/en/atom/atom_printer
//
// Printer resolution is fixed: 203dpi / 8 dots-per-mm, 384 dots per line
// (58mm head). kPrintWidthDots is that hard limit — raster images must be
// exactly this many dots wide.
namespace Printer {

constexpr uint16_t kPrintWidthDots = 384;      // fixed by the print head
constexpr uint16_t kPrintWidthBytes = kPrintWidthDots / 8;  // 48

void begin();

// ESC @ : resets the printer's line/format state. Call before each job.
void reset();

void newLine(uint8_t count = 1);

// Prints raw text as-is (no escaping/wrapping beyond what the printer does).
void printText(const String &text);

// Streams a 1bpp packed raster image (GS v 0), MSB-first, 1 = black dot.
// widthDots must equal kPrintWidthDots. Sends the raster header once, then
// call feedRasterRow()/feedRasterChunk() repeatedly with exactly
// widthDots/8 * heightDots bytes total, then call endRaster().
// beginRaster/feedRasterChunk let the caller stream data straight from an
// HTTP upload without buffering the whole image in RAM first.
bool beginRaster(uint16_t widthDots, uint16_t heightDots);
void feedRasterChunk(const uint8_t *data, size_t len);
void endRaster();

// Convenience one-shot for small in-memory bitmaps (e.g. the test page logo).
bool printRaster(uint16_t widthDots, uint16_t heightDots, const uint8_t *data, size_t len);

void printTestPage();

enum class QrErrorCorrection : uint8_t { L = 0x48, M = 0x49, Q = 0x4A, H = 0x4B };

// Renders `data` as a QR code using the printer's own built-in 2D-symbol
// generator (GS ( k commands) — no client-side rendering needed. Keep
// `data` reasonably short (a URL is fine); very long payloads can exceed
// the printer's symbol buffer.
void printQRCode(const String &data, QrErrorCorrection ecLevel = QrErrorCorrection::M);

}  // namespace Printer
