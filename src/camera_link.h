#pragma once

// Receives a grayscale camera frame from an M5StickV over a second, direct
// UART link (no WiFi/HTTP involved) and prints it straight away. Pairs with
// maixpy/m5web_capture.py running on the M5StickV.
//
// Wiring (Grove 4-pin, GND + two signal wires — leave the VCC pins
// unconnected, each board has its own power supply):
//   M5StickV G34 (UART1 TX) -> ATOM Lite G32 (RX)
//   M5StickV G35 (UART1 RX) <- ATOM Lite G26 (TX, currently unused by the
//                               protocol but wired for symmetry/future use)
//   GND                     -- GND
//
// Wire protocol, sent once per frame, no acknowledgement:
//   "M5PV" (4 bytes) | width u16 LE | height u16 LE
//   | width*height grayscale bytes (row-major, 0-255)
//   | 1 checksum byte (sum of all pixel bytes mod 256)
// width must equal Printer::kPrintWidthDots (384). The checksum is
// diagnostic-only: raster rows are streamed straight to the printer as they
// arrive, so a mismatch can only be logged, not un-printed.
namespace CameraLink {

void begin();
void poll();  // call every iteration of the main loop

}  // namespace CameraLink
