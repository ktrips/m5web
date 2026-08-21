#include "gallery.h"

#include <LittleFS.h>
#include <Preferences.h>
#include <stdio.h>

#include "caption.h"
#include "clock.h"
#include "led.h"
#include "printer.h"

namespace Gallery {

namespace {

Preferences prefs;

File writingFile;
String writingPath;
uint16_t writingId = 0;
String writingLabel;
bool writingActive = false;

String pathFor(uint16_t id, uint16_t height) {
    char buf[32];
    snprintf(buf, sizeof(buf), "/img_%04u_%04u.bin", (unsigned)id, (unsigned)height);
    return String(buf);
}

// Labels live in Preferences (NVS), keyed by id, rather than in the
// filename — arbitrary caption text doesn't fit LittleFS's filename
// constraints cleanly, and this way an empty/absent label costs nothing.
String labelKey(uint16_t id) { return "l" + String(id); }

// Save date, same rationale/storage as labelKey() (distinct prefix, same NVS).
String dateKey(uint16_t id) { return "d" + String(id); }

// Strips a leading '/' if present — different core versions of
// File::name() have returned paths with and without it.
const char *bare(const char *name) {
    return (name[0] == '/') ? name + 1 : name;
}

size_t countEntries() {
    size_t count = 0;
    File root = LittleFS.open("/");
    if (!root) return 0;
    File f = root.openNextFile();
    while (f) {
        unsigned id = 0, height = 0;
        if (!f.isDirectory() && sscanf(bare(f.name()), "img_%4u_%4u.bin", &id, &height) == 2) {
            count++;
        }
        f = root.openNextFile();
    }
    root.close();
    return count;
}

// Finds the on-disk path + height for gallery id `id`. Returns "" if not found.
String findPath(uint16_t id, uint16_t *heightOut) {
    File root = LittleFS.open("/");
    if (!root) return "";
    String result;
    File f = root.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            unsigned fid = 0, height = 0;
            if (sscanf(bare(f.name()), "img_%4u_%4u.bin", &fid, &height) == 2 && fid == id) {
                result = String("/") + bare(f.name());
                if (heightOut) *heightOut = (uint16_t)height;
                f.close();
                break;
            }
        }
        f = root.openNextFile();
    }
    root.close();
    return result;
}

}  // namespace

void begin() {
    LittleFS.begin(true);
    prefs.begin("m5web_gallery", false);
    Led::setGalleryNonEmpty(countEntries() > 0);  // reflect photos saved before this boot
}

uint16_t beginSave(uint16_t height, const String &label) {
    if (writingActive) {
        Serial.println("[gallery] a save is already in progress, ignoring");
        return 0;
    }
    if (countEntries() >= kMaxEntries) {
        Serial.println("[gallery] full, skipping save (delete some via the web UI)");
        return 0;
    }
    uint16_t id = (uint16_t)prefs.getUInt("nextId", 1);
    String path = pathFor(id, height);
    writingFile = LittleFS.open(path, "w");
    if (!writingFile) {
        Serial.println("[gallery] failed to open file for writing");
        return 0;
    }
    writingPath = path;
    writingId = id;
    writingLabel = label;
    writingActive = true;
    return id;
}

bool feedSave(const uint8_t *data, size_t len) {
    if (!writingActive) return false;
    return writingFile.write(data, len) == len;
}

void endSave() {
    if (!writingActive) return;
    writingFile.close();
    prefs.putUInt("nextId", writingId + 1);
    if (writingLabel.length() > 0) prefs.putString(labelKey(writingId).c_str(), writingLabel);
    String date = Clock::nowDateTime();  // "" if the clock hasn't synced yet — left unset in that case
    if (date.length() > 0) prefs.putString(dateKey(writingId).c_str(), date);
    Serial.printf("[gallery] saved #%u label=\"%s\" date=\"%s\"\n", writingId, writingLabel.c_str(), date.c_str());
    writingActive = false;
    Led::setGalleryNonEmpty(true);  // this save just succeeded, so the gallery is non-empty
}

void cancelSave() {
    if (!writingActive) return;
    writingFile.close();
    LittleFS.remove(writingPath);
    writingActive = false;
}

uint16_t save(const uint8_t *data, size_t len, uint16_t height, const String &label) {
    uint16_t id = beginSave(height, label);
    if (id == 0) return 0;
    if (!feedSave(data, len)) {
        cancelSave();
        return 0;
    }
    endSave();
    return id;
}

size_t list(Entry *out, size_t maxCount) {
    Entry found[kMaxEntries];
    size_t count = 0;

    File root = LittleFS.open("/");
    if (root) {
        File f = root.openNextFile();
        while (f && count < kMaxEntries) {
            if (!f.isDirectory()) {
                unsigned id = 0, height = 0;
                if (sscanf(bare(f.name()), "img_%4u_%4u.bin", &id, &height) == 2) {
                    found[count].id = (uint16_t)id;
                    found[count].height = (uint16_t)height;
                    found[count].bytes = f.size();
                    prefs.getString(labelKey((uint16_t)id).c_str(), "").toCharArray(found[count].label, sizeof(found[count].label));
                    prefs.getString(dateKey((uint16_t)id).c_str(), "").toCharArray(found[count].savedAt, sizeof(found[count].savedAt));
                    count++;
                }
            }
            f = root.openNextFile();
        }
        root.close();
    }

    // Newest first (highest id first) — simple selection sort, count is tiny.
    for (size_t i = 0; i < count; i++) {
        size_t maxIdx = i;
        for (size_t j = i + 1; j < count; j++) {
            if (found[j].id > found[maxIdx].id) maxIdx = j;
        }
        if (maxIdx != i) {
            Entry tmp = found[i];
            found[i] = found[maxIdx];
            found[maxIdx] = tmp;
        }
    }

    size_t copyCount = count < maxCount ? count : maxCount;
    for (size_t i = 0; i < copyCount; i++) out[i] = found[i];
    return count;
}

bool remove(uint16_t id) {
    String path = findPath(id, nullptr);
    if (path.length() == 0) return false;
    bool ok = LittleFS.remove(path);
    if (ok) {
        prefs.remove(labelKey(id).c_str());
        prefs.remove(dateKey(id).c_str());
        Led::setGalleryNonEmpty(countEntries() > 0);
    }
    return ok;
}

bool print(uint16_t id) {
    uint16_t height = 0;
    String path = findPath(id, &height);
    if (path.length() == 0) return false;
    File f = LittleFS.open(path, "r");
    if (!f) return false;

    // The caption (saved label + fresh print-time timestamp) is appended
    // as extra rows after the image, not overlaid onto its own last rows
    // — the file on disk (also served raw to the web UI via
    // /api/gallery/frame) stays untouched, and a caption never covers
    // real photo content.
    String label = prefs.getString(labelKey(id).c_str(), "");
    String caption = Caption::combine(label.c_str(), Clock::nowDateTime().c_str());
    uint16_t bandHeight = (caption.length() > 0 && height > Caption::kBandHeight) ? Caption::kBandHeight : 0;

    Printer::reset();
    Printer::beginRaster(Printer::kPrintWidthDots, height + bandHeight);

    uint8_t chunk[256];
    while (f.available()) {
        int n = f.read(chunk, sizeof(chunk));  // Stream::read() returns int; -1/0 = stop
        if (n <= 0) break;
        Printer::feedRasterChunk(chunk, (size_t)n);
    }
    f.close();

    if (bandHeight > 0) {
        uint8_t band[Printer::kPrintWidthBytes * Caption::kBandHeight];
        Caption::stamp(band, Printer::kPrintWidthBytes, caption.c_str());
        Printer::feedRasterChunk(band, sizeof(band));
    }

    Printer::endRaster();
    return true;
}

bool frameInfo(uint16_t id, String &pathOut, uint16_t &heightOut) {
    String path = findPath(id, &heightOut);
    if (path.length() == 0) return false;
    pathOut = path;
    return true;
}

}  // namespace Gallery
