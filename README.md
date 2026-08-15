# m5web

M5Stack **ATOM Printer Kit**（ATOM Lite + 58mm サーマルプリンター）用のファームウェア。
Wi-Fiに接続し、`m5web` というWebページをホストする。iPhoneなどからそのページを開いて
写真をアップロードすると、プリンター用の白黒ビットマップに変換してサーマルプリンターから
印刷する。テキストをそのまま印刷する機能もある。

## ハードウェア

ATOM Printer Kit の標準配線（[公式ドキュメント](https://docs.m5stack.com/en/atom/atom_printer)）:

| ATOM Lite | プリンター |
|---|---|
| G23 (TX) | RX |
| G33 (RX) | TX |
| 5V/GND   | 電源 |

プリンター本体には別途 **DC12V 2.5A以上** のACアダプタが必要（USB給電のATOM Liteとは別系統）。
UARTは 9600bps 8N1 固定。

対応機種は ATOM Lite（`m5stack-atom`, ESP32-PICO-D4）を前提にしている。ATOMS3などピン配置が
異なる機種を使う場合は `src/printer.cpp` の `kRxPin`/`kTxPin` と `platformio.ini` の `board` を
変更すること。

## 書き込み手順

PlatformIO / Arduino IDE のどちらでも書き込める。中身は同じファームウェア。

### PlatformIO（推奨）

VSCode + PlatformIO拡張、または `pio` CLI を使う。

```bash
pio run -t upload      # ファームウェア書き込み
pio run -t uploadfs    # data/index.html (Web UI) を書き込み — 初回・更新時は必ず実行
pio device monitor      # シリアルログ確認（115200bps）
```

`uploadfs` を忘れると `/` にアクセスした際 "index.html missing" と表示される。

### Arduino IDE

スケッチは [`arduino/m5web/m5web.ino`](arduino/m5web/m5web.ino)（`src/`・`data/` と同内容のフラット構成コピー。
Arduino IDEはスケッチフォルダ直下にファイルを置く必要があるため、PlatformIO用の`src/`とは別に用意している。
片方を直したらもう片方にも反映すること）。

1. ボードマネージャURLに `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
   を追加し、"esp32 by Espressif Systems" をインストール。
2. ボードに **M5Atom** を選択。
3. `ツール > Partition Scheme` はSPIFFS/LittleFS領域のあるもの（例: "Default 4MB with spiffs"）を選択。
4. `arduino/m5web/m5web.ino` を開いて スケッチ > マイコンボードに書き込む。
5. `data/index.html` はLittleFSへの書き込みが別途必要。Arduino IDE 2.xの場合は
   [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) プラグインを
   インストールし、コマンドパレットから "Upload LittleFS to Pico/ESP8266/ESP32" を実行する
   （IDE 1.8系なら同等の "ESP32 Sketch Data Upload" ツールを使う）。

## 初回セットアップ（Wi-Fi）

1. 書き込み直後はWi-Fi設定が無いため、本機が `m5web-setup-XXXX` という名前のオープンAPを
   立ち上げる。iPhoneのWi-Fi設定からこのAPに接続する。
2. ブラウザで `http://192.168.4.1` を開く（キャプティブポータルとして自動的に開くこともある）。
3. 表示された「Wi-Fi設定」カードから接続したいネットワークを選び、パスワードを入力して
   「接続する」を押す。
4. 接続に成功すると本機のAPは終了し、指定したWi-Fiにクライアントとして参加する。iPhoneのWi-Fi
   を元のネットワークに戻し、`http://m5web.local` を開く（mDNS対応）。IPアドレスは
   `pio device monitor` のログでも確認できる。

Wi-Fi設定をやり直したい場合は、ATOM本体のボタンを5秒以上長押しすると保存済みの設定を消去して
再起動し、再びAPモードに入る。

## 使い方

`http://m5web.local`（またはIPアドレス）を開くと以下ができる。

- **画像を印刷**: 写真を選択すると自動的に印刷幅（384dot=58mm幅, 203dpi）に合わせてリサイズし、
  誤差拡散（Floyd–Steinberg）・網掛け・単純2値化のいずれかで白黒ビットマップに変換する。
  明るさ・コントラスト・反転・90°回転にも対応。プレビューを確認してから「この画像を印刷」を押す。
  変換はすべてブラウザ側（Canvas）で行われるため、ATOM側の負荷やメモリ消費は最小限。
- **Put TEXT for print**: テキストエリアに入力した文章をそのまま印刷する（動作確認・簡易メモ用）。
- **QRコードを印刷**: URLなどの文字列をQRコードにして印刷する。QR自体の生成はブラウザ側ではなく
  プリンター内蔵の2Dシンボル生成機能（GS ( k コマンド）で行う。

## 制限事項

- 印刷幅は58mmヘッド固定の384dot。
- 1回の印刷の高さは既定で最大2000dot（約250mm）。それを超える画像は下部が切り詰められる
  （`src/web_server.cpp` の `kMaxHeightDots` で変更可）。
- 9600bps固定のため、印刷サイズが大きいほど転送に時間がかかる（384×2000dotで概算50秒程度）。
- Wi-Fi設定用APはオープンネットワーク（パスワード無し）。設置環境に応じて
  `src/wifi_manager.cpp` の `WiFi.softAP(apName)` にパスワード引数を足すこと。

## 構成

```
platformio.ini
src/
  main.cpp           setup/loop の配線
  wifi_manager.*      Wi-Fi(STA)接続 / APフォールバック+キャプティブポータル / mDNS
  printer.*           UART経由でのESC/POS風ラスター送信・テキスト送信（GS v 0）
  web_server.*        m5webのHTTP API (状態取得, Wi-Fi設定, 画像/テキスト印刷)
data/
  index.html          m5web本体（UI + Canvas画像変換, 外部CDN依存なし・単一ファイル）
arduino/m5web/
  m5web.ino + 同名の*.h/*.cpp/data/  Arduino IDE用の手動同期コピー（上記と同内容）
```

## API

Web UIが使っているものと同じHTTP APIを、プログラムから直接叩ける。認証は無い（同一LAN内での
利用を前提）ので、`curl`やPythonの`requests`などから直接キックできる。ベースURLは
`http://m5web.local`（またはIPアドレス）。

| メソッド | パス | 内容 |
|---|---|---|
| GET | `/api/status` | 接続状態のJSON (`mode`,`connected`,`ip`,`ssid`,`printWidthDots`,`maxHeightDots`) |
| GET | `/api/wifi/scan` | 近隣Wi-FiのJSON配列 |
| POST | `/api/wifi` | `ssid`,`password` (form) でWi-Fi接続 |
| POST | `/api/print/text` | `text` (form) をそのまま印刷 |
| POST | `/api/print/qr` | `url` (form) をQRコードにして印刷（変換はプリンター内蔵機能） |
| POST | `/api/print/test` | 配線確認用の固定テストページを印刷（UI上のボタンは無いが引き続き利用可） |
| POST | `/api/print/image/begin` | `w`,`h` (form) で次の画像サイズを予約 (wは384固定, h<=`maxHeightDots`) |
| POST | `/api/print/image` | multipart/form-dataで1bpp生ビットマップ本体をアップロード→即印刷 |

### curl例

```bash
HOST=http://m5web.local

# テキスト印刷
curl -X POST "$HOST/api/print/text" --data-urlencode "text=Hello from a script"

# QRコード印刷
curl -X POST "$HOST/api/print/qr" --data-urlencode "url=https://example.com"

# テストページ
curl -X POST "$HOST/api/print/test"

# 状態確認
curl "$HOST/api/status"
```

### 画像印刷（プログラムから）

`/api/print/image`が受け取るのはPNG/JPEGではなく、**幅384dot固定・1bpp・MSB-first・1=黒**に
パックした生バイナリ（Web UIがブラウザのCanvasで作っているものと同じ形式）。Pythonから叩く例
（Pillowで384px幅にリサイズ→2値化→パック）:

```python
import requests
from PIL import Image

HOST = "http://m5web.local"
PRINT_WIDTH = 384

img = Image.open("photo.jpg").convert("L")
h = round(PRINT_WIDTH * img.height / img.width)
img = img.resize((PRINT_WIDTH, h)).convert("1")  # Pillowの標準ディザリングで2値化

packed = bytearray((PRINT_WIDTH // 8) * h)
px = img.load()
for y in range(h):
    for x in range(PRINT_WIDTH):
        if px[x, y] == 0:  # 0=黒
            packed[y * (PRINT_WIDTH // 8) + x // 8] |= 0x80 >> (x % 8)

requests.post(f"{HOST}/api/print/image/begin", data={"w": PRINT_WIDTH, "h": h}).raise_for_status()
requests.post(f"{HOST}/api/print/image", files={"file": ("image.bin", bytes(packed), "application/octet-stream")}).raise_for_status()
```
