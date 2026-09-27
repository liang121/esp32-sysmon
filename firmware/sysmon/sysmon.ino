/*
 * sysmon — Mac load monitor for Waveshare ESP32-S3-Touch-LCD-4.3C.
 * Polls http://<host>:<port>/api/now once per second and draws it.
 * Config lives in NVS; set it over USB serial (see mac/configure.mjs):
 *   set ssid|pass|ssid2|pass2 <value> | set host <host or ip> | set port <n> | status | reboot
 * Touch anywhere: cycle backlight brightness.
 */
#include <WiFi.h>
#include <HTTPClient.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include "src/rgb_lcd_port/rgb_lcd_port.h"
#include "src/rgb_lcd_port/gui_paint/gui_paint.h"
#include "src/gt911/gt911.h"

#define W EXAMPLE_LCD_H_RES
#define H EXAMPLE_LCD_V_RES

// RGB565 palette
#define C_BG      0x0861
#define C_PANEL   0x10C3
#define C_GRID    0x2146
#define C_TEXT    0xE73C
#define C_DIM     0x8C71
#define C_BLUE    0x3C1F
#define C_BLUE_F  0x1149
#define C_PURPLE  0xA2BE
#define C_PURP_F  0x3009
#define C_GREEN   0x1B88
#define C_AMBER   0x8B60
#define C_RED     0xA125
#define C_GRAY    0x3186

Preferences prefs;
String cfgSsid, cfgPass, cfgSsid2, cfgPass2, cfgHost;
String nets[2][2];      // {ssid, pass}
int nNets = 0, netIdx = 0;
uint32_t connStartMs = 0;
uint16_t cfgPort = 8787;
IPAddress hostIp;

uint16_t *bufs[2];
int drawIdx = 0;
uint16_t *fb;

// ---- latest data ----
struct Proc { char name[25]; int cpu; int rss; };
struct Stats {
  bool valid = false;
  int cpu = 0, mp = 1, mfree = 0, cores[16] = {0}, ncores = 0;
  float load = 0, mused = 0, mtot = 0, swap = 0, swapr = 0;
  Proc top[5]; int ntop = 0;
  uint8_t hcpu[120]; uint16_t hsw[120]; int nh = 0;
} st;
uint32_t lastOkMs = 0;
String lastErr = "starting";

const uint8_t BRIGHT[] = {0, 40, 75};  // PWM duty: 0 = full brightness (inverted)
int brightIdx = 0;

// ---------- drawing helpers (direct framebuffer) ----------
static inline void px(int x, int y, uint16_t c) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c; }
void fillRect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x; if (y + h > H) h = H - y;
  for (int j = 0; j < h; j++) { uint16_t *p = fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = c; }
}
void vline(int x, int y0, int y1, uint16_t c) { if (y0 > y1) { int t = y0; y0 = y1; y1 = t; } for (int y = y0; y <= y1; y++) px(x, y, c); }
void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { px(x0, y0, c); px(x0, y0 + 1, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
// Text: glyph tables live in internal SRAM (see fontsToSram) and only foreground pixels are written.
// Flash and PSRAM share one bus on the S3; reading big font tables from flash while writing the
// framebuffer starved the LCD bounce-buffer refill and made large text glitch. bg is unused because
// every region is filled before text is drawn on it.
void text(int x, int y, const char *s, sFONT *f, uint16_t fg, uint16_t bg) {
  const int bpr = (f->Width + 7) / 8;
  for (; *s; s++, x += f->Width) {
    uint8_t c = (uint8_t)*s;
    if (c < 32 || c > 126) c = '?';
    if (x + f->Width > W) break;
    const uint8_t *g = f->table + (c - 32) * f->Height * bpr;
    for (int r = 0; r < f->Height; r++, g += bpr) {
      int yy = y + r; if (yy < 0 || yy >= H) continue;
      uint16_t *row = fb + yy * W + x;
      for (int col = 0; col < f->Width; col++)
        if (g[col >> 3] & (0x80 >> (col & 7))) row[col] = fg;
    }
  }
}
void fontsToSram() {
  sFONT *fonts[] = {&Font12, &Font16, &Font24, &Font48};
  for (sFONT *f : fonts) {
    size_t n = 95 * f->Height * ((f->Width + 7) / 8);
    uint8_t *p = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) { memcpy(p, f->table, n); f->table = p; }
  }
}

// area chart of 0..100 values, newest on the right; y-axis labels (top/mid/bottom) in a left gutter
#define GUTTER 40
void chart(int x, int y, int w, int h, const uint8_t *v, int n, uint16_t stroke, uint16_t fill,
           const char *lTop, const char *lMid, const char *lBot) {
  text(x + GUTTER - 6 - 7 * strlen(lTop), y, lTop, &Font12, C_DIM, C_BG);
  text(x + GUTTER - 6 - 7 * strlen(lMid), y + h / 2 - 6, lMid, &Font12, C_DIM, C_BG);
  text(x + GUTTER - 6 - 7 * strlen(lBot), y + h - 12, lBot, &Font12, C_DIM, C_BG);
  x += GUTTER; w -= GUTTER;
  fillRect(x, y, w, h, C_PANEL);
  for (int k = 1; k < 4; k++) for (int i = x; i < x + w; i += 4) px(i, y + h * k / 4, C_GRID);
  if (n < 2) return;
  float dx = (float)(w - 1) / 119.0f;
  int prevX = -1, prevY = 0;
  for (int i = 0; i < n; i++) {
    int xi = x + w - 1 - (int)((n - 1 - i) * dx);
    int yi = y + h - 1 - (int)(v[i] * (h - 2) / 100);
    if (prevX >= 0) {
      for (int xx = prevX; xx <= xi; xx++) {  // fill under the segment
        int yy = prevY + (yi - prevY) * (xx - prevX) / max(1, xi - prevX);
        vline(xx, yy, y + h - 1, fill);
      }
      line(prevX, prevY, xi, yi, stroke);
    }
    prevX = xi; prevY = yi;
  }
}

// ---------- render ----------
void render() {
  fb = bufs[drawIdx];
  Paint_SelectImage((uint8_t *)fb);
  // No full-screen clear: each region paints its own background and only the gaps are filled.
  // Rendering writes to PSRAM, which the LCD bounce-buffer ISR also reads every ~0.5 ms;
  // big bursts starve it and the panel glitches. Short yields between regions let it catch up.
  fillRect(0, 56, W, 8, C_BG);            // under banner
  fillRect(250, 64, 12, H - 64, C_BG);    // column gap
  fillRect(262, 64, W - 262, 20, C_BG);   // CPU title row
  fillRect(262, 296, W - 262, 26, C_BG);  // between core bars and swap chart
  fillRect(W - 12, 64, 12, H - 64, C_BG); // right margin
  fillRect(262, 84, GUTTER, 170, C_BG);   // CPU chart y-axis gutter
  fillRect(262, 322, GUTTER, 146, C_BG);  // swap chart y-axis gutter
  fillRect(262, 468, W - 262, 12, C_BG);  // bottom margin
  fillRect(262, 254, W - 262, 8, C_BG);   // between CPU chart and core bars
  char s[64];
  uint32_t age = st.valid ? (millis() - lastOkMs) / 1000 : 0;
  bool stale = !st.valid || age >= 5;

  // banner: memory pressure, or connection problem
  uint16_t bc; const char *bt;
  if (!st.valid)      { bc = C_GRAY;  bt = "WAITING FOR MAC"; }
  else if (st.mp >= 4){ bc = C_RED;   bt = "MEMORY CRITICAL - STOP"; }
  else if (st.mp >= 2){ bc = C_AMBER; bt = "MEMORY WARNING"; }
  else                { bc = C_GREEN; bt = "MEMORY OK"; }
  fillRect(0, 0, W, 56, bc);
  text(16, 16, bt, &Font24, C_TEXT, bc);
  if (stale && st.valid) { snprintf(s, sizeof s, "MAC SILENT %lus", (unsigned long)age); fillRect(W - 300, 8, 292, 40, C_RED); text(W - 290, 16, s, &Font24, C_TEXT, C_RED); }
  else if (!st.valid)    { text(W - 16 - 11 * min((int)lastErr.length(), 24), 20, lastErr.substring(0, 24).c_str(), &Font16, C_TEXT, bc); }

  vTaskDelay(1);
  // left column
  fillRect(0, 64, 250, H - 64, C_BG);
  text(16, 72, "CPU", &Font16, C_DIM, C_BG);
  snprintf(s, sizeof s, "%d%%", st.cpu); text(16, 92, s, &Font48, st.cpu >= 85 ? 0xFB00 : C_TEXT, C_BG);
  snprintf(s, sizeof s, "load %.2f", st.load); text(16, 144, s, &Font16, C_DIM, C_BG);

  text(16, 176, "MEMORY", &Font16, C_DIM, C_BG);
  snprintf(s, sizeof s, "%.1f/%.0fG", st.mused, st.mtot); text(16, 196, s, &Font24, C_TEXT, C_BG);
  snprintf(s, sizeof s, "swap %.1fG used", st.swap); text(16, 226, s, &Font16, C_DIM, C_BG);

  text(16, 262, "TOP", &Font16, C_DIM, C_BG);
  for (int i = 0; i < st.ntop; i++) {
    char nm[15]; strncpy(nm, st.top[i].name, 14); nm[14] = 0;
    text(16, 284 + i * 36, nm, &Font16, C_TEXT, C_BG);
    snprintf(s, sizeof s, "%d%% %dM", st.top[i].cpu, st.top[i].rss);
    text(16, 302 + i * 36, s, &Font12, C_DIM, C_BG);
  }

  vTaskDelay(1);
  // right: CPU chart, per-core bars, swap chart
  const int X = 262, CW = W - X - 12;
  text(X, 68, "CPU  last 2 min", &Font12, C_DIM, C_BG);
  chart(X, 84, CW, 170, st.hcpu, st.nh, C_BLUE, C_BLUE_F, "100%", "50%", "0%");
  fillRect(X, 262, CW, 34, C_BG);  // core-bar row (gaps between bars)
  if (st.ncores) {
    int bw = (CW - GUTTER - (st.ncores - 1) * 6) / st.ncores;
    for (int i = 0; i < st.ncores; i++) {
      int bx = X + GUTTER + i * (bw + 6), bh = 34, fh = st.cores[i] * bh / 100;
      fillRect(bx, 262, bw, bh, C_PANEL);
      fillRect(bx, 262 + bh - fh, bw, fh, st.cores[i] >= 90 ? 0xFB00 : C_BLUE);
    }
  }
  // swap I/O: auto-scaled to a round ceiling so idle stays flat and bursts stay readable
  int peak = 0; for (int i = 0; i < st.nh; i++) peak = max(peak, (int)st.hsw[i]);
  static const int CEIL[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
  int top = CEIL[8]; for (int c : CEIL) if (peak <= c) { top = c; break; }
  uint8_t pct[120]; for (int i = 0; i < st.nh; i++) pct[i] = min(100, st.hsw[i] * 100 / top);
  snprintf(s, sizeof s, "SWAP I/O  now %.0f MB/s  (0 = healthy)", st.swapr);
  vTaskDelay(1);
  text(X, 306, s, &Font12, st.swapr >= 20 ? 0xFB00 : C_DIM, C_BG);
  char sTop[8], sMid[8];
  snprintf(sTop, sizeof sTop, "%d", top); snprintf(sMid, sizeof sMid, "%d", top / 2);
  chart(X, 322, CW, 146, pct, st.nh, st.swapr >= 20 ? 0xFB00 : C_PURPLE, C_PURP_F, sTop, sMid, "0");

  if (stale) for (int y = 64; y < H; y += 3) for (int x = (y / 3) % 2; x < W; x += 6) px(x, y, C_BG);  // dim stale data

  waveshare_rgb_lcd_display((uint8_t *)fb);
  drawIdx ^= 1;
}

// ---------- config ----------
void loadCfg() {
  prefs.begin("sysmon", true);
  cfgSsid = prefs.getString("ssid", ""); cfgPass = prefs.getString("pass", "");
  cfgSsid2 = prefs.getString("ssid2", ""); cfgPass2 = prefs.getString("pass2", "");
  netIdx = prefs.getUChar("lastnet", 0);
  cfgHost = prefs.getString("host", ""); cfgPort = prefs.getUShort("port", 8787);
  prefs.end();
}
void handleSerial() {
  static String line;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c != '\n') { if (line.length() < 200) line += c; continue; }
    String l = line; line = "";
    if (l.startsWith("set ")) {
      int sp = l.indexOf(' ', 4); if (sp < 0) { Serial.println("ERR usage: set <key> <value>"); continue; }
      String k = l.substring(4, sp), v = l.substring(sp + 1);
      prefs.begin("sysmon", false);
      if (k == "port") prefs.putUShort("port", v.toInt()); else if (k == "ssid" || k == "pass" || k == "ssid2" || k == "pass2" || k == "host") prefs.putString(k.c_str(), v);
      else { prefs.end(); Serial.println("ERR unknown key"); continue; }
      prefs.end(); Serial.printf("OK %s\n", k.c_str());
    } else if (l == "status") {
      Serial.printf("ssid=%s ssid2=%s connected=%s host=%s port=%u wifi=%d ip=%s hostIp=%s last=%s\n", cfgSsid.c_str(), cfgSsid2.c_str(), WiFi.SSID().c_str(), cfgHost.c_str(), cfgPort,
                    WiFi.status() == WL_CONNECTED, WiFi.localIP().toString().c_str(), hostIp.toString().c_str(), lastErr.c_str());
    } else if (l == "reboot") { Serial.println("OK rebooting"); delay(100); ESP.restart(); }
  }
}

// ---------- network ----------
void connectNet(int i) {
  netIdx = i; connStartMs = millis(); hostIp = IPAddress(0, 0, 0, 0);
  WiFi.disconnect();
  WiFi.begin(nets[i][0].c_str(), nets[i][1].c_str());
  lastErr = "joining " + nets[i][0];
}
bool resolveHost() {
  if (hostIp.fromString(cfgHost)) return true;
  String h = cfgHost; if (h.endsWith(".local")) h = h.substring(0, h.length() - 6);
  IPAddress ip = MDNS.queryHost(h, 1500);
  if (ip == IPAddress(0, 0, 0, 0)) return false;
  hostIp = ip; return true;
}

JsonDocument doc;
bool fetch() {
  if (WiFi.status() != WL_CONNECTED) { lastErr = "wifi connecting"; return false; }
  if (hostIp == IPAddress(0, 0, 0, 0) && !resolveHost()) { lastErr = "mac not found"; return false; }
  HTTPClient http;
  http.setConnectTimeout(800); http.setTimeout(900);
  http.begin(String("http://") + hostIp.toString() + ":" + cfgPort + "/api/now");
  int code = http.GET();
  if (code != 200) { lastErr = "http " + String(code); http.end(); if (code < 0) hostIp = IPAddress(0, 0, 0, 0); return false; }
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) { lastErr = "json err"; return false; }
  st.cpu = doc["cpu"]; st.mp = doc["mp"]; st.mfree = doc["mfree"];
  st.load = doc["load"]; st.mused = doc["mused"]; st.mtot = doc["mtot"]; st.swap = doc["swap"]; st.swapr = doc["swapr"];
  JsonArray cores = doc["cores"]; st.ncores = 0; for (int v : cores) if (st.ncores < 16) st.cores[st.ncores++] = v;
  JsonArray top = doc["top"]; st.ntop = 0;
  for (JsonArray p : top) { if (st.ntop >= 5) break; strlcpy(st.top[st.ntop].name, p[0] | "?", 25); st.top[st.ntop].cpu = p[1]; st.top[st.ntop].rss = p[2]; st.ntop++; }
  JsonArray hc = doc["hcpu"], hs = doc["hsw"]; st.nh = 0;
  for (size_t i = 0; i < hc.size() && i < 120; i++) { st.hcpu[i] = constrain((int)hc[i], 0, 100); st.hsw[i] = constrain((int)(hs[i] | 0), 0, 60000); st.nh++; }
  st.valid = true; lastOkMs = millis(); lastErr = "ok";
  return true;
}

void setup() {
  Serial.begin(115200);
  fontsToSram();
  DEV_I2C_Init();
  IO_EXTENSION_Init();
  touch_gt911_init(DEV_I2C_Get_Bus_Device());
  waveshare_esp32_s3_rgb_lcd_init();
  waveshare_rgb_lcd_bl_on();
  IO_EXTENSION_Pwm_Output(BRIGHT[brightIdx]);
  void *b1, *b2; waveshare_get_frame_buffer(&b1, &b2);
  bufs[0] = (uint16_t *)b1; bufs[1] = (uint16_t *)b2;
  Paint_NewImage((uint8_t *)bufs[0], W, H, 0, C_BG);
  Paint_SetScale(65);

  loadCfg();
  if (cfgSsid.length()) {
    WiFi.persistent(false);  // don't write NVS on every (re)connect: flash writes stall the LCD ISR
    WiFi.mode(WIFI_STA); WiFi.setHostname("sysmon"); WiFi.setSleep(false);
    nets[0][0] = cfgSsid; nets[0][1] = cfgPass; nNets = 1;
    if (cfgSsid2.length()) { nets[1][0] = cfgSsid2; nets[1][1] = cfgPass2; nNets = 2; }
    connectNet(netIdx % nNets);
    MDNS.begin("sysmon");
    lastErr = "wifi connecting";
  } else lastErr = "no wifi config";
  render();
}

void loop() {
  static uint32_t nextFetch = 0;
  static bool wasTouched = false;
  handleSerial();

  touch_gt911_point_t tp = touch_gt911_read_point(1);
  bool touched = tp.cnt > 0;
  if (touched && !wasTouched) { brightIdx = (brightIdx + 1) % sizeof(BRIGHT); IO_EXTENSION_Pwm_Output(BRIGHT[brightIdx]); }
  wasTouched = touched;

  if ((int32_t)(millis() - nextFetch) >= 0) {
    nextFetch = millis() + 1000;
    if (nNets) {
      // A network can be joinable yet unable to reach the Mac (guest / isolated SSID):
      // fall over to the other one if we can't connect in 15 s or get no data for 30 s.
      uint32_t since = millis() - max(lastOkMs, connStartMs);
      bool up = WiFi.status() == WL_CONNECTED;
      if ((!up && millis() - connStartMs > 15000) || (up && nNets > 1 && since > 30000)) connectNet((netIdx + 1) % nNets);
      static int savedNet = -1;
      if (fetch() && savedNet != netIdx) { prefs.begin("sysmon", false); prefs.putUChar("lastnet", netIdx); prefs.end(); savedNet = netIdx; }
    }
    render();
  }
  delay(20);
}
