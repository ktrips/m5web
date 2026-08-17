#include "caption.h"

#include <string.h>

namespace Caption {

namespace {

constexpr uint8_t kMarginLeft = 4;

}  // namespace

void stamp(uint8_t *band, uint16_t widthBytes, const char *text) {
    if (!band || !text || !text[0]) return;
    memset(band, 0, (size_t)widthBytes * kBandHeight);  // 0 = white, per the printer's 1bpp convention
    Font::drawText(band, widthBytes, kBandHeight, kMarginLeft, kPadTop, text, kScale);
}

String combine(const char *label, const char *timeStr) {
    String out;
    if (label && label[0]) out += label;
    if (timeStr && timeStr[0]) {
        if (out.length() > 0) out += ' ';
        out += timeStr;
    }
    return out;
}

}  // namespace Caption
