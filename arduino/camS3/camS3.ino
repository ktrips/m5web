// camS3.ino — M5Stack Unit CamS3-5MP standalone web camera/printer
// ("m5cam.local"), with two switchable modes (see printer_mode.h, and the
// 「接続方式」card on either web UI below):
//
//   ATOM経由 (kViaAtom, the default — original camS3.ino behavior, no
//   config needed): CamS3 is just a camera. A capture is POSTed straight
//   to an ATOM Lite running m5web's `/api/print/photo` (multipart JPEG
//   upload; see ../../src/jpeg_print.* and README.md's
//   「外部プログラムからの写真送信（/api/print/photo）」section) — no ATOM
//   Lite firmware changes needed. All printing/gallery/settings live on
//   m5web.local; this device's own page is a small status/shutter/mode
//   card (see web_server.cpp's kLightPage), no filesystem needed at all.
//
//   直接接続 (kDirect): a thermal printer is wired straight to this board
//   (see printer.h's configurable TX/RX pins) and CamS3 runs the same
//   menu m5web does — gallery, text/QR/haiku printing, Wi-Fi, OpenAI —
//   served from data/index.html on Storage::fs() (flash or SD, see
//   storage.h). See own_camera.h for the capture->print pipeline.
//
// Target hardware: M5Stack Unit CamS3-5MP (ESP32-S3-WROOM-1-N16R8,
// PY260/OV2640 5MP sensor depending on hardware revision — see M5Stack's
// own docs to confirm yours, docs.m5stack.com/en/arduino/m5unitcams3_5mp).
// This unit has NO onboard button or display, so a photo is triggered one
// of two ways (either works, use whichever is convenient):
//   (1) Ground GPIO0 briefly (an external push button between GPIO0 and
//       GND, matching M5Stack's own example wiring) — read as
//       INPUT_PULLUP and debounced in loop().
//   (2) HTTP GET to this device's own `/shutter` endpoint (e.g.
//       `curl http://m5cam.local/shutter` from a phone/script/automation),
//       for a no-extra-hardware remote trigger.
// The onboard LED (GPIO14, a plain single-color fill-light LED, not an
// addressable RGB one — no white/green color-coding is physically
// possible here) blinks slowly while waiting for WiFi setup; during a
// capture it's held solid on for the whole capture+print/forward, then
// blinks once on success or 3x on failure once it's done — see
// handleTrigger()/handleCaptureRequest()'s setLed()/blinkLed() calls.
//
// Arduino IDE setup:
//   - Board: "M5UnitCAMS3" if your board package provides it, otherwise a
//     generic "ESP32S3 Dev Module" works too as long as the settings below
//     are set manually.
//   - Tools menu: USB CDC On Boot = Enabled, PSRAM = OPI PSRAM (required —
//     a 5MP-capable JPEG frame buffer needs PSRAM; capture will fail
//     without it, see initCamera()'s error logging).
//   - arduino-esp32 core v3.3.0 or later (older versions may not recognize
//     this sensor / the pin_sccb_* config field names used below).
//   - esp32-camera (`esp_camera.h`) and WiFi/HTTPClient/WebServer/
//     DNSServer/Preferences/ESPmDNS/FFat/SD/SPI all ship inside the ESP32
//     Arduino core — no extra install needed for kViaAtom mode. kDirect
//     mode additionally needs the same **TJpg_Decoder** (Bodmer) library
//     m5web's own JPEG-upload path uses (see jpeg_print.h) — install it
//     from the Library Manager if you plan to use kDirect mode.
//   - kDirect mode's page is embedded in the firmware (see data_html.h) —
//     no separate data/ upload step needed, unlike m5web's own
//     data/index.html (see storage.h for why: this board's realistic
//     16MB-flash partition schemes are FAT-only, and the usual LittleFS-
//     upload IDE plugins don't target FAT partitions). kViaAtom mode's
//     page is embedded the same way.
//
// WiFi setup: no credentials are hardcoded in this file, same as
// arduino/m5paper/m5paper.ino and src/wifi_manager.cpp. On first boot (or
// whenever the saved network can't be reached), this opens its own AP
// ("camS3-setup-XXXX") with a small captive-portal web page — connect a
// phone to that AP, pick/enter the real network and password there, and
// it's saved to NVS (Preferences) for future boots.
//
// Pin table confirmed against M5Stack's own official CameraWebServer
// example (CAMERA_MODEL_M5STACK_CAMS3_UNIT in their board_config.h) —
// matches exactly. WiFi onboarding, /shutter, /status, and the LED all
// confirmed working on real hardware too. The one real-hardware finding:
// esp_camera_init() must NOT be called with a small frame_size (e.g.
// SVGA) directly on this sensor/driver combo — it fails with esp_err_t
// 0x20002 (ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE). Fixed by
// initializing at FRAMESIZE_UXGA (matching the official example's own
// PSRAM-present default) and downsizing to kFrameSize via a runtime
// set_framesize() call right after — see initCamera().
//   - Sensor identity: M5Stack shipped this unit with different sensors
//     across hardware revisions (OV2640, then PY260) — the esp32-camera
//     driver auto-detects via SCCB probe, so no sensor-specific code
//     should be needed, but very old core versions may not recognize a
//     newer sensor's ID.

#include "esp_camera.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

#include "clock.h"
#include "default_adjust.h"
#include "gallery.h"
#include "haiku.h"
#include "openai.h"
#include "own_camera.h"
#include "printer.h"
#include "printer_mode.h"
#include "storage.h"
#include "web_server.h"

// ---- target: the ATOM Lite running m5web, in kViaAtom mode (see
// printer_mode.h) — configurable from the web UI, PrinterMode::atomHost(),
// defaulting to "m5web.local". ESP32 Arduino generally resolves ".local"
// fine; if it doesn't on your network, set an IP address instead (see
// m5web's own README.md / `pio device monitor`).
constexpr uint16_t M5WEB_PORT = 80;

// ---- camera pins (M5Stack Unit CamS3-5MP; see docs.m5stack.com's
// m5unitcams3_5mp/web_cam example for this exact table) ----
#define PWDN_GPIO_NUM -1  // not wired on this unit — software reset only
#define RESET_GPIO_NUM 21
#define XCLK_GPIO_NUM 11
#define SIOD_GPIO_NUM 17
#define SIOC_GPIO_NUM 41
#define Y9_GPIO_NUM 13
#define Y8_GPIO_NUM 4
#define Y7_GPIO_NUM 10
#define Y6_GPIO_NUM 5
#define Y5_GPIO_NUM 7
#define Y4_GPIO_NUM 16
#define Y3_GPIO_NUM 15
#define Y2_GPIO_NUM 6
#define VSYNC_GPIO_NUM 42
#define HREF_GPIO_NUM 18
#define PCLK_GPIO_NUM 12

constexpr uint8_t kLedPin = 14;      // onboard fill-light LED
constexpr uint8_t kTriggerPin = 0;   // GPIO0, external button to GND (see file header)
constexpr unsigned long kDebounceMs = 300;  // ignore re-triggers within this long of the last one

// Deliberately NOT this unit's full FRAMESIZE_QSXGA (2592x1944, 5MP) —
// m5web's print head is a fixed 384-dot-wide thermal head, and
// /api/print/photo resizes+dithers down to that regardless of input size
// (see README.md's size-limit note: 400KB cap, long edge ~2000px
// suggested). Sending full 5MP would mean a much larger, slower upload
// and a heavier decode on the ATOM Lite's own limited (no-PSRAM) heap for
// zero visible benefit on a 384-dot print. SVGA (800x600) already exceeds
// the print head's resolution comfortably. Bump this (e.g. FRAMESIZE_XGA
// or FRAMESIZE_QSXGA) if you have another use for the full-res JPEG, but
// expect a slower round-trip and re-check the 400KB cap.
constexpr framesize_t kFrameSize = FRAMESIZE_SVGA;
constexpr int kJpegQuality = 12;  // 0 (best/largest) .. 63 (worst/smallest); 10-15 is a reasonable print-quality range

// ---- WiFi setup (AP mode + captive portal) — same pattern as
// arduino/m5paper/m5paper.ino / src/wifi_manager.cpp, minus the display
// calls (this unit has none; Serial + the LED are the only feedback) ----
constexpr uint8_t kDnsPort = 53;
const IPAddress kApIP(192, 168, 4, 1);
Preferences wifiPrefs;
DNSServer dnsServer;
WebServer setupServer(80);
String apSsid;

bool ledOn = false;
void setLed(bool on) {
    ledOn = on;
    digitalWrite(kLedPin, on ? HIGH : LOW);
}

void blinkLed(int times, int onMs, int offMs) {
    for (int i = 0; i < times; i++) {
        setLed(true);
        delay(onMs);
        setLed(false);
        if (i + 1 < times) delay(offMs);
    }
}

// ---- camera ----

bool initCamera() {
    camera_config_t config = {};
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    // Init at UXGA (the sensor's own default/safe size), not kFrameSize
    // directly — M5Stack's own CameraWebServer example for this exact
    // board does the same (see its board_config.h /
    // CAMERA_MODEL_M5STACK_CAMS3_UNIT setup) and it matters: initializing
    // esp_camera_init() straight at a smaller size like SVGA fails on
    // this sensor/driver combo with esp_err_t 0x20002
    // (ESP_ERR_CAMERA_FAILED_TO_SET_FRAME_SIZE), confirmed on real
    // hardware. Downsizing to kFrameSize is instead done via a runtime
    // set_framesize() call right after init below, which is the path
    // that actually works.
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = kJpegQuality;
    config.fb_count = 2;  // needs PSRAM (see file header) — lets the driver start filling the next frame while we send the last one
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // one fresh shot per trigger, not a rolling live-stream buffer

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[camera] esp_camera_init failed: 0x%x\n", err);
        return false;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor) {
        err = sensor->set_framesize(sensor, kFrameSize);
        if (err != 0) {
            Serial.printf("[camera] set_framesize(kFrameSize) failed: %d — capturing at UXGA instead\n", err);
        }
    }

    // Discard the first few frames after init/framesize change before
    // trusting a capture. Confirmed on real hardware: the very first
    // esp_camera_fb_get() right after esp_camera_init()/set_framesize()
    // comes back solid black — the sensor's auto-exposure/auto-white-
    // balance hasn't converged yet (and grab_mode=CAMERA_GRAB_WHEN_EMPTY
    // can also just hand back a stale buffer from before the framesize
    // change). A handful of warm-up captures, spaced out to give AEC/AGC
    // real time between frames, fixes it — same fix M5Stack's own
    // CameraWebServer example and most ESP32-camera examples apply.
    for (int i = 0; i < 5; i++) {
        camera_fb_t *warm = esp_camera_fb_get();
        if (warm) esp_camera_fb_return(warm);
        delay(150);
    }

    Serial.println("[camera] initialized");
    return true;
}

// Captures one JPEG frame and POSTs it to m5web's /api/print/photo as a
// multipart/form-data upload. Builds the multipart header/trailer as small
// strings and writes the JPEG bytes straight from the camera driver's own
// buffer in between — same "write the raw bytes directly, don't copy them
// into a second buffer first" approach m5web's own web_server.cpp uses for
// serving frames (see its handleCameraFrame()/handleCapture()).
// POSTs raw JPEG bytes to PrinterMode::atomHost()'s /api/print/photo as
// a multipart upload — the primitive sendJpegFileToAtom() (below) builds
// on to send an already-captured file (own_camera.cpp's capture/preview
// paths — see camS3_actions.h — read the bytes off Storage::fs() first).
bool sendJpegBytesToAtom(const uint8_t *buf, size_t len, String &resultMsg) {
    String host = PrinterMode::atomHost();
    WiFiClient client;
    bool ok = client.connect(host.c_str(), M5WEB_PORT);
    if (!ok) {
        resultMsg = "connect to " + host + " failed";
        return false;
    }

    const char *boundary = "----camS3Boundary7MA4YWxk";
    String head = String("--") + boundary + "\r\n" +
                  "Content-Disposition: form-data; name=\"photo\"; filename=\"camS3.jpg\"\r\n" +
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";
    size_t contentLength = head.length() + len + tail.length();

    client.print(String("POST /api/print/photo HTTP/1.1\r\n") + "Host: " + host + "\r\n" +
                 "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
                 "Content-Length: " + String(contentLength) + "\r\n" + "Connection: close\r\n\r\n");
    client.print(head);
    client.write(buf, len);
    client.print(tail);

    // Read the status line (and drain the rest) with a generous timeout —
    // the ATOM Lite's own decode+dither+print can take a few seconds.
    unsigned long start = millis();
    String statusLine;
    while (client.connected() || client.available()) {
        if (client.available()) {
            statusLine = client.readStringUntil('\n');
            break;
        }
        if (millis() - start > 20000) {
            client.stop();
            resultMsg = "timed out waiting for m5web's response";
            return false;
        }
        delay(10);
    }
    client.stop();

    // "HTTP/1.1 200 OK" -> take the 3-digit code out of the middle.
    int code = 0;
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace >= 0) code = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();

    if (code == 200) {
        resultMsg = "printed";
        return true;
    }
    resultMsg = "m5web returned " + String(code) + " (" + statusLine + ")";
    return false;
}

// Sends an already-captured JPEG file (see own_camera.cpp's kPreview
// mode, which decodes-and-holds a capture for confirmation before this
// gets called) to ATOM instead of taking a fresh photo — used when the
// 撮影方式 setting is preview and PrinterMode is kViaAtom (see
// camS3_actions.h).
bool sendJpegFileToAtom(const String &path, String &resultMsg) {
    File f = Storage::fs().open(path, "r");
    if (!f) {
        resultMsg = "pending capture file missing";
        return false;
    }
    size_t len = f.size();
    uint8_t *buf = (uint8_t *)malloc(len);
    if (!buf) {
        f.close();
        resultMsg = "out of memory reading pending capture";
        return false;
    }
    f.read(buf, len);
    f.close();
    bool ok = sendJpegBytesToAtom(buf, len, resultMsg);
    free(buf);
    return ok;
}

// Mode-aware capture, shared by the GPIO0 button and web_server.cpp's
// POST /api/camera/capture + GET /shutter (see camS3_actions.h). The
// 撮影方式 setting (OwnCamera::mode()) decides preview-vs-immediate
// regardless of PrinterMode; OwnCamera itself decides print-locally vs.
// forward-to-ATOM based on PrinterMode at capture/confirm time.
void handleTrigger(const char *source) {
    Serial.printf("[shutter] triggered via %s\n", source);
    setLed(true);  // on for the whole capture — see file header's LED note
    String resultMsg;
    bool ok;
    if (OwnCamera::mode() == OwnCamera::Mode::kPreview) {
        ok = OwnCamera::captureForPreview(resultMsg);
        if (ok) resultMsg = "captured — open the 撮影 card to review and print";
    } else {
        ok = OwnCamera::captureAndCommitNow(resultMsg);
        if (ok) resultMsg = "printed";
    }
    setLed(false);
    Serial.printf("[shutter] %s: %s\n", ok ? "ok" : "FAILED", resultMsg.c_str());
    blinkLed(ok ? 1 : 3, ok ? 400 : 120, 120);
}

// ---- WiFi setup (AP mode + captive portal) ----
// Mirrors arduino/m5paper/m5paper.ino's runSetupAP()/ensureWifiConnected()
// almost exactly, minus the on-screen status (replaced with Serial +
// blinkLed() here, since this unit has no display).

const char kSetupPage[] PROGMEM = R"HTML(<!doctype html><html><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CamS3 Wi-Fi設定</title>
<style>
body{font-family:-apple-system,sans-serif;max-width:420px;margin:24px auto;padding:0 16px;color:#222}
label{display:block;margin:14px 0 4px;font-size:14px;color:#555}
select,input[type=text],input[type=password]{width:100%;padding:10px;font-size:16px;box-sizing:border-box}
button{width:100%;padding:12px;margin-top:18px;font-size:16px;background:#3d8bfd;color:#fff;border:none;border-radius:6px}
#msg{margin-top:12px;font-size:14px}
.manual-line{display:flex;align-items:center;gap:6px;margin-top:10px;font-size:14px}
</style></head>
<body>
<h2>CamS3 Wi-Fi設定</h2>
<p>ATOM Lite（m5web）と同じWi-Fiネットワークを選んでください。</p>
<label>ネットワーク</label>
<select id="ssidSelect"><option value="">スキャン中…</option></select>
<div class="manual-line"><input type="checkbox" id="manual"><label for="manual" style="margin:0">SSIDを手入力する</label></div>
<input type="text" id="manualSsid" placeholder="SSID" style="display:none;margin-top:8px">
<label>パスワード</label>
<input type="password" id="pass">
<button id="btn">接続する</button>
<div id="msg"></div>
<script>
fetch('/api/wifi/scan').then(function(r){return r.json();}).then(function(list){
  var sel=document.getElementById('ssidSelect');
  sel.innerHTML='';
  if(list.length===0){sel.innerHTML='<option value="">見つかりませんでした</option>';}
  list.sort(function(a,b){return b.rssi-a.rssi;}).forEach(function(n){
    var o=document.createElement('option');
    o.value=n.ssid; o.textContent=n.ssid+(n.open?'':' [lock]');
    sel.appendChild(o);
  });
}).catch(function(){document.getElementById('ssidSelect').innerHTML='<option value="">スキャン失敗</option>';});
document.getElementById('manual').addEventListener('change',function(e){
  document.getElementById('manualSsid').style.display=e.target.checked?'block':'none';
  document.getElementById('ssidSelect').style.display=e.target.checked?'none':'block';
});
document.getElementById('btn').addEventListener('click',function(){
  var manual=document.getElementById('manual').checked;
  var ssid=manual?document.getElementById('manualSsid').value.trim():document.getElementById('ssidSelect').value;
  var pass=document.getElementById('pass').value;
  var msg=document.getElementById('msg');
  if(!ssid){msg.textContent='SSIDを入力してください';return;}
  msg.textContent='接続中…';
  var xhr=new XMLHttpRequest();
  xhr.open('POST','/api/wifi');
  xhr.setRequestHeader('Content-Type','application/x-www-form-urlencoded');
  xhr.onload=function(){
    if(xhr.status===200){msg.textContent='接続成功。スマホのWi-Fiを元に戻してください。';}
    else{msg.textContent='失敗: '+xhr.responseText;}
  };
  xhr.onerror=function(){msg.textContent='本機のAPが終了した可能性があります。';};
  xhr.send('ssid='+encodeURIComponent(ssid)+'&password='+encodeURIComponent(pass));
});
</script></body></html>)HTML";

void handleSetupRoot() { setupServer.send_P(200, "text/html", kSetupPage); }

// Matches src/wifi_manager.cpp's jsonEscape() — a raw SSID could in theory
// contain a quote/backslash, which would otherwise break the JSON.
String jsonEscape(const String &in) {
    String out;
    out.reserve(in.length() + 4);
    for (size_t i = 0; i < in.length(); i++) {
        char c = in[i];
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

void handleSetupScan() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    setupServer.send(200, "application/json", json);
}

bool tryStationConnect(const String &ssid, const String &password, unsigned long timeoutMs) {
    Serial.printf("[wifi] connecting to '%s'...\n", ssid.c_str());
    WiFi.begin(ssid.c_str(), password.c_str());
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - start > timeoutMs) {
            Serial.printf("[wifi] connect to '%s' timed out\n", ssid.c_str());
            return false;
        }
        delay(200);
    }
    Serial.printf("[wifi] connected: ssid=%s ip=%s rssi=%ddBm\n", ssid.c_str(), WiFi.localIP().toString().c_str(),
                  WiFi.RSSI());
    return true;
}

// Reused by web_server.cpp's /api/wifi/scan + /api/wifi (runtime network
// switch, distinct from the AP-mode captive-portal handlers above) — see
// camS3_actions.h.
String wifiScanJson() {
    int n = WiFi.scanNetworks();
    String json = "[";
    for (int i = 0; i < n; i++) {
        if (i > 0) json += ",";
        json += "{\"ssid\":\"" + jsonEscape(WiFi.SSID(i)) + "\",";
        json += "\"rssi\":" + String(WiFi.RSSI(i)) + ",";
        json += "\"open\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    json += "]";
    WiFi.scanDelete();
    return json;
}

bool wifiConnectAndSave(const String &ssid, const String &password) {
    bool ok = tryStationConnect(ssid, password, 8000);
    if (ok) {
        wifiPrefs.putString("ssid", ssid);
        wifiPrefs.putString("pass", password);
    }
    return ok;
}

void handleSetupConnect() {
    if (!setupServer.hasArg("ssid") || setupServer.arg("ssid").length() == 0) {
        setupServer.send(400, "text/plain", "ssid required");
        return;
    }
    String ssid = setupServer.arg("ssid");
    String password = setupServer.hasArg("password") ? setupServer.arg("password") : "";

    bool ok = tryStationConnect(ssid, password, 8000);
    if (ok) {
        wifiPrefs.putString("ssid", ssid);
        wifiPrefs.putString("pass", password);
    }
    setupServer.send(ok ? 200 : 400, "text/plain", ok ? "OK" : "Failed to join that network");
}

// Starts the setup AP + captive-portal web server, and blocks (servicing
// DNS/HTTP requests, blinking the LED slowly to show it's waiting) until a
// phone successfully submits working credentials via handleSetupConnect().
// Returns with WiFi already connected as a station.
void runSetupAP() {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    char nameBuf[24];
    snprintf(nameBuf, sizeof(nameBuf), "camS3-setup-%02X%02X", mac[4], mac[5]);
    apSsid = nameBuf;

    WiFi.mode(WIFI_OFF);
    delay(100);
    WiFi.mode(WIFI_AP_STA);  // AP for the phone to join + STA so we can try joining the real network
    WiFi.setSleep(false);

    bool apOk = WiFi.softAP(apSsid.c_str());
    dnsServer.start(kDnsPort, "*", kApIP);
    Serial.printf("[wifi] AP setup mode: softAP()=%s ssid=%s ip=%s mac=%s\n", apOk ? "ok" : "FAILED",
                  apSsid.c_str(), WiFi.softAPIP().toString().c_str(), WiFi.softAPmacAddress().c_str());
    Serial.printf("[wifi] connect a phone to '%s' and open http://%s\n", apSsid.c_str(), kApIP.toString().c_str());

    setupServer.on("/", HTTP_GET, handleSetupRoot);
    setupServer.onNotFound(handleSetupRoot);  // captive-portal probes land on the setup page too
    setupServer.on("/api/wifi/scan", HTTP_GET, handleSetupScan);
    setupServer.on("/api/wifi", HTTP_POST, handleSetupConnect);
    setupServer.begin();

    unsigned long lastBlink = 0;
    while (WiFi.status() != WL_CONNECTED) {
        dnsServer.processNextRequest();
        setupServer.handleClient();
        if (millis() - lastBlink > 800) {
            lastBlink = millis();
            setLed(!ledOn);
        }
        delay(2);
    }
    setLed(false);

    setupServer.stop();
    dnsServer.stop();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
}

// Tries the saved network first (if any); falls back to runSetupAP() —
// which blocks until it has good credentials — so this always returns
// with WiFi connected.
void ensureWifiConnected() {
    wifiPrefs.begin("camS3", false);
    String savedSsid = wifiPrefs.getString("ssid", "");
    String savedPass = wifiPrefs.getString("pass", "");

    WiFi.mode(WIFI_STA);
    bool connected = false;
    if (savedSsid.length() > 0) {
        connected = tryStationConnect(savedSsid, savedPass, 15000);
        if (!connected) Serial.println("[wifi] saved network unreachable, entering setup mode");
    } else {
        Serial.println("[wifi] no saved network, entering setup mode");
    }

    if (!connected) runSetupAP();
}

void setup() {
    Serial.begin(115200);
    delay(200);
    Serial.println("\n=== camS3 starting ===");

    pinMode(kLedPin, OUTPUT);
    setLed(false);
    pinMode(kTriggerPin, INPUT_PULLUP);

    if (!initCamera()) {
        Serial.println("[camera] init FAILED — capture will not work; check pin table / PSRAM setting (see file header)");
    }

    ensureWifiConnected();

    PrinterMode::begin();  // decides which of the initialization below actually runs
    Serial.printf("[wifi] ready: ip=%s, mode=%s\n", WiFi.localIP().toString().c_str(),
                  PrinterMode::mode() == PrinterMode::Mode::kDirect ? "direct" : "via_atom");

    // Storage/DefaultAdjust/OwnCamera are needed regardless of
    // PrinterMode — own_camera.cpp's capture/preview path always writes
    // its temp JPEG through Storage::fs() and always reads
    // DefaultAdjust's brightness/contrast, even when the eventual commit
    // is a forward to ATOM rather than a local print (see own_camera.h's
    // doc comment). Order matters: Storage before Gallery/OwnCamera/
    // web_server (all read/write through Storage::fs()).
    Storage::begin();
    DefaultAdjust::begin();
    OwnCamera::begin();

    if (PrinterMode::mode() == PrinterMode::Mode::kDirect) {
        // Own-printer/own-gallery init — CamS3 has no printer/gallery of
        // its own to run in kViaAtom mode (see printer_mode.h's doc
        // comment). Clock/Haiku/OpenAI are only ever read from this
        // mode's own web UI (俳句設定/OpenAI設定 cards, and Caption's
        // timestamp stamping) — via_atom mode never touches them (ATOM
        // Lite stamps its own timestamp with its own Clock module once
        // the forwarded photo arrives there).
        Printer::begin();
        Gallery::begin();
        Clock::begin();
        Haiku::begin();
        OpenAI::begin();
    }

    WebServer_::begin();

    if (MDNS.begin("m5cam")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[wifi] mDNS: http://m5cam.local/");
    }

    blinkLed(2, 150, 150);  // "ready" signal

    Serial.println("=== camS3 ready — open http://m5cam.local/, ground GPIO0, or GET /shutter to take a photo ===");
}

bool lastTriggerState = HIGH;
unsigned long lastTriggerMs = 0;

void checkButtonTrigger() {
    bool state = digitalRead(kTriggerPin);
    if (lastTriggerState == HIGH && state == LOW && (millis() - lastTriggerMs) > kDebounceMs) {
        lastTriggerMs = millis();
        handleTrigger("GPIO0 button");
    }
    lastTriggerState = state;
}

constexpr unsigned long kHeartbeatIntervalMs = 15000;
unsigned long lastHeartbeatMs = 0;

void printHeartbeat() {
    unsigned long now = millis();
    if (now - lastHeartbeatMs < kHeartbeatIntervalMs) return;
    lastHeartbeatMs = now;
    bool connected = WiFi.status() == WL_CONNECTED;
    Serial.printf("[status] uptime=%lus heap=%u psram=%u wifi=%s rssi=%ddBm\n", now / 1000, ESP.getFreeHeap(),
                  ESP.getFreePsram(), connected ? "connected" : "DISCONNECTED", WiFi.RSSI());
}

void loop() {
    WebServer_::loop();
    checkButtonTrigger();
    printHeartbeat();
}
