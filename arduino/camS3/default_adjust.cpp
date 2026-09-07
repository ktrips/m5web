#include "default_adjust.h"

#include <Preferences.h>

namespace DefaultAdjust {

namespace {
Preferences prefs;
int8_t currentBrightness = 0;
int8_t currentContrast = 0;

int8_t clamp(int v) {
    if (v < -100) return -100;
    if (v > 100) return 100;
    return (int8_t)v;
}
}  // namespace

void begin() {
    prefs.begin("m5cam_adj", false);
    currentBrightness = clamp(prefs.getInt("brightness", 0));
    currentContrast = clamp(prefs.getInt("contrast", 0));
}

int8_t brightness() { return currentBrightness; }
int8_t contrast() { return currentContrast; }

void setAdjust(int brightness, int contrast) {
    currentBrightness = clamp(brightness);
    currentContrast = clamp(contrast);
    prefs.putInt("brightness", currentBrightness);
    prefs.putInt("contrast", currentContrast);
}

}  // namespace DefaultAdjust
