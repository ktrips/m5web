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

// The 「ストレージ設定」card polls sdAvailable() every few seconds (see
// data_html.h's refreshStorageSettings()) so it can grey out the SD
// option live if a card is inserted/removed — but without a cooldown,
// every one of those polls would re-attempt a full SD.begin()/SPI probe
// whenever no card is mounted, which is wasted SPI bus activity on a
// device with no card at all (the common case for flash-only users).
// Retrying only this often still notices a newly-inserted card in a
// reasonable time without hammering the bus continuously.
constexpr unsigned long kMountRetryCooldownMs = 15000;
unsigned long lastMountAttemptMs = 0;
bool everAttempted = false;

const char *backendName(Backend b) { return b == Backend::kSdCard ? "sd" : "flash"; }

// `force` bypasses the cooldown — used by setBackend()/begin(), explicit
// one-shot attempts that should never be silently skipped, unlike
// sdAvailable()'s repeated background polling (see kMountRetryCooldownMs).
bool mountSd(bool force = false) {
    if (sdMounted) return true;
    unsigned long now = millis();
    if (!force && everAttempted && (now - lastMountAttemptMs) < kMountRetryCooldownMs) return false;
    everAttempted = true;
    lastMountAttemptMs = now;

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
    if (saved == Backend::kSdCard && mountSd(/*force=*/true)) {
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
    if (b == Backend::kSdCard && !mountSd(/*force=*/true)) {
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
