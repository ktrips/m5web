#pragma once

#include <Arduino.h>

// Persisted "デフォルト: 明るさ・コントラスト" values applied to every photo
// this board prints (its own camera captures and phone-uploaded/external
// photos alike) before dithering — same role m5web's CameraLink::status()
// brightness/contrast fields play there, split out into its own tiny
// module here since CamS3 has no CameraLink (no UART camera link to hang
// it off of). -100..100, 0 = no adjustment.
namespace DefaultAdjust {

void begin();

int8_t brightness();
int8_t contrast();

// Clamped to -100..100.
void setAdjust(int brightness, int contrast);

}  // namespace DefaultAdjust
