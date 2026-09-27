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
#include "ui.h"
#include "gt911.h"
#include "io_extension.h"

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
static stats_t st;
static ai_t ai[2];  // Claude Code, Codex
static uint32_t last_ok_ms = 0, conn_start_ms = 0;
static char last_err[40] = "starting";
static volatile bool wifi_up = false;
static char my_ip[16] = "0.0.0.0", host_ip[16] = "";
static const uint8_t BRIGHT[] = {0, 40, 75};  // PWM duty: 0 = full brightness (inverted)
static int bright_idx = 0;

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
void app_main(void) {
  esp_err_t e = nvs_flash_init();
  if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); nvs_flash_init(); }

  usb_serial_jtag_driver_config_t uc = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&uc);
  usb_serial_jtag_vfs_use_driver();
  setvbuf(stdin, NULL, _IONBF, 0);

  touch_gt911_init();  // initializes the I2C expander too
  esp_lcd_panel_handle_t panel = waveshare_esp32_s3_rgb_lcd_init();
  ui_init(panel);
  waveshare_rgb_lcd_bl_on();
  IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]);

  for (int k = 0; k < 2; k++) { ai[k].h5 = ai[k].wk = ai[k].h5r = ai[k].wkr = ai[k].age = -1; strlcpy(ai[k].err, "waiting", sizeof ai[k].err); }
  load_cfg();
  xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
  if (n_nets) wifi_start(); else snprintf(last_err, sizeof last_err, "no wifi config");
  ui_present(&st, ai, false, last_err);

  uint32_t next_fetch = 0; int saved_net = -1;
  for (;;) {
    if (ui_take_brightness_request()) {
      bright_idx = (bright_idx + 1) % sizeof BRIGHT;
      IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]);
    }
    if (ui_take_refresh_request()) request_ai_refresh();

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
      ui_present(&st, ai, st.valid && millis() - last_ok_ms < 5000, last_err);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
