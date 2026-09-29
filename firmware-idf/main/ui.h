#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_lcd_panel_ops.h"

typedef struct { char name[25]; int cpu, rss; } proc_t;
typedef struct {
  bool valid;
  int cpu, mp, cores[16], ncores, ntop, nh;
  float load, mused, mtot, swap, swapr, mdemand, mstored, mcomp;
  proc_t top[5];
  uint8_t hcpu[120]; uint16_t hsw[120];
} stats_t;

typedef struct { int h5, h5r, wk, wkr, age, busy; char plan[12], err[41]; } ai_t;

#define UI_WIFI_MAX_APS 12
typedef enum { UI_WIFI_SCAN, UI_WIFI_CONNECT } ui_wifi_request_kind_t;
typedef struct {
  ui_wifi_request_kind_t kind;
  uint8_t ssid[33];
  uint8_t ssid_len;
  bool open;
  char password[65];
} ui_wifi_request_t;
typedef struct {
  uint8_t ssid[33];
  uint8_t ssid_len;
  bool open;
} ui_wifi_ap_t;
typedef struct {
  char connected_ssid[33];
  char message[80];
  bool connected;
  bool busy;
  uint8_t ap_count;
  ui_wifi_ap_t aps[UI_WIFI_MAX_APS];
} ui_wifi_view_t;

void ui_init(esp_lcd_panel_handle_t panel);
// Caller owns *pixels and must free it. Captures the last displayed RGB565 frame.
bool ui_capture_rgb565(uint8_t **pixels, size_t *size);
void ui_present(const stats_t *stats, const ai_t ai[2], bool mac_online, const char *last_error);
bool ui_take_refresh_request(void);
bool ui_take_brightness_request(void);
bool ui_take_wifi_request(ui_wifi_request_t *out);
void ui_wifi_present(const ui_wifi_view_t *view);
