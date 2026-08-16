#pragma once

#include <Arduino.h>

// Persistent history of printed images (both phone-uploaded photos and
// M5StickV camera captures), saved to LittleFS as packed 1bpp bitmaps —
// same format the printer itself consumes, width always
// Printer::kPrintWidthDots. Independent of CameraLink's single in-RAM
// "last frame" — this is a longer, on-disk history the web UI can browse,
// reprint from, and delete from.
//
// Files are named /img_NNNN_HHHH.bin (id, height), so listing/deleting
// never needs a separate index file. Capped at kMaxEntries; once full,
// new saves are skipped (logged) until the user frees space by deleting
// old entries — never auto-evicted, so nothing is lost without the user
// choosing to remove it.
namespace Gallery {

constexpr size_t kMaxEntries = 20;

// Matches CameraLink::kMaxLabelLen; phone uploads never set a label.
constexpr size_t kMaxLabelLen = 31;

struct Entry {
    uint16_t id;
    uint16_t height;
    size_t bytes;
    char label[kMaxLabelLen + 1];  // "" if none
};

void begin();

// One-shot save for callers that already have the whole bitmap in RAM
// (e.g. CameraLink's frame buffer). `label` is an optional short caption
// (e.g. "Face", from on-device detection) shown alongside the entry in the
// web UI; pass "" if there's nothing to say about it. Returns the new
// entry's id, or 0 on failure (including a full gallery).
uint16_t save(const uint8_t *data, size_t len, uint16_t height, const String &label = "");

// Streaming save for callers receiving data in chunks (e.g. an HTTP
// upload) that don't want to buffer the whole image in RAM first.
// beginSave() picks an id and opens the file (0 return = skip: gallery
// full or LittleFS error). feedSave() appends a chunk, returning false on
// a write error (caller should then call cancelSave()). endSave() closes
// and commits the entry (including the label passed to beginSave());
// cancelSave() closes and deletes the partial file. Only one streaming
// save can be in flight at a time.
uint16_t beginSave(uint16_t height, const String &label = "");
bool feedSave(const uint8_t *data, size_t len);
void endSave();
void cancelSave();

// Newest-first list, up to maxCount entries copied into `out`. Returns
// the total number of entries found (always <= kMaxEntries).
size_t list(Entry *out, size_t maxCount);

bool remove(uint16_t id);

// Reprints a saved entry by streaming it straight from flash to the
// printer (never buffers the whole file in RAM).
bool print(uint16_t id);

// Path + height for streaming a saved entry's raw bitmap to the web UI
// (width is always Printer::kPrintWidthDots). Returns false if not found.
bool frameInfo(uint16_t id, String &pathOut, uint16_t &heightOut);

}  // namespace Gallery
