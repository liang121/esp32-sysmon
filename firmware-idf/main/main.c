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
#include "freertos/queue.h"
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
#include "wifi_policy.h"
#include "gt911.h"
#include "io_extension.h"

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// ---------------- config ----------------
static char cfg_ssid[2][33], cfg_pass[2][65], cfg_host[64];
static uint16_t cfg_port = 8787;
static int n_nets = 0, net_idx = 0;
static int saved_net_in_nvs = -1;

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
  uint8_t last = 0; nvs_get_u8(h, "lastnet", &last); net_idx = saved_net_in_nvs = last;
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
static uint32_t last_resolve_ms;
static ui_wifi_view_t wifi_view;
typedef enum { NET_DISCONNECTED, NET_GOT_IP, NET_SCAN_DONE } net_event_kind_t;
typedef struct { net_event_kind_t kind; uint8_t reason; esp_ip4_addr_t ip; } net_event_t;
static QueueHandle_t net_events;
static esp_netif_t *wifi_netif;
typedef enum { CANDIDATE_NONE, CANDIDATE_WAIT_DISCONNECT, CANDIDATE_CONNECTING,
               CANDIDATE_ROLLBACK_WAIT } candidate_phase_t;
static candidate_phase_t candidate_phase;
static char candidate_ssid[33], candidate_pass[65];
static bool scan_busy, retry_pending, saved_connecting, candidate_timeout_issued;
static uint32_t retry_at_ms;
static unsigned wifi_failures;
static const uint8_t BRIGHT[] = {0, 40, 75};  // PWM duty: 0 = full brightness (inverted)
static int bright_idx = 0;

// ---------------- network ----------------
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  net_event_t event = {0};
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    event.kind = NET_DISCONNECTED;
    event.reason = ((wifi_event_sta_disconnected_t *)data)->reason;
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    event.kind = NET_GOT_IP;
    event.ip = ((ip_event_got_ip_t *)data)->ip_info.ip;
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) event.kind = NET_SCAN_DONE;
  else return;
  if (net_events && xQueueSend(net_events, &event, 0) != pdTRUE)
    ESP_LOGW("sysmon", "network event queue full");
}

static void show_wifi_state(const char *message, bool busy) {
  strlcpy(wifi_view.message, message, sizeof wifi_view.message);
  wifi_view.connected = wifi_up;
  wifi_view.busy = busy;
  wifi_view.connected_ssid[0] = 0;
  if (wifi_up) {
    wifi_ap_record_t ap = {0};
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
      memcpy(wifi_view.connected_ssid, ap.ssid, sizeof wifi_view.connected_ssid - 1);
  }
  ui_wifi_present(&wifi_view);
}

static void connect_net(int i) {
  net_idx = i; conn_start_ms = millis(); host_ip[0] = 0; wifi_up = false;
  saved_connecting = true;
  wifi_config_t wc = {0};
  wifi_copy_ssid(wc.sta.ssid, cfg_ssid[i]);
  strlcpy((char *)wc.sta.password, cfg_pass[i], sizeof wc.sta.password);
  esp_wifi_set_config(WIFI_IF_STA, &wc);
  esp_wifi_connect();
  snprintf(last_err, sizeof last_err, "joining %s", cfg_ssid[i]);
  show_wifi_state("Connecting to saved Wi-Fi...", true);
}
static void wifi_start(void) {
  net_events = xQueueCreate(16, sizeof(net_event_t));
  ESP_ERROR_CHECK(net_events ? ESP_OK : ESP_ERR_NO_MEM);
  esp_netif_init();
  esp_event_loop_create_default();
  wifi_netif = esp_netif_create_default_wifi_sta();
  ESP_ERROR_CHECK(wifi_netif ? ESP_OK : ESP_ERR_NO_MEM);
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
  if (n_nets) connect_net(net_idx);
  else show_wifi_state("No saved Wi-Fi. Tap SCAN.", false);
}

static void clear_candidate(void) {
  candidate_phase = CANDIDATE_NONE;
  candidate_timeout_issued = false;
  memset(candidate_pass, 0, sizeof candidate_pass);
  memset(candidate_ssid, 0, sizeof candidate_ssid);
}

static void rollback_candidate(const char *reason) {
  clear_candidate();
  retry_pending = false;
  strlcpy(last_err, reason, sizeof last_err);
  if (wifi_up) {
    candidate_phase = CANDIDATE_ROLLBACK_WAIT;
    if (esp_wifi_disconnect() == ESP_OK) {
      show_wifi_state(reason, true);
      return;
    }
    candidate_phase = CANDIDATE_NONE;
  }
  if (n_nets) connect_net(net_idx);
  show_wifi_state(reason, n_nets > 0);
}

static void apply_candidate(void) {
  wifi_config_t wc = {0};
  wifi_copy_ssid(wc.sta.ssid, candidate_ssid);
  strlcpy((char *)wc.sta.password, candidate_pass, sizeof wc.sta.password);
  candidate_phase = CANDIDATE_CONNECTING;
  candidate_timeout_issued = false;
  conn_start_ms = millis();
  wifi_up = false;
  host_ip[0] = 0;
  esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
  if (err == ESP_OK) err = esp_wifi_connect();
  memset(&wc, 0, sizeof wc);
  if (err != ESP_OK) { rollback_candidate("Wi-Fi connection failed"); return; }
  show_wifi_state("Connecting to selected Wi-Fi...", true);
}

static bool save_candidate(void) {
  char ssids[2][33], passwords[2][65];
  memcpy(ssids, cfg_ssid, sizeof ssids);
  memcpy(passwords, cfg_pass, sizeof passwords);
  int count = n_nets;
  wifi_promote(ssids, passwords, &count, candidate_ssid, candidate_pass);
  nvs_handle_t h;
  esp_err_t err = nvs_open("sysmon", NVS_READWRITE, &h);
  if (err == ESP_OK) {
    err = nvs_set_str(h, "ssid", ssids[0]);
    if (err == ESP_OK) err = nvs_set_str(h, "pass", passwords[0]);
    if (err == ESP_OK) err = nvs_set_str(h, "ssid2", count > 1 ? ssids[1] : "");
    if (err == ESP_OK) err = nvs_set_str(h, "pass2", count > 1 ? passwords[1] : "");
    if (err == ESP_OK) err = nvs_set_u8(h, "lastnet", 0);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
  }
  if (err == ESP_OK) {
    memcpy(cfg_ssid, ssids, sizeof cfg_ssid);
    memcpy(cfg_pass, passwords, sizeof cfg_pass);
    n_nets = count;
    net_idx = saved_net_in_nvs = 0;
  }
  memset(passwords, 0, sizeof passwords);
  return err == ESP_OK;
}

static void start_candidate(const ui_wifi_request_t *request) {
  size_t pass_len = strnlen(request->password, sizeof request->password);
  if (!request->ssid_len || request->ssid_len > 32 ||
      (!request->open && (pass_len < 8 || pass_len > 63))) {
    show_wifi_state("Password must have 8-63 characters", false);
    return;
  }
  memcpy(candidate_ssid, request->ssid, request->ssid_len);
  candidate_ssid[request->ssid_len] = 0;
  strlcpy(candidate_pass, request->password, sizeof candidate_pass);
  retry_pending = false;
  saved_connecting = false;
  if (n_nets) {
    candidate_phase = CANDIDATE_WAIT_DISCONNECT;
    if (esp_wifi_disconnect() == ESP_OK) {
      show_wifi_state("Changing Wi-Fi...", true);
      return;
    }
  }
  apply_candidate();
}

static void receive_scan(void) {
  uint16_t count = UI_WIFI_MAX_APS;
  wifi_ap_record_t records[UI_WIFI_MAX_APS] = {0};
  esp_err_t err = esp_wifi_scan_get_ap_records(&count, records);
  wifi_view.ap_count = 0;
  if (err == ESP_OK) {
    for (uint16_t i = 0; i < count; ++i) {
      ui_wifi_ap_t *ap = &wifi_view.aps[wifi_view.ap_count++];
      ap->ssid_len = strnlen((char *)records[i].ssid, 32);
      memcpy(ap->ssid, records[i].ssid, ap->ssid_len);
      ap->ssid[ap->ssid_len] = 0;
      ap->open = records[i].authmode == WIFI_AUTH_OPEN;
    }
  }
  scan_busy = false;
  show_wifi_state(err != ESP_OK ? "Wi-Fi scan failed" : count ? "Choose a network" : "No networks found", false);
}

static void handle_network_event(const net_event_t *event) {
  if (event->kind == NET_SCAN_DONE) { receive_scan(); return; }
  if (event->kind == NET_DISCONNECTED) {
    wifi_up = false;
    host_ip[0] = 0;
    saved_connecting = false;
    if (candidate_phase == CANDIDATE_WAIT_DISCONNECT) { apply_candidate(); return; }
    if (candidate_phase == CANDIDATE_ROLLBACK_WAIT) {
      candidate_phase = CANDIDATE_NONE;
      if (n_nets) connect_net(net_idx);
      return;
    }
    if (candidate_phase == CANDIDATE_CONNECTING) {
      rollback_candidate(event->reason == WIFI_REASON_AUTH_FAIL ? "Wrong Wi-Fi password" : "Wi-Fi connection failed");
      return;
    }
    strlcpy(last_err, "wifi disconnected", sizeof last_err);
    wifi_failures++;
    retry_pending = n_nets > 0;
    retry_at_ms = millis() + 3000; // failure backoff, not event ordering
    show_wifi_state("Wi-Fi disconnected", scan_busy);
    return;
  }
  if (event->kind == NET_GOT_IP) {
    if (candidate_phase == CANDIDATE_WAIT_DISCONNECT ||
        candidate_phase == CANDIDATE_ROLLBACK_WAIT || candidate_timeout_issued) return;
    wifi_ap_record_t ap = {0};
    esp_netif_ip_info_t ip_info = {0};
    const char *expected_ssid = candidate_phase == CANDIDATE_CONNECTING
                                    ? candidate_ssid : n_nets ? cfg_ssid[net_idx] : "";
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK ||
        esp_netif_get_ip_info(wifi_netif, &ip_info) != ESP_OK ||
        !wifi_got_ip_matches(expected_ssid, ap.ssid, event->ip.addr, ip_info.ip.addr)) return;
    snprintf(my_ip, sizeof my_ip, IPSTR, IP2STR(&event->ip));
    wifi_up = true;
    wifi_failures = 0;
    saved_connecting = false;
    retry_pending = false;
    if (candidate_phase == CANDIDATE_CONNECTING) {
      if (!save_candidate()) { rollback_candidate("Could not save Wi-Fi"); return; }
      clear_candidate();
    } else if (saved_net_in_nvs != net_idx && n_nets) {
      nvs_handle_t h;
      if (nvs_open("sysmon", NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_u8(h, "lastnet", net_idx) == ESP_OK && nvs_commit(h) == ESP_OK)
          saved_net_in_nvs = net_idx;
        nvs_close(h);
      }
    }
    show_wifi_state("Wi-Fi connected", scan_busy);
  }
}

static void handle_wifi_request(ui_wifi_request_t *request) {
  if (request->kind == UI_WIFI_SCAN && !scan_busy && candidate_phase == CANDIDATE_NONE) {
    scan_busy = true;
    show_wifi_state("Scanning...", true);
    esp_err_t err = esp_wifi_scan_start(NULL, false);
    if (err != ESP_OK) {
      scan_busy = false;
      show_wifi_state("Wi-Fi scan failed", false);
    }
  } else if (request->kind == UI_WIFI_CONNECT && !scan_busy && candidate_phase == CANDIDATE_NONE) {
    start_candidate(request);
  }
  memset(request->password, 0, sizeof request->password);
}

static bool resolve_host(void) {
  struct in_addr a;
  if (inet_aton(cfg_host, &a)) { strlcpy(host_ip, cfg_host, sizeof host_ip); return true; }
  last_resolve_ms = millis();
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
    // Keep trying the last working IP while refreshing mDNS. A transient mDNS
    // failure during a Mac service outage must not erase the working address.
    if (host_ip[0] && millis() - last_resolve_ms >= 5000) resolve_host();
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
  wifi_start();
  if (!n_nets) snprintf(last_err, sizeof last_err, "no wifi config");
  ui_present(&st, ai, false, last_err);

  uint32_t next_fetch = 0;
  for (;;) {
    net_event_t event;
    while (xQueueReceive(net_events, &event, 0) == pdTRUE) handle_network_event(&event);
    ui_wifi_request_t request;
    while (ui_take_wifi_request(&request)) handle_wifi_request(&request);
    if (candidate_phase == CANDIDATE_CONNECTING && !candidate_timeout_issued &&
        millis() - conn_start_ms > 20000) {
      candidate_timeout_issued = true;
      if (esp_wifi_disconnect() != ESP_OK) rollback_candidate("Wi-Fi connection timed out");
    }
    if (saved_connecting && millis() - conn_start_ms > 15000) {
      saved_connecting = false;
      if (esp_wifi_disconnect() != ESP_OK) {
        retry_pending = true;
        retry_at_ms = millis() + 3000;
      }
    }
    if (retry_pending && !saved_connecting && !scan_busy && candidate_phase == CANDIDATE_NONE &&
        (int32_t)(millis() - retry_at_ms) >= 0) {
      retry_pending = false;
      int next = wifi_next_network(n_nets, net_idx, wifi_up, wifi_failures);
      if (next >= 0) connect_net(next);
    }
    if (ui_take_brightness_request()) {
      bright_idx = (bright_idx + 1) % sizeof BRIGHT;
      IO_EXTENSION_Pwm_Output(BRIGHT[bright_idx]);
    }
    if (ui_take_refresh_request()) request_ai_refresh();

    if ((int32_t)(millis() - next_fetch) >= 0) {
      next_fetch = millis() + 1000;
      if (n_nets) {
        fetch();
      }
      ui_present(&st, ai, st.valid && millis() - last_ok_ms < 5000, last_err);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
