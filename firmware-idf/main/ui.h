#pragma once

#include <stdbool.h>
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

void ui_init(esp_lcd_panel_handle_t panel);
void ui_present(const stats_t *stats, const ai_t ai[2], bool mac_online, const char *last_error);
bool ui_take_refresh_request(void);
bool ui_take_brightness_request(void);
