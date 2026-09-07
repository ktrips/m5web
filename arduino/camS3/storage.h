#pragma once

#include <Arduino.h>
#include <FS.h>

// Selects which filesystem CamS3's own gallery/temp files live on: the
// module's internal flash or a microSD card (SPI), selectable from the
// 設定タブ「ストレージ設定」card (/api/storage/settings) without
// reflashing. m5web (ATOM Lite) has no SD slot, so it doesn't use this
// module — it keeps calling LittleFS directly.
//
// Internal-flash backend is **FFat** (FAT), not LittleFS — deliberately.
// M5Stack's own board definition for CamS3 (and its 16MB-flash siblings)
// only offers small (<=1.5MB) SPIFFS/LittleFS partition-scheme presets;
// the presets actually sized for this board's 16MB flash (e.g. "16M
// Flash (3MB APP/9.9MB FATFS)", `app3M_fat9M_16MB`) are FAT-only —
// confirmed on real hardware: LittleFS.begin() can never mount on those
// (there's no spiffs-subtype partition at all), which is also why
// index.html is embedded in the firmware (see data_html.h) instead of
// needing a separate LittleFS-upload step — the usual "Upload LittleFS
// to..." IDE plugins don't target FAT partitions.
//
// Both FFat (fs::FFatFS) and SD (fs::SDFS) derive from fs::FS, so every
// caller elsewhere in this sketch (Gallery, JpegPrint) can stay written
// against a plain fs::FS& and not care which backend is actually active.
//
// UNTESTED ON REAL HARDWARE — the SD SPI pins below follow M5Stack's
// published Unit CamS3-5MP pin table but haven't been confirmed against
// actual hardware (same caveat camS3.ino's own camera pin table already
// carries). If SD::begin() fails, double-check these against your unit's
// docs first.
namespace Storage {

enum class Backend { kFlash, kSdCard };

// SPI pins for the microSD slot — adjust these if your CamS3 revision's
// docs list different numbers (see file header).
constexpr uint8_t kSdCsPin = 3;
constexpr uint8_t kSdSckPin = 46;
constexpr uint8_t kSdMisoPin = 39;
constexpr uint8_t kSdMosiPin = 38;

// Restores the saved backend choice from NVS (default kFlash) and mounts
// it. If the saved choice was kSdCard but the card can't be mounted
// (missing/removed/wiring issue), falls back to kFlash for this boot
// without changing the saved preference — so a card pulled out doesn't
// permanently lock the user out of the flash fallback.
void begin();

Backend backend();

// Switches backends immediately (no reboot needed) and persists the
// choice. Returns false (backend unchanged) if kSdCard was requested but
// the card can't be mounted.
bool setBackend(Backend b);

// True if an SD card was detected as mountable, independent of which
// backend is currently active — lets the web UI grey out the "SD" option
// if not applicable rather than offering a choice that will just fail.
bool sdAvailable();

// The currently active filesystem. Every other module in this sketch
// (Gallery, JpegPrint, web_server) should read/write through this rather
// than calling FFat/SD directly.
fs::FS &fs();

}  // namespace Storage
