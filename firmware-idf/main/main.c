/*
 * sysmon — Mac load monitor for Waveshare ESP32-S3-Touch-LCD-4.3C (ESP-IDF build).
 * Polls http://<host>:<port>/api/now once per second and draws it.
 * Config lives in NVS; set it over USB serial (see mac/configure.mjs):
 *   set ssid|pass|ssid2|pass2|host|port <value> | status | reboot
 * Swipe left/right: switch page (system load / AI usage). Tap: cycle backlight brightness.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "mdns.h"
#include "cJSON.h"
#include "lwip/inet.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

#include "rgb_lcd_port.h"
#include "gui_paint.h"
#include "gt911.h"
#include "io_extension.h"

#define W EXAMPLE_LCD_H_RES
#define H EXAMPLE_LCD_V_RES
#define GUTTER 40

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
#define C_HOT     0xFB00

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------- config ----------------
static char cfg_ssid[2][33], cfg_pass[2][65], cfg_host[64];
static uint16_t cfg_port = 8787;
static int n_nets = 0, net_idx = 0;

static void nvs_get_s(nvs_handle_t h, const char *k, char *out, size_t n) {
  size_t len = n; if (nvs_get_str(h, k, out, &len) != ESP_OK) out[0] = 0;
}
static void load_cfg(void) {
  nvs_handle_t h;
  if (nvs_open("sysmon", NVS_READONLY, &h) != ESP_OK) return;
  nvs_get_s(h, "ssid", cfg_ssid[0], sizeof cfg_ssid[0]); nvs_get_s(h, "pass", cfg_pass[0], sizeof cfg_pass[0]);
  nvs_get_s(h, "ssid2", cfg_ssid[1], sizeof cfg_ssid[1]); nvs_get_s(h, "pass2", cfg_pass[1], sizeof cfg_pass[1]);
  nvs_get_s(h, "host", cfg_host, sizeof cfg_host);
  nvs_get_u16(h, "port", &cfg_port);
  uint8_t last = 0; nvs_get_u8(h, "lastnet", &last); net_idx = last;
  nvs_close(h);
  n_nets = cfg_ssid[0][0] ? (cfg_ssid[1][0] ? 2 : 1) : 0;
  if (net_idx >= n_nets) net_idx = 0;
}

// ---------------- state ----------------
typedef struct { char name[25]; int cpu, rss; } proc_t;
typedef struct {
  bool valid;
  int cpu, mp, cores[16], ncores, ntop, nh;
  float load, mused, mtot, swap, swapr, mdemand, mstored, mcomp;
  proc_t top[5];
  uint8_t hcpu[120]; uint16_t hsw[120];
} stats_t;
static stats_t st;

// AI subscription usage (Claude Code / Codex): percent used and seconds until reset per window
typedef struct { int h5, h5r, wk, wkr, age, busy; char plan[12], err[41]; } ai_t;
static ai_t ai[2];  // 0 = Claude Code, 1 = Codex
typedef enum { SCREEN_HOME, SCREEN_MONITOR } screen_t;
static screen_t screen = SCREEN_HOME;
static int page = 0;
#define NPAGES 2
#define HOME_X 16
#define HOME_Y 8
#define HOME_W 56
#define HOME_H 44
#define MONITOR_X 56
#define MONITOR_Y 128
#define MONITOR_W 196
#define MONITOR_H 232
static uint32_t last_ok_ms = 0, conn_start_ms = 0;
static char last_err[40] = "starting";
static volatile bool wifi_up = false;
static char my_ip[16] = "0.0.0.0", host_ip[16] = "";
static const uint8_t BRIGHT[] = {0, 40, 75};  // PWM duty: 0 = full brightness (inverted)
static int bright_idx = 0;

// ---------------- drawing ----------------
static uint16_t *bufs[2], *fb;
static int draw_idx = 0;

static inline void px(int x, int y, uint16_t c) { if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c; }
static void fill_rect(int x, int y, int w, int h, uint16_t c) {
  if (x < 0) { w += x; x = 0; }
  if (y < 0) { h += y; y = 0; }
  if (x + w > W) w = W - x;
  if (y + h > H) h = H - y;
  for (int j = 0; j < h; j++) { uint16_t *p = fb + (y + j) * W + x; for (int i = 0; i < w; i++) p[i] = c; }
}
static bool inside(int x, int y, int left, int top, int w, int h) {
  return x >= left && x < left + w && y >= top && y < top + h;
}
static void vline(int x, int y0, int y1, uint16_t c) { if (y0 > y1) { int t = y0; y0 = y1; y1 = t; } for (int y = y0; y <= y1; y++) px(x, y, c); }
static void line(int x0, int y0, int x1, int y1, uint16_t c) {
  int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, e = dx + dy;
  for (;;) { px(x0, y0, c); px(x0, y0 + 1, c); if (x0 == x1 && y0 == y1) break; int e2 = 2 * e; if (e2 >= dy) { e += dy; x0 += sx; } if (e2 <= dx) { e += dx; y0 += sy; } }
}
// ASCII-only text, foreground pixels only (regions are filled before text is drawn on them)
static void text(int x, int y, const char *s, sFONT *f, uint16_t fg) {
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
// area chart of 0..100 values, newest on the right, with y-axis labels in a left gutter
static void chart(int x, int y, int w, int h, const uint8_t *v, int n, uint16_t stroke, uint16_t fill,
                  const char *l_top, const char *l_mid, const char *l_bot) {
  fill_rect(x, y, GUTTER, h, C_BG);
  text(x + GUTTER - 6 - 7 * strlen(l_top), y, l_top, &Font12, C_DIM);
  text(x + GUTTER - 6 - 7 * strlen(l_mid), y + h / 2 - 6, l_mid, &Font12, C_DIM);
  text(x + GUTTER - 6 - 7 * strlen(l_bot), y + h - 12, l_bot, &Font12, C_DIM);
  x += GUTTER; w -= GUTTER;
  fill_rect(x, y, w, h, C_PANEL);
  for (int k = 1; k < 4; k++) for (int i = x; i < x + w; i += 4) px(i, y + h * k / 4, C_GRID);
  if (n < 2) return;
  float dx = (float)(w - 1) / 119.0f;
  int prev_x = -1, prev_y = 0;
  for (int i = 0; i < n; i++) {
    int xi = x + w - 1 - (int)((n - 1 - i) * dx);
    int yi = y + h - 1 - (int)(v[i] * (h - 2) / 100);
    if (prev_x >= 0) {
      for (int xx = prev_x; xx <= xi; xx++) {
        int span = xi - prev_x > 0 ? xi - prev_x : 1;
        vline(xx, prev_y + (yi - prev_y) * (xx - prev_x) / span, y + h - 1, fill);
      }
      line(prev_x, prev_y, xi, yi, stroke);
    }
    prev_x = xi; prev_y = yi;
  }
}

static void render_system(void);
static void render_ai(void);

static void render_home_icon(void) {
  int x = HOME_X + HOME_W / 2, y = HOME_Y + 6;
  fill_rect(HOME_X, HOME_Y, HOME_W, HOME_H, C_BLUE_F);
  line(x - 18, y + 16, x, y, C_TEXT);
  line(x, y, x + 18, y + 16, C_TEXT);
  fill_rect(x - 14, y + 16, 28, 20, C_TEXT);
  fill_rect(x - 11, y + 19, 22, 17, C_BLUE_F);
  fill_rect(x - 4, y + 25, 8, 11, C_TEXT);
}

static void render_home(void) {
  text(56, 38, "APPS", &Font48, C_TEXT);
  text(58, 94, "Your desk, at a glance", &Font16, C_DIM);

  fill_rect(MONITOR_X, MONITOR_Y, MONITOR_W, MONITOR_H, C_PANEL);
  fill_rect(MONITOR_X + 22, MONITOR_Y + 20, 152, 152, C_BLUE_F);
  for (int i = 0; i < 3; i++) {
    int h = 37 + i * 23;
    fill_rect(MONITOR_X + 49 + i * 34, MONITOR_Y + 142 - h, 18, h, i == 2 ? C_PURPLE : C_BLUE);
  }
  line(MONITOR_X + 38, MONITOR_Y + 98, MONITOR_X + 66, MONITOR_Y + 77, C_TEXT);
  line(MONITOR_X + 66, MONITOR_Y + 77, MONITOR_X + 101, MONITOR_Y + 86, C_TEXT);
  line(MONITOR_X + 101, MONITOR_Y + 86, MONITOR_X + 151, MONITOR_Y + 48, C_TEXT);
  text(MONITOR_X + 27, MONITOR_Y + 184, "MONITOR", &Font24, C_TEXT);
  text(MONITOR_X + 27, MONITOR_Y + 213, "SYSTEM + AI", &Font12, C_DIM);

  text(56, 432, "Tap an app to open", &Font16, C_DIM);
  bool online = st.valid && millis() - last_ok_ms < 5000;
  text(609, 434, online ? "MAC ONLINE" : "MAC OFFLINE", &Font12, online ? C_GREEN : C_AMBER);
}

static void render(void) {
  fb = bufs[draw_idx];
  fill_rect(0, 0, W, H, C_BG);
  if (screen == SCREEN_HOME) {
    render_home();
    waveshare_rgb_lcd_display((uint8_t *)fb);
    draw_idx ^= 1;
    return;
  }
  char s[64];
  uint32_t age = st.valid ? (millis() - last_ok_ms) / 1000 : 0;
  bool stale = !st.valid || age >= 5;

  // banner: memory pressure, or connection state
  uint16_t bc; const char *bt;
  if (!st.valid)       { bc = C_GRAY;  bt = "WAITING FOR MAC"; }
  else if (st.mp >= 4) { bc = C_RED;   bt = "MEMORY CRITICAL - STOP"; }
  else if (st.mp >= 2) { bc = C_AMBER; bt = "MEMORY WARNING"; }
  else                 { bc = C_GREEN; bt = "MEMORY OK"; }
  if (page == 0) {
    fill_rect(0, 0, W, 56, bc);
    text(124, 20, bt, &Font16, C_TEXT);
  }
  if (page == 0 && stale && st.valid) {
    snprintf(s, sizeof s, "MAC SILENT %lus", (unsigned long)age);
    fill_rect(W - 300, 8, 292, 40, C_RED); text(W - 290, 16, s, &Font24, C_TEXT);
  } else if (page == 0 && !st.valid) {
    int len = strlen(last_err); if (len > 30) len = 30;
    text(W - 16 - 11 * len, 20, last_err, &Font16, C_TEXT);
  }

  if (page == 0) render_system(); else render_ai();

  render_home_icon();

  // page dots
  for (int i = 0; i < NPAGES; i++) fill_rect(W / 2 - NPAGES * 10 + i * 20 + 3, 471, 8, 6, i == page ? C_TEXT : C_GRID);

  if (stale) for (int y = 64; y < H; y += 3) for (int x = (y / 3) % 2; x < W; x += 6) px(x, y, C_BG);  // dim stale data

  waveshare_rgb_lcd_display((uint8_t *)fb);
  draw_idx ^= 1;
}

static void render_system(void) {
  char s[64];
  // left column
  text(16, 72, "CPU", &Font16, C_DIM);
  snprintf(s, sizeof s, "%d%%", st.cpu); text(16, 92, s, &Font48, st.cpu >= 85 ? C_HOT : C_TEXT);
  snprintf(s, sizeof s, "load %.2f", st.load); text(16, 144, s, &Font16, C_DIM);
  // "used / total" hides compression; show how much data apps really hold vs physical RAM
  text(16, 176, "APP DATA vs RAM", &Font16, C_DIM);
  snprintf(s, sizeof s, "%.1fG/%.0fG", st.mdemand, st.mtot);
  text(16, 196, s, &Font24, st.mdemand > st.mtot ? (st.mdemand > st.mtot * 1.5f ? C_HOT : 0xFE60) : C_TEXT);
  snprintf(s, sizeof s, "zip %.1fG -> %.1fG", st.mstored, st.mcomp); text(16, 224, s, &Font12, C_DIM);
  snprintf(s, sizeof s, "ram %.1fG  swap %.1fG", st.mused, st.swap); text(16, 240, s, &Font12, C_DIM);
  text(16, 262, "TOP", &Font16, C_DIM);
  for (int i = 0; i < st.ntop; i++) {
    char nm[15]; strncpy(nm, st.top[i].name, 14); nm[14] = 0;
    text(16, 284 + i * 36, nm, &Font16, C_TEXT);
    snprintf(s, sizeof s, "%d%% %dM", st.top[i].cpu, st.top[i].rss);
    text(16, 302 + i * 36, s, &Font12, C_DIM);
  }

  // right: CPU chart, per-core bars, swap chart
  const int X = 262, CW = W - X - 12;
  text(X, 68, "CPU  last 2 min", &Font12, C_DIM);
  chart(X, 84, CW, 170, st.hcpu, st.nh, C_BLUE, C_BLUE_F, "100%", "50%", "0%");
  if (st.ncores) {
    int bw = (CW - GUTTER - (st.ncores - 1) * 6) / st.ncores;
    for (int i = 0; i < st.ncores; i++) {
      int bx = X + GUTTER + i * (bw + 6), bh = 34, fh = st.cores[i] * bh / 100;
      fill_rect(bx, 262, bw, bh, C_PANEL);
      fill_rect(bx, 262 + bh - fh, bw, fh, st.cores[i] >= 90 ? C_HOT : C_BLUE);
    }
  }
  // swap I/O, auto-scaled to a round ceiling so idle stays flat and bursts stay readable
  int peak = 0; for (int i = 0; i < st.nh; i++) if (st.hsw[i] > peak) peak = st.hsw[i];
  static const int CEIL[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
  int top = 5000; for (int i = 0; i < 9; i++) if (peak <= CEIL[i]) { top = CEIL[i]; break; }
  uint8_t pct[120]; for (int i = 0; i < st.nh; i++) { int p = st.hsw[i] * 100 / top; pct[i] = p > 100 ? 100 : p; }
  bool hot = st.swapr >= 20;
  snprintf(s, sizeof s, "SWAP I/O  now %.0f MB/s  (0 = healthy)", st.swapr);
  text(X, 306, s, &Font12, hot ? C_HOT : C_DIM);
  char s_top[12], s_mid[12];
  snprintf(s_top, sizeof s_top, "%d", top); snprintf(s_mid, sizeof s_mid, "%d", top / 2);
  chart(X, 322, CW, 146, pct, st.nh, hot ? C_HOT : C_PURPLE, C_PURP_F, s_top, s_mid, "0");
}

static void fmt_dur(char *out, size_t n, int secs) {
  if (secs < 0) snprintf(out, n, "reset time unknown");
  else if (secs >= 86400) snprintf(out, n, "resets in %dd %dh", secs / 86400, secs % 86400 / 3600);
  else if (secs >= 3600) snprintf(out, n, "resets in %dh %dm", secs / 3600, secs % 3600 / 60);
  else snprintf(out, n, "resets in %dm", (secs + 59) / 60);
}
static void usage_row(int y, const char *label, int pct, int reset) {
  char s[40];
  text(16, y + 8, label, &Font16, C_DIM);
  const int bx = 130, bw = 500, bh = 32;
  fill_rect(bx, y, bw, bh, C_PANEL);
  if (pct >= 0) {
    int w = (pct > 100 ? 100 : pct) * bw / 100;
    fill_rect(bx, y, w, bh, pct >= 90 ? C_HOT : pct >= 70 ? 0xFE60 : C_BLUE);
    snprintf(s, sizeof s, "%d%%", pct);
  } else snprintf(s, sizeof s, "--");
  text(650, y + 4, s, &Font24, pct >= 90 ? C_HOT : C_TEXT);
  fmt_dur(s, sizeof s, reset);
  text(bx, y + 38, s, &Font12, C_DIM);
}
static void usage_section(int y, const char *name, const ai_t *a) {
  char s[64];
  text(16, y, name, &Font24, C_TEXT);
  if (a->plan[0]) { snprintf(s, sizeof s, "(%s)", a->plan); text(16 + 17 * strlen(name) + 10, y + 6, s, &Font16, C_DIM); }
  if (a->err[0]) { snprintf(s, sizeof s, "! %s", a->err); text(W - 16 - 7 * strlen(s), y + 8, s, &Font12, C_HOT); }
  else if (a->age >= 0) { snprintf(s, sizeof s, "updated %dm ago", a->age / 60); text(W - 16 - 7 * strlen(s), y + 8, s, &Font12, C_DIM); }
  usage_row(y + 40, "5 HOUR", a->h5, a->h5r);
  usage_row(y + 112, "WEEKLY", a->wk, a->wkr);
}
#define BTN_X 600
#define BTN_Y 10
#define BTN_W 184
#define BTN_H 44
static uint32_t refresh_pressed_ms = 0;
static void render_ai(void) {
  text(124, 20, "AI USAGE", &Font24, C_TEXT);
  bool busy = ai[0].busy || ai[1].busy || millis() - refresh_pressed_ms < 1500;
  fill_rect(BTN_X, BTN_Y, BTN_W, BTN_H, busy ? C_GRAY : C_BLUE);
  const char *lbl = busy ? "UPDATING" : "REFRESH";
  text(BTN_X + (BTN_W - 17 * strlen(lbl)) / 2, BTN_Y + 10, lbl, &Font24, C_TEXT);
  if (!st.valid || millis() - last_ok_ms >= 5000) text(328, 26, "mac not responding", &Font12, C_HOT);
  usage_section(72, "CLAUDE CODE", &ai[0]);
  for (int x = 16; x < W - 16; x += 4) px(x, 254, C_GRID);
  usage_section(264, "CODEX", &ai[1]);
}

// ---------------- network ----------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_up = false;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
    snprintf(my_ip, sizeof my_ip, IPSTR, IP2STR(&e->ip_info.ip));
    wifi_up = true;
  }
}
static void connect_net(int i) {
  net_idx = i; conn_start_ms = millis(); host_ip[0] = 0; wifi_up = false;
  esp_wifi_disconnect();
  wifi_config_t wc = {0};
  strlcpy((char *)wc.sta.ssid, cfg_ssid[i], sizeof wc.sta.ssid);
  strlcpy((char *)wc.sta.password, cfg_pass[i], sizeof wc.sta.password);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  esp_wifi_connect();
  snprintf(last_err, sizeof last_err, "joining %s", cfg_ssid[i]);
}
static void wifi_start(void) {
  esp_netif_init();
  esp_event_loop_create_default();
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);  // no flash writes on (re)connect
  esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
  esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL);
  esp_wifi_set_mode(WIFI_MODE_STA);
  esp_wifi_start();
  esp_wifi_set_ps(WIFI_PS_NONE);
  mdns_init();
  mdns_hostname_set("sysmon");
  connect_net(net_idx);
}
static bool resolve_host(void) {
  struct in_addr a;
  if (inet_aton(cfg_host, &a)) { strlcpy(host_ip, cfg_host, sizeof host_ip); return true; }
  char h[64]; strlcpy(h, cfg_host, sizeof h);
  char *dot = strstr(h, ".local"); if (dot) *dot = 0;
  esp_ip4_addr_t addr = {0};
  if (mdns_query_a(h, 1500, &addr) != ESP_OK) return false;
  snprintf(host_ip, sizeof host_ip, IPSTR, IP2STR(&addr));
  return true;
}

static char body[6144]; static int body_len;
static esp_err_t http_event(esp_http_client_event_t *e) {
  if (e->event_id == HTTP_EVENT_ON_DATA && body_len + e->data_len < (int)sizeof body - 1) {
    memcpy(body + body_len, e->data, e->data_len); body_len += e->data_len;
  }
  return ESP_OK;
}
static bool fetch(void) {
  if (!wifi_up) { snprintf(last_err, sizeof last_err, "wifi connecting"); return false; }
  if (!host_ip[0] && !resolve_host()) { snprintf(last_err, sizeof last_err, "mac not found"); return false; }
  char url[96]; snprintf(url, sizeof url, "http://%s:%u/api/now", host_ip, cfg_port);
  esp_http_client_config_t hc = { .url = url, .timeout_ms = 900, .event_handler = http_event };
  esp_http_client_handle_t c = esp_http_client_init(&hc);
  body_len = 0;
  esp_err_t err = esp_http_client_perform(c);
  int code = esp_http_client_get_status_code(c);
  esp_http_client_cleanup(c);
  if (err != ESP_OK || code != 200) {
    snprintf(last_err, sizeof last_err, "http %s", err != ESP_OK ? esp_err_to_name(err) : "status");
    if (err != ESP_OK) host_ip[0] = 0;  // re-resolve next time (Mac IP may have changed)
    return false;
  }
  body[body_len] = 0;
  cJSON *j = cJSON_Parse(body);
  if (!j) { snprintf(last_err, sizeof last_err, "json err"); return false; }
#define NUM(k) (cJSON_GetObjectItem(j, k) ? cJSON_GetObjectItem(j, k)->valuedouble : 0)
  st.cpu = NUM("cpu"); st.mp = NUM("mp"); st.load = NUM("load"); st.mused = NUM("mused");
  st.mtot = NUM("mtot"); st.swap = NUM("swap"); st.swapr = NUM("swapr");
  st.mdemand = NUM("mdemand"); st.mstored = NUM("mstored"); st.mcomp = NUM("mcomp");
  cJSON *a, *it;
  st.ncores = 0; a = cJSON_GetObjectItem(j, "cores");
  cJSON_ArrayForEach(it, a) if (st.ncores < 16) st.cores[st.ncores++] = it->valueint;
  st.ntop = 0; a = cJSON_GetObjectItem(j, "top");
  cJSON_ArrayForEach(it, a) {
    if (st.ntop >= 5) break;
    cJSON *n = cJSON_GetArrayItem(it, 0);
    strlcpy(st.top[st.ntop].name, cJSON_IsString(n) ? n->valuestring : "?", sizeof st.top[0].name);
    st.top[st.ntop].cpu = cJSON_GetArrayItem(it, 1) ? cJSON_GetArrayItem(it, 1)->valueint : 0;
    st.top[st.ntop].rss = cJSON_GetArrayItem(it, 2) ? cJSON_GetArrayItem(it, 2)->valueint : 0;
    st.ntop++;
  }
  cJSON *hc_a = cJSON_GetObjectItem(j, "hcpu"), *hs_a = cJSON_GetObjectItem(j, "hsw");
  st.nh = 0;
  int n = cJSON_GetArraySize(hc_a); if (n > 120) n = 120;
  for (int i = 0; i < n; i++) {
    int v = cJSON_GetArrayItem(hc_a, i)->valueint; st.hcpu[i] = v < 0 ? 0 : v > 100 ? 100 : v;
    cJSON *sv = cJSON_GetArrayItem(hs_a, i); int w = sv ? sv->valueint : 0; st.hsw[i] = w < 0 ? 0 : w > 60000 ? 60000 : w;
    st.nh++;
  }
  cJSON *ai_o = cJSON_GetObjectItem(j, "ai");
  const char *keys[2] = {"cc", "cx"};
  for (int k = 0; k < 2 && ai_o; k++) {
    cJSON *o = cJSON_GetObjectItem(ai_o, keys[k]); if (!o) continue;
    cJSON *h5 = cJSON_GetObjectItem(o, "h5"), *wk = cJSON_GetObjectItem(o, "wk");
    ai[k].h5 = h5 ? cJSON_GetArrayItem(h5, 0)->valueint : -1; ai[k].h5r = h5 ? cJSON_GetArrayItem(h5, 1)->valueint : -1;
    ai[k].wk = wk ? cJSON_GetArrayItem(wk, 0)->valueint : -1; ai[k].wkr = wk ? cJSON_GetArrayItem(wk, 1)->valueint : -1;
    cJSON *age = cJSON_GetObjectItem(o, "age"); ai[k].age = age ? age->valueint : -1;
    cJSON *busy = cJSON_GetObjectItem(o, "busy"); ai[k].busy = busy ? busy->valueint : 0;
    cJSON *pl = cJSON_GetObjectItem(o, "plan"), *er = cJSON_GetObjectItem(o, "err");
    strlcpy(ai[k].plan, cJSON_IsString(pl) ? pl->valuestring : "", sizeof ai[k].plan);
    strlcpy(ai[k].err, cJSON_IsString(er) ? er->valuestring : "", sizeof ai[k].err);
  }
#undef NUM
  cJSON_Delete(j);
  st.valid = true; last_ok_ms = millis(); snprintf(last_err, sizeof last_err, "ok");
  return true;
}

static void request_ai_refresh(void) {
  if (!wifi_up || !host_ip[0]) return;
  char url[96]; snprintf(url, sizeof url, "http://%s:%u/api/ai/refresh", host_ip, cfg_port);
  esp_http_client_config_t hc = { .url = url, .timeout_ms = 900 };
  esp_http_client_handle_t c = esp_http_client_init(&hc);
  esp_http_client_perform(c);  // 202 started / 429 too soon: either way the next poll shows the state
  esp_http_client_cleanup(c);
}

// ---------------- serial config console ----------------
static void console_task(void *arg) {
  char l[256];
  for (;;) {
    if (!fgets(l, sizeof l, stdin)) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }
    l[strcspn(l, "\r\n")] = 0;
    if (!strncmp(l, "set ", 4)) {
      char *k = l + 4, *v = strchr(k, ' ');
      if (!v) { printf("ERR usage: set <key> <value>\n"); continue; }
      *v++ = 0;
      nvs_handle_t h; nvs_open("sysmon", NVS_READWRITE, &h);
      esp_err_t e = ESP_OK;
      if (!strcmp(k, "port")) e = nvs_set_u16(h, "port", atoi(v));
      else if (!strcmp(k, "ssid") || !strcmp(k, "pass") || !strcmp(k, "ssid2") || !strcmp(k, "pass2") || !strcmp(k, "host")) e = nvs_set_str(h, k, v);
      else { nvs_close(h); printf("ERR unknown key\n"); continue; }
      nvs_commit(h); nvs_close(h);
      printf(e == ESP_OK ? "OK %s\n" : "ERR %s\n", k);
    } else if (!strcmp(l, "status")) {
      wifi_ap_record_t ap = {0}; esp_wifi_sta_get_ap_info(&ap);
      printf("ssid=%s ssid2=%s connected=%s host=%s port=%u wifi=%d ip=%s hostIp=%s last=%s\n",
             cfg_ssid[0], cfg_ssid[1], wifi_up ? (char *)ap.ssid : "", cfg_host, cfg_port, wifi_up, my_ip,
             host_ip[0] ? host_ip : "0.0.0.0", last_err);
    } else if (!strcmp(l, "reboot")) {
      printf("OK rebooting\n"); fflush(stdout); vTaskDelay(pdMS_TO_TICKS(100)); esp_restart();
    }
  }
}

// ---------------- main ----------------
static void fonts_to_sram(void) {
  sFONT *fonts[] = {&Font12, &Font16, &Font24, &Font48};
  for (int i = 0; i < 4; i++) {
    size_t n = 95 * fonts[i]->Height * ((fonts[i]->Width + 7) / 8);
    uint8_t *p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (p) { memcpy(p, fonts[i]->table, n); fonts[i]->table = p; }
  }
}

void app_main(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

  usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&uc);
  usb_serial_jtag_vfs_use_driver();
  setvbuf(stdin, NULL, _IONBF, 0);

  fonts_to_sram();
  touch_gt911_init();  // also brings up I2C and the IO expander
  waveshare_esp32_s3_rgb_lcd_init();
  waveshare_rgb_lcd_bl_on();
  IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]);
  void *b1, *b2; waveshare_get_frame_buffer(&b1, &b2);
  bufs[0] = b1; bufs[1] = b2;

  for (int k = 0; k < 2; k++) { ai[k].h5 = ai[k].wk = ai[k].h5r = ai[k].wkr = ai[k].age = -1; strlcpy(ai[k].err, "waiting", sizeof ai[k].err); }
  load_cfg();
  xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
  if (n_nets) wifi_start(); else snprintf(last_err, sizeof last_err, "no wifi config");
  render();

  uint32_t next_fetch = 0; int saved_net = -1;
  bool touching = false; int t_x0 = 0, t_y0 = 0, t_x = 0, t_y = 0; uint32_t t_ms = 0;
  for (;;) {
    touch_gt911_point_t tp = touch_gt911_read_point(1);
    if (tp.cnt > 0) {
      if (!touching) { touching = true; t_x0 = tp.x[0]; t_y0 = tp.y[0]; t_ms = millis(); }
      t_x = tp.x[0]; t_y = tp.y[0];
    } else if (touching) {  // released: route the gesture to the visible screen
      touching = false;
      int dx = t_x - t_x0;
      bool tap = abs(dx) < 25 && abs(t_y - t_y0) < 25 && millis() - t_ms < 600;
      if (screen == SCREEN_HOME) {
        if (tap && inside(t_x0, t_y0, MONITOR_X, MONITOR_Y, MONITOR_W, MONITOR_H)) {
          screen = SCREEN_MONITOR; page = 0; render();
        } else if (tap) {
          bright_idx = (bright_idx + 1) % sizeof BRIGHT; IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]);
        }
      } else if (tap && inside(t_x0, t_y0, HOME_X, HOME_Y, HOME_W, HOME_H)) {
        screen = SCREEN_HOME; render();
      } else if (dx <= -100 || dx >= 100) {
        page = (page + (dx < 0 ? 1 : NPAGES - 1)) % NPAGES; render();
      } else if (tap) {
        if (page == 1 && inside(t_x0, t_y0, BTN_X - 10, BTN_Y - 10, BTN_W + 20, BTN_H + 20)) {
          refresh_pressed_ms = millis(); render(); request_ai_refresh();
        } else { bright_idx = (bright_idx + 1) % sizeof BRIGHT; IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]); }
      }
    }

    if ((int32_t)(millis() - next_fetch) >= 0) {
      next_fetch = millis() + 1000;
      if (n_nets) {
        // A network can be joinable yet unable to reach the Mac (guest / isolated SSID):
        // fall over to the other one if we can't connect in 15 s or get no data for 30 s.
        uint32_t since = millis() - (last_ok_ms > conn_start_ms ? last_ok_ms : conn_start_ms);
        if ((!wifi_up && millis() - conn_start_ms > 15000) || (wifi_up && n_nets > 1 && since > 30000))
          connect_net((net_idx + 1) % n_nets);
        else if (!wifi_up && millis() - conn_start_ms > 5000 && n_nets == 1)
          connect_net(0);
        if (fetch() && saved_net != net_idx) {
          nvs_handle_t h; if (nvs_open("sysmon", NVS_READWRITE, &h) == ESP_OK) { nvs_set_u8(h, "lastnet", net_idx); nvs_commit(h); nvs_close(h); }
          saved_net = net_idx;
        }
      }
      render();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
