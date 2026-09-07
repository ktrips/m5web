#include "storage.h"

#include <FFat.h>
#include <Preferences.h>
#include <SD.h>
#include <SPI.h>

namespace Storage {

namespace {

Preferences prefs;
Backend currentBackend = Backend::kFlash;
bool sdMounted = false;
SPIClass sdSpi(HSPI);

const char *backendName(Backend b) { return b == Backend::kSdCard ? "sd" : "flash"; }

bool mountSd() {
    if (sdMounted) return true;
    sdSpi.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin);
    sdMounted = SD.begin(kSdCsPin, sdSpi);
    if (!sdMounted) {
        Serial.println("[storage] SD.begin() failed — check wiring/pins (see storage.h)");
    }
    return sdMounted;
}

}  // namespace

void begin() {
    if (!FFat.begin(true)) {
        // Almost always means the sketch's Partition Scheme has no FAT
        // region at all (formatOnFail=true can't create space that isn't
        // there) — see README.md's CamS3 Arduino IDE setup step for the
        // fix (pick a scheme whose data partition is FAT, e.g. "16M
        // Flash (3MB APP/9.9MB FATFS)"). Every temp-file write
        // (own_camera.cpp's capture, jpeg_print.cpp's external-photo/
        // URL-fetch paths, the gallery) will keep failing with a generic
        // "storage full?" error until this is fixed, so log it loudly
        // and distinctly here.
        Serial.println(
            "[storage] FFat.begin() FAILED — check Tools > Partition Scheme has a FAT "
            "region (see README.md); captures/uploads will fail with \"storage full?\" "
            "until this is fixed");
    }
    prefs.begin("m5cam_storage", false);
    Backend saved = prefs.getUChar("backend", (uint8_t)Backend::kFlash) == (uint8_t)Backend::kSdCard
                        ? Backend::kSdCard
                        : Backend::kFlash;
    if (saved == Backend::kSdCard && mountSd()) {
        currentBackend = Backend::kSdCard;
    } else {
        if (saved == Backend::kSdCard) {
            Serial.println("[storage] saved backend was SD but it's unavailable — using flash for this boot");
        }
        currentBackend = Backend::kFlash;
    }
    Serial.printf("[storage] active backend: %s\n", backendName(currentBackend));
}

Backend backend() { return currentBackend; }

bool setBackend(Backend b) {
    if (b == Backend::kSdCard && !mountSd()) {
        Serial.println("[storage] can't switch to SD — mount failed");
        return false;
    }
    currentBackend = b;
    prefs.putUChar("backend", (uint8_t)b);
    Serial.printf("[storage] switched to: %s\n", backendName(currentBackend));
    return true;
}

bool sdAvailable() { return mountSd(); }

fs::FS &fs() {
    return currentBackend == Backend::kSdCard ? (fs::FS &)SD : (fs::FS &)FFat;
}

}  // namespace Storage
