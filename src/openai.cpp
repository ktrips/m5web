#include "openai.h"

#include <HTTPClient.h>
#include <Preferences.h>
#include <WiFiClientSecure.h>

namespace OpenAI {

namespace {

// Small and cheap while still vision-capable as of writing — swap this if
// OpenAI retires it; nothing else here assumes a specific model.
constexpr const char *kModel = "gpt-4o-mini";

// No literal '"' or '\' in here — embedded directly into a JSON string
// literal in generateHaiku() below, so keep it that way if this is edited.
constexpr const char *kPrompt =
    "この画像に写っている情景にぴったり合う、季語を含む五七五の俳句を1句だけ日本語で作ってください。"
    "俳句の3行だけを出力し、説明や前置き、記号（「」など）は付けないでください。";

Preferences prefs;
String apiKey;

// Finds the first top-level `"<key>":"..."` string field in `json` and
// returns it JSON-unescaped (\", \\, \/, \n, \t, \uXXXX). Not a real JSON
// parser — the ATOM Lite side of this project deliberately has no JSON
// library dependency (see platformio.ini), and OpenAI's response shape
// here is simple/predictable enough that this is safe: `content` only
// ever appears once (the haiku text), and error responses only ever
// nest one `message` field.
String extractJsonString(const String &json, const char *key) {
    String needle = String("\"") + key + "\":\"";
    int keyPos = json.indexOf(needle);
    if (keyPos < 0) return "";
    int start = keyPos + needle.length();
    int len = json.length();

    String out;
    out.reserve(len - start);
    for (int i = start; i < len; i++) {
        char c = json[i];
        if (c == '"') break;
        if (c == '\\' && i + 1 < len) {
            char next = json[++i];
            switch (next) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': break;
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'u':
                    if (i + 4 < len) {
                        char hex[5] = {json[i + 1], json[i + 2], json[i + 3], json[i + 4], '\0'};
                        uint16_t cp = (uint16_t)strtol(hex, nullptr, 16);
                        i += 4;
                        // BMP -> UTF-8. No surrogate-pair handling — haiku
                        // text stays within the BMP, so this is sufficient.
                        if (cp < 0x80) {
                            out += (char)cp;
                        } else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                    }
                    break;
                default: out += next; break;
            }
        } else {
            out += c;
        }
    }
    return out;
}

}  // namespace

void begin() {
    prefs.begin("m5web_openai", false);
    apiKey = prefs.getString("apiKey", "");
}

bool hasKey() { return apiKey.length() > 0; }

void setKey(const String &key) {
    apiKey = key;
    prefs.putString("apiKey", apiKey);
    Serial.printf("[openai] API key %s\n", apiKey.length() > 0 ? "set" : "cleared");
}

bool generateHaiku(const String &base64Png, String &haikuOut, String &errorOut) {
    if (apiKey.length() == 0) {
        errorOut = "OpenAI APIキーが未設定です（設定タブで登録してください）";
        return false;
    }
    if (base64Png.length() == 0) {
        errorOut = "画像がありません";
        return false;
    }

    WiFiClientSecure client;
    // No certificate pinning — matches this project's existing "trusted
    // LAN, no auth" security posture (see README) rather than embedding
    // and having to rotate OpenAI's root CA. Traffic is still
    // TLS-encrypted end-to-end; only server identity verification is
    // skipped.
    client.setInsecure();
    client.setTimeout(20000);

    HTTPClient http;
    http.setTimeout(20000);
    if (!http.begin(client, "https://api.openai.com/v1/chat/completions")) {
        errorOut = "OpenAIへの接続開始に失敗しました";
        return false;
    }
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Authorization", "Bearer " + apiKey);

    String body;
    body.reserve(base64Png.length() + 512);
    body += "{\"model\":\"";
    body += kModel;
    body += "\",\"max_tokens\":150,\"messages\":[{\"role\":\"user\",\"content\":[";
    body += "{\"type\":\"text\",\"text\":\"";
    body += kPrompt;
    body += "\"},{\"type\":\"image_url\",\"image_url\":{\"url\":\"data:image/png;base64,";
    body += base64Png;
    body += "\"}}]}]}";

    Serial.printf("[openai] POST /v1/chat/completions (%u byte body)\n", (unsigned)body.length());
    int code = http.POST(body);
    if (code <= 0) {
        errorOut = "OpenAIへの通信に失敗しました（" + http.errorToString(code) + "）";
        http.end();
        return false;
    }
    String respBody = http.getString();
    http.end();

    if (code != 200) {
        String msg = extractJsonString(respBody, "message");
        errorOut = "OpenAI APIエラー (" + String(code) + ")" + (msg.length() ? ": " + msg : "");
        Serial.printf("[openai] error %d: %s\n", code, respBody.c_str());
        return false;
    }

    String content = extractJsonString(respBody, "content");
    content.trim();
    if (content.length() == 0) {
        errorOut = "俳句を取得できませんでした";
        Serial.printf("[openai] no content in response: %s\n", respBody.c_str());
        return false;
    }
    haikuOut = content;
    Serial.printf("[openai] haiku generated (%u chars)\n", (unsigned)haikuOut.length());
    return true;
}

}  // namespace OpenAI
