/*
  SPDX-License-Identifier: MIT
  Copyright (c) 2026 宮内工務店

  c5_wifi_scope.ino
  ESP32-C5 (M5Stamp C5) 2.4GHz / 5GHz Wi-Fi チャンネル占有スコープ用ファームウェア

  やっていること
   1. プロミスキャスモードでチャンネルを順番に切り替え、各チャンネルの
      受信パケット数・データ量・RSSIを集計して JSON 1行/チャンネルで出力
   2. 数周ごとに通常のAPスキャンを行い、SSID/BSSID/チャンネル/RSSIを出力
   3. ビーコンを解析して、各APのチャンネル幅(20/40/80/160MHz)と中心チャンネルを取得
     (HT Operation / VHT Operation を読む。巡回中に聞こえたAPだけが対象)
  4. PCから届くコマンド(改行区切り)で帯域・滞在時間・AP検出を切り替え

  コマンド(PC→ボード)
    band 2 | band 5 | band a    巡回する帯域
    band n                      巡回しない(APスキャン専用)
    dwell <ms>                  1チャンネルの滞在時間(30〜1000)
    scan 0 | scan 1             AP検出のオン/オフ
    scannow                     すぐAPスキャン
    chans 1,6,36,40             巡回するチャンネルを指定(測定用) / chans all で解除
    mstart / mstop              ビーコンのRSSI集計の開始 / 終了(終了時にAPごとの結果を出力)
    ident                       3秒間LEDを速く点滅(どの台か探す用)
    hello                       現在の設定を返す

  Arduino IDE 設定
    M5Stack 公式の手順に従い、ボードパッケージを入れて「M5StampC5」を選ぶ
    (https://docs.m5stack.com/en/arduino/m5stampc5/program)
    「USB CDC On Boot」の項目がある場合は Enabled
*/

#include <WiFi.h>
#include "esp_wifi.h"

// ---- 役割表示用LED ----
// Stamp-C5 にはRGB LEDがなく、青いLED(G28 / BOOT表示)が1個あるだけです。
// 役割は点滅回数で示します: 2.4GHz=1回 / 5GHz=2回 / 両方=3回 / スキャン専用=点灯したまま
// LEDの極性は未確認です。点滅の向きが逆(消灯が基本で光る↔点灯が基本で消える)なら LED_ON を LOW に。
#define LED_PIN 28
#define LED_ON  HIGH
// 外付けWS2812(NeoPixel)をつないだ場合は、そのピン番号を入れると役割を色で表示します。
// 2.4GHz=緑 / 5GHz=青 / スキャン専用=橙 / 両方=白。使わないなら -1。
#define RGB_PIN -1

// 巡回するチャンネル(日本向け)。設定できないチャンネルは自動でスキップされます。
static const uint8_t CH24[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
static const uint8_t CH5[]  = {36, 40, 44, 48, 52, 56, 60, 64,
                               100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144,
                               149, 153, 157, 161, 165};

// 設定(コマンドで変更可)
static uint8_t  bandMode    = 3;     // bit0: 2.4GHz, bit1: 5GHz
static uint16_t dwellMs     = 120;
static bool     scanEnabled = true;
static uint8_t  scanEvery   = 3;     // 何周に1回APスキャンするか
static bool     scanDue     = false;
static uint32_t sweepCount  = 0;

// ---- ビーコン解析(ESPに依存しない純粋な処理。PC上でも単体テストできる) ----
// ---BEGIN PARSE---
struct ParsedAp {
  uint8_t  bssid[6];
  char     ssid[33];
  uint8_t  ch;      // プライマリチャンネル
  uint16_t w;       // チャンネル幅 MHz (20/40/80/160)
  uint8_t  c;       // 中心チャンネル番号(5MHz刻みの番号)
};

// ビーコン(0x80)/プローブ応答(0x50)から情報を取り出す。対象外ならfalse。
static bool parseBeacon(const uint8_t *d, uint32_t len, uint8_t curCh, ParsedAp &o) {
  if (len < 40 || (d[0] != 0x80 && d[0] != 0x50)) return false;
  uint32_t end = len - 4;                       // 末尾のFCSは読まない
  memcpy(o.bssid, d + 16, 6);
  o.ssid[0] = 0;
  int ds = 0, htPri = 0, sec = 0, htw = 0, vhtw = -1, seg0 = 0, seg1 = 0;
  uint32_t off = 36;                            // 24(ヘッダ) + 12(固定パラメータ)
  while (off + 2 <= end) {
    uint8_t id = d[off], l = d[off + 1];
    if (off + 2 + l > end) break;
    const uint8_t *v = d + off + 2;
    if (id == 0 && l <= 32) {
      if (l > 0 && v[0] != 0) { memcpy(o.ssid, v, l); o.ssid[l] = 0; }   // 先頭が0なら非公開SSID
    } else if (id == 3 && l >= 1)  { ds = v[0]; }
    else if (id == 61 && l >= 2)   { htPri = v[0]; sec = v[1] & 3; htw = (v[1] >> 2) & 1; }
    else if (id == 192 && l >= 3)  { vhtw = v[0]; seg0 = v[1]; seg1 = v[2]; }
    off += 2 + l;
  }
  int primary = htPri ? htPri : (ds ? ds : curCh);
  if (primary <= 0) return false;
  int w = 20, c = primary;
  if (htw && sec == 1)      { w = 40; c = primary + 2; }
  else if (htw && sec == 3) { w = 40; c = primary - 2; }
  if (primary > 14 && vhtw >= 1 && seg0) {      // VHT Operation は5GHzのみ
    bool wide = seg1 && abs(seg1 - seg0) == 8;  // 160MHz(新しい表現)
    if (vhtw == 2 || (vhtw == 1 && wide)) { w = 160; c = wide ? seg1 : seg0; }
    else                                   { w = 80;  c = seg0; }   // 80+80 は主セグメントのみ
  }
  o.ch = (uint8_t)primary; o.w = (uint16_t)w; o.c = (uint8_t)c;
  return true;
}
// ---END PARSE---

// AP表(ビーコンから集めた情報)
#define AP_MAX 64
struct ApEnt {
  bool     used;
  bool     dirty;
  uint8_t  bssid[6];
  char     ssid[33];
  uint8_t  ch;
  int8_t   rssi;
  uint16_t w;
  uint8_t  c;
  uint32_t seen;
  uint32_t emitted;
  int32_t  msum;      // 測定用: RSSIの合計(dBm)
  uint16_t mcnt;      // 測定用: ビーコンを聞いた回数
  int8_t   mmax;      // 測定用: 最大RSSI
};
static ApEnt apTab[AP_MAX];
static portMUX_TYPE apMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint8_t curCh = 0;
static volatile bool gMeasure = false;
static uint8_t customCh[40];
static uint8_t customN = 0;               // 0なら通常の帯域巡回

// 受信カウンタ(コールバックから更新)
static volatile uint32_t gPkts = 0, gBytes = 0;
static volatile int32_t  gRssiSum = 0;
static volatile int8_t   gRssiMax = -128;

static uint32_t identUntil = 0;
static uint32_t lastRgb    = 0xFFFFFFFF;

static char    lineBuf[192];
static uint8_t lineLen = 0;

static void onPacket(void *buf, wifi_promiscuous_pkt_type_t type) {
  const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
  int8_t r = p->rx_ctrl.rssi;
  gPkts++;
  gBytes += p->rx_ctrl.sig_len;
  gRssiSum += r;
  if (r > gRssiMax) gRssiMax = r;

  if (type != WIFI_PKT_MGMT) return;
  ParsedAp a;
  if (!parseBeacon(p->payload, p->rx_ctrl.sig_len, curCh, a)) return;

  portENTER_CRITICAL(&apMux);
  int idx = -1, freeIdx = -1, oldIdx = 0;
  uint32_t oldest = 0xFFFFFFFF;
  for (int i = 0; i < AP_MAX; i++) {
    if (apTab[i].used && !memcmp(apTab[i].bssid, a.bssid, 6)) { idx = i; break; }
    if (!apTab[i].used) { if (freeIdx < 0) freeIdx = i; }
    else if (apTab[i].seen < oldest) { oldest = apTab[i].seen; oldIdx = i; }
  }
  bool fresh = idx < 0;
  if (fresh) idx = (freeIdx >= 0) ? freeIdx : oldIdx;
  ApEnt &e = apTab[idx];
  if (fresh) { memset(&e, 0, sizeof(e)); e.used = true; memcpy(e.bssid, a.bssid, 6); }
  if (fresh || e.ch != a.ch || e.w != a.w || e.c != a.c) e.dirty = true;
  if (a.ssid[0] && strcmp(e.ssid, a.ssid)) { strcpy(e.ssid, a.ssid); e.dirty = true; }
  e.ch = a.ch; e.w = a.w; e.c = a.c;
  if (fresh || curCh == a.ch) e.rssi = r;       // 隣のチャンネルで聞こえた分はRSSIに使わない
  e.seen = millis();
  if (gMeasure && curCh == a.ch) {              // 測定中: そのAPのチャンネルで聞こえた分だけ数える
    e.msum += r; e.mcnt++;
    if (e.mcnt == 1 || r > e.mmax) e.mmax = r;
  }
  portEXIT_CRITICAL(&apMux);
}

static void printJsonStr(const String &s) {
  Serial.print('"');
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = (uint8_t)s[i];
    if (c == '"' || c == '\\') { Serial.print('\\'); Serial.write(c); }
    else if (c < 0x20)         { Serial.print(' '); }
    else                       { Serial.write(c); }
  }
  Serial.print('"');
}

// 役割番号: 1=2.4GHz 2=5GHz 3=スキャン専用 4=両方
static uint8_t roleNum() {
  return bandMode == 1 ? 1 : bandMode == 2 ? 2 : bandMode == 0 ? 3 : 4;
}

static void ledTick() {
  uint32_t now = millis();
  bool ident = now < identUntil;
  uint8_t role = roleNum();

  if (LED_PIN >= 0) {
    static const uint8_t blinks[5] = {0, 1, 2, 0, 3};
    bool on;
    if (ident)          on = ((now / 100) % 2) == 0;
    else if (role == 3) on = true;
    else { uint32_t t = now % 2400; on = (t < blinks[role] * 300UL) && ((t % 300) < 150); }
    digitalWrite(LED_PIN, on ? LED_ON : !LED_ON);
  }
  if (RGB_PIN >= 0) {
    static const uint8_t col[5][3] = {{0,0,0}, {0,40,24}, {8,20,90}, {70,40,0}, {40,40,40}};
    bool on = ident ? (((now / 100) % 2) == 0) : true;
    uint32_t v = on ? ((uint32_t)col[role][0] << 16 | col[role][1] << 8 | col[role][2]) : 0;
    if (v != lastRgb) { lastRgb = v; neopixelWrite(RGB_PIN, v >> 16, (v >> 8) & 0xFF, v & 0xFF); }
  }
}

static void hello() {
  uint16_t id = (uint16_t)(ESP.getEfuseMac() >> 32);
  const char *b = (bandMode == 0) ? "n" : (bandMode == 1) ? "2" : (bandMode == 2) ? "5" : "a";
  Serial.printf("{\"t\":\"hello\",\"id\":\"%04X\",\"band\":\"%s\",\"dwell\":%u,\"scan\":%d,\"chans\":%u}\n",
                id, b, (unsigned)dwellMs, scanEnabled ? 1 : 0, (unsigned)customN);
}

static void emitMeasure() {
  for (int i = 0; i < AP_MAX; i++) {
    ApEnt e; bool out = false;
    portENTER_CRITICAL(&apMux);
    if (apTab[i].used && apTab[i].mcnt) { e = apTab[i]; out = true; }
    portEXIT_CRITICAL(&apMux);
    if (!out) continue;
    Serial.print("{\"t\":\"m\",\"ssid\":");
    printJsonStr(String(e.ssid));
    Serial.printf(",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ch\":%u,\"w\":%u,\"n\":%u,\"avg\":%.1f,\"max\":%d}\n",
                  e.bssid[0], e.bssid[1], e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5],
                  (unsigned)e.ch, (unsigned)e.w, (unsigned)e.mcnt, (float)e.msum / e.mcnt, (int)e.mmax);
  }
  Serial.print("{\"t\":\"mdone\"}\n");
}

static void runCmd(const char *s) {
  if (!strncmp(s, "band ", 5)) {
    char v = s[5];
    bandMode = (v == 'n') ? 0 : (v == '2') ? 1 : (v == '5') ? 2 : 3;
    if (bandMode == 0) scanEnabled = true;
  } else if (!strncmp(s, "dwell ", 6)) {
    dwellMs = constrain(atoi(s + 6), 30, 1000);
  } else if (!strncmp(s, "scan ", 5)) {
    scanEnabled = atoi(s + 5) != 0;
  } else if (!strcmp(s, "scannow")) {
    scanDue = true;
  } else if (!strcmp(s, "ident")) {
    identUntil = millis() + 3000;
  } else if (!strncmp(s, "chans ", 6)) {
    customN = 0;
    if (strncmp(s + 6, "all", 3)) {             // "1,6,36" 形式
      const char *p = s + 6;
      while (*p && customN < sizeof(customCh)) {
        int v = atoi(p);
        if (v >= 1 && v <= 177) customCh[customN++] = (uint8_t)v;
        while (*p && *p != ',') p++;
        if (*p == ',') p++;
      }
    }
  } else if (!strcmp(s, "mstart")) {
    portENTER_CRITICAL(&apMux);
    for (int i = 0; i < AP_MAX; i++) { apTab[i].msum = 0; apTab[i].mcnt = 0; apTab[i].mmax = -128; }
    gMeasure = true;
    portEXIT_CRITICAL(&apMux);
  } else if (!strcmp(s, "mstop")) {
    gMeasure = false;
    emitMeasure();
    return;                                     // 結果のあとにhelloは付けない
  }
  hello();
}

static void handleSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (lineLen) { lineBuf[lineLen] = 0; runCmd(lineBuf); lineLen = 0; }
    } else if (lineLen < sizeof(lineBuf) - 1) {
      lineBuf[lineLen++] = c;
    }
  }
}

static void waitMs(uint32_t ms) {
  uint32_t t0 = millis();
  while (millis() - t0 < ms) { handleSerial(); ledTick(); delay(5); }
}

static void emitAps() {
  uint32_t now = millis();
  for (int i = 0; i < AP_MAX; i++) {
    ApEnt e; bool out = false;
    portENTER_CRITICAL(&apMux);
    if (apTab[i].used && now - apTab[i].seen < 60000 &&
        (apTab[i].dirty || (apTab[i].seen > apTab[i].emitted && now - apTab[i].emitted > 5000))) {
      e = apTab[i]; out = true;
      apTab[i].emitted = now; apTab[i].dirty = false;
    }
    portEXIT_CRITICAL(&apMux);
    if (!out) continue;
    Serial.print("{\"t\":\"ap\",\"ssid\":");
    printJsonStr(String(e.ssid));
    Serial.printf(",\"bssid\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"ch\":%u,\"rssi\":%d,\"w\":%u,\"c\":%u}\n",
                  e.bssid[0], e.bssid[1], e.bssid[2], e.bssid[3], e.bssid[4], e.bssid[5],
                  (unsigned)e.ch, (int)e.rssi, (unsigned)e.w, (unsigned)e.c);
  }
}

static void sweep(const uint8_t *list, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (esp_wifi_set_channel(list[i], WIFI_SECOND_CHAN_NONE) != ESP_OK) continue;
    gPkts = 0; gBytes = 0; gRssiSum = 0; gRssiMax = -128;
    curCh = list[i];
    uint32_t t0 = millis();
    waitMs(dwellMs);
    uint32_t ms = millis() - t0;

    uint32_t pk = gPkts, by = gBytes;
    int32_t  sum = gRssiSum;
    int      mx  = gRssiMax;
    int      avg = pk ? (int)(sum / (int32_t)pk) : 0;
    if (!pk) mx = 0;

    Serial.printf("{\"t\":\"ch\",\"ch\":%u,\"n\":%lu,\"b\":%lu,\"avg\":%d,\"max\":%d,\"ms\":%lu}\n",
                  list[i], (unsigned long)pk, (unsigned long)by, avg, mx, (unsigned long)ms);
    emitAps();
  }
}

static void doScan() {
  ledTick();
  esp_wifi_set_promiscuous(false);
  int n = WiFi.scanNetworks(false /*async*/, true /*hidden*/, false /*passive*/, 120 /*ms/ch*/);
  for (int i = 0; i < n; i++) {
    Serial.print("{\"t\":\"ap\",\"ssid\":");
    printJsonStr(WiFi.SSID(i));
    Serial.printf(",\"bssid\":\"%s\",\"ch\":%d,\"rssi\":%d,\"auth\":%d}\n",
                  WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i),
                  (int)WiFi.encryptionType(i));
  }
  WiFi.scanDelete();
  esp_wifi_set_promiscuous(true);
}

void setup() {
  Serial.begin(115200);
  if (LED_PIN >= 0) pinMode(LED_PIN, OUTPUT);
  delay(300);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_country_code("JP", true);   // 5GHzの使用可能チャンネルを日本の規制に合わせる

  wifi_promiscuous_filter_t filt = {};
  filt.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL;
  esp_wifi_set_promiscuous_filter(&filt);
  esp_wifi_set_promiscuous_rx_cb(&onPacket);
  esp_wifi_set_promiscuous(true);

  hello();
}

void loop() {
  handleSerial();

  if (customN) {                       // 測定用: 指定チャンネルだけを巡回
    sweep(customCh, customN);
    sweepCount++;
    if (scanDue || (scanEnabled && sweepCount % scanEvery == 0)) { scanDue = false; doScan(); }
    return;
  }

  if (bandMode == 0) {                 // APスキャン専用: 連続でスキャンする
    if (scanEnabled) doScan(); else waitMs(100);
    return;
  }

  if (bandMode & 1) sweep(CH24, sizeof(CH24));
  if (bandMode & 2) sweep(CH5, sizeof(CH5));
  sweepCount++;

  if (scanDue || (scanEnabled && sweepCount % scanEvery == 0)) {
    scanDue = false;
    doScan();
  }
}
