// camS3.ino — M5Stack Unit CamS3-5MP wireless shutter for m5web.
//
// Captures a JPEG photo with the CamS3's onboard camera and sends it to the
// ATOM Lite (m5web) over WiFi to be dithered and printed — no ATOM Lite
// firmware changes needed. This is purely a new HTTP client of the
// already-existing `/api/print/photo` endpoint (multipart JPEG upload; see
// ../../src/jpeg_print.* and README.md's
// 「外部プログラムからの写真送信（/api/print/photo）」section), the same way
// arduino/m5paper/m5paper.ino is a new client of `/api/gallery*` — no
// UART link like the M5StickV (../../src/camera_link.*) is needed here
// since the CamS3, like the M5PaperColor, has its own WiFi radio.
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
//       `curl http://cams3.local/shutter` from a phone/script/automation),
//       for a no-extra-hardware remote trigger.
// The onboard LED (GPIO14, a plain fill-light LED, not an addressable
// RGB one) blinks slowly while waiting for WiFi setup, and flashes once
// around each capture as a shutter indicator — see setLed()/blinkLed().
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
//   - No extra libraries to install: the esp32-camera driver
//     (`esp_camera.h`) ships inside the ESP32 Arduino core itself, and
//     WiFi/HTTPClient/WebServer/DNSServer/Preferences/ESPmDNS all do too.
//
// WiFi setup: no credentials are hardcoded in this file, same as
// arduino/m5paper/m5paper.ino and src/wifi_manager.cpp. On first boot (or
// whenever the saved network can't be reached), this opens its own AP
// ("camS3-setup-XXXX") with a small captive-portal web page — connect a
// phone to that AP, pick/enter the real network and password there, and
// it's saved to NVS (Preferences) for future boots.
//
// UNTESTED ON REAL HARDWARE: written against M5Stack's own documented
// Unit CamS3-5MP pin table and example (docs.m5stack.com's web_cam guide)
// but not verified against actual hardware. Likely spots to double-check:
//   - Pin assignments below (XCLK/SIOD/SIOC/Y2-Y9/VSYNC/HREF/PCLK/RESET/
//     LED) — sourced from the official docs table; if the camera fails to
//     init (see initCamera()'s Serial output for the exact esp_err_t), a
//     pin mismatch is the first thing to check.
//   - Sensor identity: M5Stack shipped this unit with different sensors
//     across hardware revisions (OV2640, then PY260) — the esp32-camera
//     driver auto-detects via SCCB probe, so no sensor-specific code
//     should be needed, but very old core versions may not recognize a
//     newer sensor's ID.
//   - kFrameSize below defaults to SVGA (800x600) rather than this unit's
//     full 5MP — see that constant's comment for why.

#include "esp_camera.h"
#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ---- target: the ATOM Lite running m5web ----
// ESP32 Arduino generally resolves ".local" fine; if it doesn't on your
// network, replace this with the ATOM's IP address instead (see m5web's
// own README.md / `pio device monitor`).
const char *M5WEB_HOST = "m5web.local";
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

// ---- runtime HTTP server (started once WiFi is up) — the remote-trigger
// path described in the file header, alongside the GPIO0 button ----
WebServer runtimeServer(80);

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
    config.frame_size = kFrameSize;
    config.jpeg_quality = kJpegQuality;
    config.fb_count = 2;  // needs PSRAM (see file header) — lets the driver start filling the next frame while we send the last one
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // one fresh shot per trigger, not a rolling live-stream buffer

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("[camera] esp_camera_init failed: 0x%x\n", err);
        return false;
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
bool captureAndSend(String &resultMsg) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        resultMsg = "capture failed (esp_camera_fb_get returned null)";
        return false;
    }
    Serial.printf("[camera] captured %ux%u, %u bytes\n", fb->width, fb->height, (unsigned)fb->len);

    setLed(true);  // on for the duration of the upload — doubles as a "busy" indicator

    WiFiClient client;
    bool ok = client.connect(M5WEB_HOST, M5WEB_PORT);
    if (!ok) {
        esp_camera_fb_return(fb);
        setLed(false);
        resultMsg = String("connect to ") + M5WEB_HOST + " failed";
        return false;
    }

    const char *boundary = "----camS3Boundary7MA4YWxk";
    String head = String("--") + boundary + "\r\n" +
                  "Content-Disposition: form-data; name=\"photo\"; filename=\"camS3.jpg\"\r\n" +
                  "Content-Type: image/jpeg\r\n\r\n";
    String tail = String("\r\n--") + boundary + "--\r\n";
    size_t contentLength = head.length() + fb->len + tail.length();

    client.print(String("POST /api/print/photo HTTP/1.1\r\n") + "Host: " + M5WEB_HOST + "\r\n" +
                 "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
                 "Content-Length: " + String(contentLength) + "\r\n" + "Connection: close\r\n\r\n");
    client.print(head);
    client.write(fb->buf, fb->len);
    client.print(tail);

    esp_camera_fb_return(fb);  // driver's buffer no longer needed once the bytes are on the wire

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
            setLed(false);
            resultMsg = "timed out waiting for m5web's response";
            return false;
        }
        delay(10);
    }
    client.stop();
    setLed(false);

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

void handleTrigger(const char *source) {
    Serial.printf("[shutter] triggered via %s\n", source);
    String resultMsg;
    bool ok = captureAndSend(resultMsg);
    Serial.printf("[shutter] %s: %s\n", ok ? "ok" : "FAILED", resultMsg.c_str());
    blinkLed(ok ? 1 : 3, ok ? 400 : 120, 120);
}

// ---- runtime HTTP server: the remote-trigger path (see file header) ----

void handleShutterRequest() {
    String resultMsg;
    bool ok = captureAndSend(resultMsg);
    Serial.printf("[shutter] (http) %s: %s\n", ok ? "ok" : "FAILED", resultMsg.c_str());
    runtimeServer.send(ok ? 200 : 502, "text/plain", resultMsg);
}

void handleStatusRequest() {
    String json = "{";
    json += "\"uptimeSec\":" + String(millis() / 1000) + ",";
    json += "\"freeHeap\":" + String(ESP.getFreeHeap()) + ",";
    json += "\"freePsram\":" + String(ESP.getFreePsram()) + ",";
    json += "\"wifiRssi\":" + String(WiFi.RSSI()) + ",";
    json += "\"m5webHost\":\"" + String(M5WEB_HOST) + "\"";
    json += "}";
    runtimeServer.send(200, "application/json", json);
}

void startRuntimeServer() {
    runtimeServer.on("/shutter", HTTP_GET, handleShutterRequest);
    runtimeServer.on("/status", HTTP_GET, handleStatusRequest);
    runtimeServer.begin();
    Serial.println("[http] runtime server ready: GET /shutter, GET /status");
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
    Serial.printf("[wifi] ready: ip=%s -> printing to %s\n", WiFi.localIP().toString().c_str(), M5WEB_HOST);

    if (MDNS.begin("cams3")) {
        MDNS.addService("http", "tcp", 80);
        Serial.println("[wifi] mDNS: http://cams3.local/shutter");
    }

    startRuntimeServer();
    blinkLed(2, 150, 150);  // "ready" signal

    Serial.println("=== camS3 ready — ground GPIO0 or GET /shutter to take a photo ===");
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
    runtimeServer.handleClient();
    checkButtonTrigger();
    printHeartbeat();
}
