# m5web_capture.py — M5StickV side of the camera link.
#
# Captures a photo on button press and sends it over UART1 (Grove pins
# G34=TX/G35=RX) straight to an ATOM Printer running the m5web firmware
# (see ../src/camera_link.cpp for the receiving end and wiring diagram).
# No WiFi involved — this is a direct wired link between the two boards.
#
# Install: copy this file AND shutter.wav to the M5StickV (MaixPy IDE file
# transfer, or an SD card) — shutter.wav must end up at /flash/shutter.wav
# (or edit SHUTTER_WAV below to point at /sd/shutter.wav if you'd rather
# keep it on an SD card). Then open m5web_capture.py in the MaixPy IDE (or
# copy it to the device as main.py) with the M5StickV connected over USB,
# and run/save it.
#
# NOTE: written and reviewed against MaixPy's documented API, but not
# tested on real M5StickV hardware. If frames come out visually scrambled
# (e.g. diagonally shifted), the most likely cause is to_bytes() not
# matching the row-major byte order assumed here — swap to the slower
# get_pixel(x, y) loop (commented out below) as a fallback.

import sensor
import lcd
import time
import audio
from fpioa_manager import fm
from machine import UART
from Maix import GPIO, I2S
from board import board_info

PRINT_WIDTH = 384  # must match Printer::kPrintWidthDots in src/printer.h
BAUD = 115200
MAGIC = b"M5PV"
SHUTTER_WAV = "/flash/shutter.wav"

lcd.init()
sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.VGA)  # 640x480; downscaled (never upscaled) below
sensor.skip_frames(time=1500)

fm.register(34, fm.fpioa.UART1_TX, force=True)
fm.register(35, fm.fpioa.UART1_RX, force=True)
uart = UART(UART.UART1, BAUD, 8, 0, 0, timeout=1000, read_buf_len=4096)

fm.register(board_info.BUTTON_A, fm.fpioa.GPIO1, force=True)
button = GPIO(GPIO.GPIO1, GPIO.IN, GPIO.PULL_UP)

# Onboard speaker (M5StickV's built-in I2S DAC + amp), per M5Stack's own
# MaixPy example — SPK_SD is the amplifier enable/shutdown pin, must be
# driven high before playback.
fm.register(board_info.SPK_SD, fm.fpioa.GPIO0, force=True)
speaker_enable = GPIO(GPIO.GPIO0, GPIO.OUT)
speaker_enable.value(1)
fm.register(board_info.SPK_DIN, fm.fpioa.I2S1_OUT_D1, force=True)
fm.register(board_info.SPK_BCLK, fm.fpioa.I2S1_SCLK, force=True)
fm.register(board_info.SPK_LRCLK, fm.fpioa.I2S1_WS, force=True)
speaker = I2S(I2S.DEVICE_1)
speaker.channel_config(speaker.CHANNEL_1, I2S.TRANSMITTER, resolution=I2S.RESOLUTION_16_BIT,
                        align_mode=I2S.STANDARD_MODE)


def play_shutter():
    """Best-effort camera-click sound; a missing/bad wav must never block a capture."""
    try:
        player = audio.Audio(path=SHUTTER_WAV)
        info = player.play_process(speaker)
        speaker.set_sample_rate(info[1])
        while True:
            ret = player.play()
            if ret is None or ret == 0:
                break
        player.finish()
    except Exception as e:
        print("shutter sound failed:", e)


def send_frame(img):
    w = PRINT_WIDTH
    h = img.height() * w // img.width()
    img = img.resize(w, h)

    buf = img.to_bytes()  # raw grayscale bytes, row-major, len == w*h

    # Fallback if to_bytes() ever turns out to be wrong on your firmware:
    # buf = bytearray(w * h)
    # i = 0
    # for y in range(h):
    #     for x in range(w):
    #         buf[i] = img.get_pixel(x, y)
    #         i += 1

    header = bytearray(MAGIC)
    header += bytes([w & 0xFF, (w >> 8) & 0xFF, h & 0xFF, (h >> 8) & 0xFF])
    uart.write(header)
    uart.write(buf)
    uart.write(bytes([sum(buf) & 0xFF]))

    return w, h


print("m5web_capture ready. Press the button to capture and print.")

while True:
    img = sensor.snapshot()
    lcd.display(img)

    if button.value() == 0:
        time.sleep_ms(50)  # debounce
        if button.value() == 0:
            still = sensor.snapshot()
            play_shutter()
            lcd.draw_string(4, 4, "Sending...", lcd.WHITE, lcd.BLACK)
            lcd.display(still)
            w, h = send_frame(still)
            print("sent %dx%d" % (w, h))
            while button.value() == 0:  # wait for release before re-arming
                time.sleep_ms(20)

    time.sleep_ms(20)
