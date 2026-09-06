#pragma once

#include <Arduino.h>

// Encodes the M5StickV camera's current stored frame (the same data
// /api/camera/frame serves raw) as a JPEG, for GET /capture — a plain,
// no-/api-prefix snapshot URL matching the usual "network camera"
// convention (Home Assistant's generic camera platform, motion-
// detection tools, etc. commonly expect exactly this shape: GET a URL,
// get an image/jpeg body back, no JSON/multipart involved).
//
// UNTESTED ON REAL HARDWARE, same caveat as jpeg_print.h's JPEG
// *decoding* — this is JPEG *encoding* this time, a second, independent
// risk on top of the same board's known memory constraints (see that
// file, and the openai.* / 俳句生成 history it references). To keep the
// encode's memory bounded, the frame is downscaled first (both
// dimensions, by the same integer factor) if it's taller than
// kMaxOutputHeight (see jpeg_capture.cpp) — the served JPEG is never
// full print resolution when the source frame is tall, only
// capped-and-downscaled. Source data is always the same 1bpp dithered
// black/white bitmap CameraLink stores (this project never keeps a
// full-color/grayscale original), so the JPEG itself is a compressed
// dithered black/white image, not a smooth grayscale/color photo.
//
// Uses a new JPEGENC (bitbank2) dependency — written against that
// library's commonly-documented API (open()/encodeBegin()/addFrame()/
// close(), JPEGE_PIXEL_GRAYSCALE, JPEGE_SUBSAMPLE_NONE, JPEGE_Q_HIGH,
// JPEGE_SUCCESS) without the ability to verify it against the actual
// installed version on real hardware — check jpeg_capture.cpp's calls
// against your installed version's JPEGENC.h if this fails to compile
// or encode.
namespace JpegCapture {

// Encodes the current frame into an internal static buffer (valid until
// the next call — callers must finish using it, e.g. write it to the
// HTTP response, before calling this again). Returns false (no frame
// ever received, or the encoder failed) with a human-readable reason in
// `error`; `outBuf`/`outLen` point at the encoded JPEG on success.
bool encodeCurrentFrame(const uint8_t *&outBuf, size_t &outLen, String &error);

}  // namespace JpegCapture
