#include "printer.h"

namespace Printer {

namespace {
constexpr uint8_t kRxPin = 33;
constexpr uint8_t kTxPin = 23;
constexpr uint32_t kBaud = 9600;

HardwareSerial &port = Serial2;

uint16_t rasterHeightDots = 0;
size_t rasterBytesExpected = 0;
size_t rasterBytesSent = 0;
}  // namespace

void begin() {
    port.begin(kBaud, SERIAL_8N1, kRxPin, kTxPin);
}

void reset() {
    const uint8_t cmd[] = {0x1B, 0x40};  // ESC @
    port.write(cmd, sizeof(cmd));
}

void newLine(uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        port.write('\n');
    }
}

void printText(const String &text) {
    port.print(text);
}

bool beginRaster(uint16_t widthDots, uint16_t heightDots) {
    if (widthDots != kPrintWidthDots || heightDots == 0) {
        return false;
    }
    rasterHeightDots = heightDots;
    rasterBytesExpected = (size_t)kPrintWidthBytes * heightDots;
    rasterBytesSent = 0;

    // GS v 0 m xL xH yL yH — raw raster bit image, mode 0 = normal size.
    // Header bytes are written to a local buffer, never into the caller's
    // pixel data (the vendor library's printBMP() clobbers the first 8
    // bytes of the image buffer to build this header in place — fixed here
    // by keeping the header entirely separate from the pixel stream).
    uint8_t header[8] = {0x1D, 0x76, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00};
    header[3] = 0x00;  // mode: normal
    header[4] = (uint8_t)(kPrintWidthBytes & 0xFF);
    header[5] = (uint8_t)((kPrintWidthBytes >> 8) & 0xFF);
    header[6] = (uint8_t)(heightDots & 0xFF);
    header[7] = (uint8_t)((heightDots >> 8) & 0xFF);
    port.write(header, sizeof(header));
    return true;
}

void feedRasterChunk(const uint8_t *data, size_t len) {
    if (rasterBytesSent + len > rasterBytesExpected) {
        len = rasterBytesExpected - rasterBytesSent;
    }
    if (len == 0) return;
    port.write(data, len);
    rasterBytesSent += len;
}

void endRaster() {
    newLine(3);
    rasterHeightDots = 0;
    rasterBytesExpected = 0;
    rasterBytesSent = 0;
}

bool printRaster(uint16_t widthDots, uint16_t heightDots, const uint8_t *data, size_t len) {
    if (!beginRaster(widthDots, heightDots)) return false;
    feedRasterChunk(data, len);
    endRaster();
    return true;
}

void printTestPage() {
    reset();
    printText("m5web\n");
    printText("ATOM Printer test page\n");
    printText("--------------------------------\n");
    printText("Printer: OK\n");
    newLine(3);
}

void printQRCode(const String &data, QrErrorCorrection ecLevel) {
    // These GS ( k byte sequences mirror M5Stack's stock
    // ATOM_PRINTER_CMD.h exactly (including its trailing 0x00 bytes, which
    // fall outside the pL/pH-declared payload length). This printer's
    // firmware has minor deviations from strict Epson ESC/POS framing, so
    // known-working bytes are reproduced as-is rather than "corrected" to
    // the generic spec.

    // Select error correction level: GS ( k 03 00 31 45 n
    const uint8_t eclCmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x45};
    port.write(eclCmd, sizeof(eclCmd));
    port.write((uint8_t)ecLevel);

    // Store symbol data: GS ( k pL pH 31 50 30 d1...dk 00
    uint16_t len = data.length() + 3;
    const uint8_t storeCmd[] = {0x1D, 0x28, 0x6B, (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
                                 0x31, 0x50, 0x30};
    port.write(storeCmd, sizeof(storeCmd));
    port.print(data);
    port.write((uint8_t)0x00);

    // Print the stored symbol: GS ( k 03 00 31 51 30 00
    const uint8_t printCmd[] = {0x1D, 0x28, 0x6B, 0x03, 0x00, 0x31, 0x51, 0x30, 0x00};
    port.write(printCmd, sizeof(printCmd));
    newLine(3);
}

}  // namespace Printer
