# m5web_capture.py — M5StickV side of the camera link.
#
# Captures a photo on button press and sends it over UART1 (Grove pins
# G34=TX/G35=RX) straight to an ATOM Printer running the m5web firmware
# (see ../src/camera_link.cpp for the receiving end and wiring diagram).
# No WiFi involved — this is a direct wired link between the two boards.
#
# Also runs on-device face detection (Haar cascade, CPU-based — no extra
# kmodel file needed): a box is drawn around any detected face in the live
# preview, and on capture a short label ("Face" / "Face x2" / ...) is sent
# to the ATOM alongside the image, to show as a caption on the m5web page.
#
# Install: copy this file AND shutter.wav to the M5StickV (MaixPy IDE file
# transfer, or an SD card) — shutter.wav must end up at /flash/shutter.wav
# (or edit SHUTTER_WAV below to point at /sd/shutter.wav if you'd rather
# keep it on an SD card). Then open m5web_capture.py in the MaixPy IDE (or
# copy it to the device as main.py) with the M5StickV connected over USB,
# and run/save it.
#
# NOTE: written and reviewed against MaixPy's documented API, tested and
# adjusted against feedback from real M5StickV hardware (camera rotation,
# face-detection memory/resolution — see the comments where each is set),
# but not exhaustively verified. If frames come out visually scrambled
# (e.g. diagonally shifted), the most likely cause is to_bytes() not
# matching the row-major byte order assumed here — swap to the slower
# get_pixel(x, y) loop (commented out below) as a fallback.

import sensor
import lcd
import time
import audio
import image
import gc
from fpioa_manager import fm
from machine import UART
from Maix import GPIO, I2S
from board import board_info

PRINT_WIDTH = 384  # must match Printer::kPrintWidthDots in src/printer.h
BAUD = 115200
MAGIC = b"M5PV"
SHUTTER_WAV = "/flash/shutter.wav"
MAX_LABEL_LEN = 31  # must match CameraLink::kMaxLabelLen / Gallery::kMaxLabelLen in src/

lcd.init()
sensor.reset()
sensor.set_pixformat(sensor.GRAYSCALE)
sensor.set_framesize(sensor.VGA)  # 640x480; downscaled (never upscaled) below
sensor.skip_frames(time=1500)

# Camera mount correction: the sensor is physically rotated relative to how
# the M5StickV is held (LED-side up) — confirmed on real hardware that a
# 90° clockwise rotation is needed for captured photos to come out upright
# in that orientation. There's no hardware-level fix for this on this
# MaixPy build (sensor.set_transpose() raised AttributeError — it isn't
# available here), so send_frame() below does the rotation itself, sampling
# pixels directly off the original frame rather than building a rotated
# copy — see that function's docstring for why (short version: even one
# ~196KB intermediate buffer is too much for this board's heap).
#
# NOTE: this only corrects the transmitted/printed photo, not the M5StickV's
# own on-screen live preview (still native sensor orientation) — rotating
# every live frame would be far too slow for a real-time preview loop.

fm.register(34, fm.fpioa.UART1_TX, force=True)
fm.register(35, fm.fpioa.UART1_RX, force=True)
uart = UART(UART.UART1, BAUD, 8, 0, 0, timeout=1000, read_buf_len=4096)

fm.register(board_info.BUTTON_A, fm.fpioa.GPIO1, force=True)
button = GPIO(GPIO.GPIO1, GPIO.IN, GPIO.PULL_UP)

# Onboard RGB LED: white while a capture is in progress, a brief green
# blink once the frame has been sent, then off. Active-low (0=on, 1=off).
fm.register(board_info.LED_W, fm.fpioa.GPIO2, force=True)
fm.register(board_info.LED_G, fm.fpioa.GPIO3, force=True)
led_w = GPIO(GPIO.GPIO2, GPIO.OUT)
led_g = GPIO(GPIO.GPIO3, GPIO.OUT)
led_w.value(1)
led_g.value(1)

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

# Haar-cascade face detection — CPU-based, built into MaixPy's image module,
# so it works without needing a separate kmodel file flashed to the board
# (unlike the KPU-accelerated demos). Runs directly on the GRAYSCALE feed
# already used for printing, no pixformat change needed.
FACE_CASCADE = image.HaarCascade("frontalface")
# The K210's Haar cascade (find_features) has thrown "Out of normal
# MicroPython Heap Memory!" on real hardware, which is why detect_faces()
# runs on a downsampled copy rather than the full VGA frame — but too small
# a working image (previously 80x60/128x96 here) means a face doesn't leave
# enough detail to match the cascade at all, so nothing is ever detected
# even though nothing crashes either. These are a middle ground: if
# MemoryError comes back, raise the divisors (smaller working image, less
# heap, less accurate); if detection is still unreliable, lower them
# (bigger working image, more heap risk, more accurate) or tune
# threshold/scale_factor below.
FACE_DETECT_DIVISOR_LIVE = 4     # live preview: 640x480 -> 160x120
FACE_DETECT_DIVISOR_CAPTURE = 3  # one-off check at capture time: 640x480 -> 213x160
FACE_DETECT_EVERY_N_FRAMES = 5   # throttle live detection; box is redrawn from the last result in between


def detect_faces(img, divisor):
    """Returns face rects [(x, y, w, h), ...] in img's own coordinate space.
    Haar cascade is heap-hungry on the K210 (its own internal scratch
    buffers scale with input size, on top of whatever we pass it) — a
    MemoryError here is treated as "no faces found" rather than crashing
    the capture, since detection is a nice-to-have on top of the core
    photo-print flow, never a requirement for it."""
    gc.collect()  # maximize free heap before the attempt, not just after
    try:
        # img.resize() returns a new, independent image — no img.copy()
        # needed first, that would waste heap duplicating the full-res frame.
        target = img.resize(img.width() // divisor, img.height() // divisor) if divisor > 1 else img
        faces = target.find_features(FACE_CASCADE, threshold=0.5, scale_factor=1.25)
        if divisor > 1:
            faces = [(x * divisor, y * divisor, w * divisor, h * divisor) for (x, y, w, h) in faces]
        return faces
    except MemoryError as e:
        print("face detection skipped (out of heap):", e)
        return []
    finally:
        gc.collect()


def face_label(faces):
    if not faces:
        return ""
    if len(faces) == 1:
        return "Face"
    return "Face x%d" % len(faces)


def flash_screen():
    """Brief full-screen white flash for shutter feedback — M5StickV has no
    dedicated indicator LED, so the LCD itself doubles as one."""
    lcd.clear(lcd.WHITE)


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


def send_frame(img, label=""):
    """Resizes (to PRINT_WIDTH, preserving aspect) and rotates 90° CW, then
    streams the result to the ATOM one row at a time — computed straight
    from `img`'s own pixel buffer via get_pixel(), never materializing a
    resized/rotated copy of the whole image.

    This isn't the obvious way to do it (img.resize() + a from-bytes
    rotation is simpler code) but building even a single ~196KB buffer
    (384x512, this file's actual PRINT_WIDTH-scaled size) has thrown
    "MemoryError: memory allocation failed" on real hardware — this
    board's MicroPython heap can't supply that from what's apparently a
    fairly small/fragmented free pool, even though `img` itself (the
    sensor's own frame buffer, VGA = 640x480 = 307200 bytes) already
    exists without issue. A 384-byte row buffer, reused every iteration,
    stays far under whatever that ceiling actually is.

    The resize + 90° CW rotation are combined into one direct
    native-pixel lookup per output pixel (see the loop below) rather than
    done as two separate passes, so no intermediate image/buffer of any
    size is ever created — see the note above `sensor.set_pixformat()`
    for why a rotation is needed here at all. Necessarily slower than a
    buffer-based approach (~200K individual get_pixel() calls), but that
    trades time — already-tolerated multi-second budget alongside the
    UART transfer and shutter sound — for memory, which is the one we
    don't have to spare here.
    """
    native_w = img.width()
    native_h = img.height()
    final_w = PRINT_WIDTH  # fixed by the printer; this is the width *after* rotation
    pre_h = final_w  # pre-rotation resize target height (becomes final_w after rotation)
    pre_w = native_w * pre_h // native_h  # pre-rotation resize target width, aspect-preserving
    final_h = pre_w  # post-rotation height (== pre-rotation width)

    label_bytes = label.encode("utf-8")[:MAX_LABEL_LEN]

    header = bytearray(MAGIC)
    header += bytes([final_w & 0xFF, (final_w >> 8) & 0xFF, final_h & 0xFF, (final_h >> 8) & 0xFF])
    header += bytes([len(label_bytes)])
    header += label_bytes
    uart.write(header)

    checksum = sum(label_bytes) & 0xFF
    row = bytearray(final_w)
    for fy in range(final_h):
        nx = fy * native_w // pre_w  # constant across the row — see the docstring's derivation
        for fx in range(final_w):
            ny = (final_w - 1 - fx) * native_h // pre_h
            row[fx] = img.get_pixel(nx, ny)
        uart.write(row)
        checksum = (checksum + sum(row)) & 0xFF
    uart.write(bytes([checksum]))

    return final_w, final_h


print("m5web_capture ready. Press the button to capture and print.")

frame_count = 0
last_faces = []

while True:
    img = sensor.snapshot()

    frame_count += 1
    if frame_count % FACE_DETECT_EVERY_N_FRAMES == 0:
        last_faces = detect_faces(img, FACE_DETECT_DIVISOR_LIVE)
        print("live face detect: %d found" % len(last_faces))
    for (fx, fy, fw, fh) in last_faces:
        img.draw_rectangle(fx, fy, fw, fh, color=255, thickness=2)

    lcd.display(img)

    if button.value() == 0:
        time.sleep_ms(50)  # debounce
        if button.value() == 0:
            still = sensor.snapshot()
            faces = detect_faces(still, FACE_DETECT_DIVISOR_CAPTURE)
            label = face_label(faces)
            led_w.value(0)  # white on: capture/send in progress
            flash_screen()
            play_shutter()  # blocks while the wav plays; screen stays white meanwhile
            status_text = "Sending... " + label if label else "Sending..."
            lcd.draw_string(4, 4, status_text, lcd.WHITE, lcd.BLACK)
            lcd.display(still)
            w, h = send_frame(still, label)
            led_w.value(1)  # white off
            led_g.value(0)  # green on: send complete
            time.sleep_ms(400)
            led_g.value(1)  # green off
            print("sent %dx%d label=%r" % (w, h, label))
            while button.value() == 0:  # wait for release before re-arming
                time.sleep_ms(20)

    time.sleep_ms(20)
