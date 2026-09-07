#pragma once

#include <Arduino.h>

// Decodes a JPEG file already sitting on Storage::fs() (flash or SD — see
// storage.h; web_server.cpp's handleExternalPhotoChunk()/own_camera.cpp
// stream a photo there before calling this) and prints it — resized to
// Printer::kPrintWidthDots wide (aspect-preserving), adjusted by the
// persisted 「デフォルト: 明るさ・コントラスト」 values (DefaultAdjust), dithered,
// with the same kind of date/time + optional caption every other print
// path stamps — then saves it to the gallery. Never holds more than one
// ~16-row decode band in RAM at a time (see jpeg_print.cpp's band-buffer
// design) rather than buffering the whole decoded image.
//
// This is CamS3's own copy of m5web's src/jpeg_print.*, trimmed of the
// CameraLink mirroring (CamS3 has no separate "last frame" preview store
// — a capture's result shows up via the gallery instead) and pointed at
// Storage::fs() instead of a hardcoded filesystem.
//
// UNTESTED ON REAL HARDWARE — same caveat as m5web's copy: if it fails
// with an out-of-memory error, lower kDecodeWidthCap (jpeg_print.cpp)
// further; if TJpg_Decoder's actual installed-version API differs from
// what's called here, that's the other likely adjustment point.
namespace JpegPrint {

// `path` must already exist on Storage::fs() as a complete, closed file
// (the caller still owns deleting it afterward, success or failure either
// way). `label`/`location` are optional caption pieces; both "" means the
// caption is just the timestamp (itself omitted too if the clock hasn't
// synced yet). Returns false with a human-readable reason in `error` on
// any failure: not a valid JPEG, an aspect ratio too tall for a
// Printer::kPrintWidthDots-wide print, out of memory, or a decode error.
bool printFromFile(const String &path, const String &label, const String &location, String &error);

// Capped well below printFromFile()'s own kMaxHeightDots — a preview is
// held in RAM (via decodeToBuffer() below) rather than streamed straight
// to the printer, so it needs a much smaller, fixed bound. Matches
// m5web's own CameraLink::kMaxHeightDots for the same reason.
constexpr uint16_t kPreviewMaxHeightDots = 800;

// Decodes `path`'s JPEG the same way printFromFile() does (resize,
// brightness/contrast, dither) but writes the result into `outBuf`
// (caller-owned, at least Printer::kPrintWidthBytes * kPreviewMaxHeightDots
// bytes) instead of printing it or touching the gallery — used by
// own_camera.cpp's プレビュー確認 capture mode to render a preview without
// committing to a print. `outHeight` receives the decoded height (capped
// at kPreviewMaxHeightDots). Returns false with a reason in `error` on
// failure.
bool decodeToBuffer(const String &path, uint8_t *outBuf, uint16_t &outHeight, String &error);

// Fetches `url` via a plain HTTP GET, streams the response straight to a
// Storage::fs() temp file (capped at the same size the direct-upload path
// enforces, never buffered whole in RAM), then hands it to
// printFromFile() exactly as if it had arrived via the multipart upload
// path. Deletes its temp file when done either way.
//
// `url` must start with "http://" — https:// is rejected outright (same
// TLS/heap-headroom reasoning as m5web's copy).
bool fetchAndPrint(const String &url, const String &label, const String &location, String &error);

}  // namespace JpegPrint
