#include "ui.h"

#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_lvgl_port.h"
#include "gt911.h"
#include "rgb_lcd_port.h"

#define BG 0x081018
#define PANEL 0x12202B
#define TEXT 0xE7ECF0
#define DIM 0x8999A6
#define BLUE 0x489EFF
#define PURPLE 0xA377F5
#define HOT 0xFF6040

static lv_obj_t *screens[3]; // app launcher, system, AI usage
static lv_obj_t *online_label, *status_label, *cpu_label, *memory_label, *details_label;
static lv_obj_t *process_labels[5], *core_bars[16], *cpu_chart, *swap_chart;
static lv_chart_series_t *cpu_series, *swap_series;
static lv_obj_t *swap_label, *refresh_label, *ai_title[2], *ai_status[2];
static lv_obj_t *usage_bar[2][2], *usage_pct[2][2], *usage_reset[2][2];
static int active_screen;
static bool refresh_requested, brightness_requested;

static lv_color_t color(uint32_t hex) { return lv_color_hex(hex); }

static lv_obj_t *label(lv_obj_t *parent, const char *value, int x, int y,
                       const lv_font_t *font, uint32_t ink) {
    lv_obj_t *obj = lv_label_create(parent);
    lv_label_set_text(obj, value);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_style_text_font(obj, font, 0);
    lv_obj_set_style_text_color(obj, color(ink), 0);
    return obj;
}

static void enter(int screen) {
    active_screen = screen;
    lv_screen_load(screens[screen]);
}

static void on_enter(lv_event_t *event) { enter(1); }
static void on_home(lv_event_t *event) { enter(0); }
static void on_refresh(lv_event_t *event) { refresh_requested = true; }
static void on_brightness(lv_event_t *event) { brightness_requested = true; }

static void on_gesture(lv_event_t *event) {
    if (active_screen == 0) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT && active_screen == 1) enter(2);
    else if (dir == LV_DIR_RIGHT && active_screen == 2) enter(1);
}

static lv_obj_t *base_screen(void) {
    lv_obj_t *screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(screen, color(BG), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(screen, 0, 0);
    lv_obj_set_scrollable(screen, false);
    lv_obj_add_event_cb(screen, on_brightness, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(screen, on_gesture, LV_EVENT_GESTURE, NULL);
    return screen;
}

// The hit target is 52x48, but its only visible pixels are the white house glyph.
static void add_home(lv_obj_t *screen) {
    lv_obj_t *hit = lv_obj_create(screen);
    lv_obj_set_size(hit, 52, 48);
    lv_obj_set_pos(hit, 12, 4);
    lv_obj_set_style_bg_opa(hit, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(hit, 0, 0);
    lv_obj_set_style_shadow_width(hit, 0, 0);
    lv_obj_set_style_pad_all(hit, 0, 0);
    lv_obj_set_scrollable(hit, false);
    lv_obj_add_event_cb(hit, on_home, LV_EVENT_CLICKED, NULL);
    lv_obj_set_gesture_bubble(hit, true);
    lv_obj_t *icon = lv_label_create(hit);
    lv_label_set_text(icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(icon, color(0xFFFFFF), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_center(icon);
}

static lv_obj_t *card(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, color(PANEL), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 12, 0);
    lv_obj_set_scrollable(obj, false);
    lv_obj_set_gesture_bubble(obj, true);
    return obj;
}

static void add_dots(lv_obj_t *screen, int selected) {
    label(screen, selected == 0 ? "●  ○" : "○  ●", 376, 455, &lv_font_montserrat_12, TEXT);
}

static void create_launcher(void) {
    lv_obj_t *s = screens[0] = base_screen();
    label(s, "APPS", 56, 30, &lv_font_montserrat_48, TEXT);
    label(s, "Your desk, at a glance", 58, 94, &lv_font_montserrat_18, DIM);
    lv_obj_t *tile = card(s, 56, 128, 196, 232);
    lv_obj_add_event_cb(tile, on_enter, LV_EVENT_CLICKED, NULL);
    lv_obj_t *art = card(tile, 8, 4, 152, 152);
    lv_obj_set_style_bg_color(art, color(0x143356), 0);
    lv_obj_set_clickable(art, false);
    for (int i = 0; i < 3; ++i) {
        int heights[] = {44, 69, 93};
        lv_obj_t *bar = lv_obj_create(art);
        lv_obj_set_pos(bar, 23 + i * 36, 116 - heights[i]);
        lv_obj_set_size(bar, 22, heights[i]);
        lv_obj_set_style_bg_color(bar, color(i == 2 ? PURPLE : BLUE), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_scrollable(bar, false);
        lv_obj_set_clickable(bar, false);
    }
    label(tile, "MONITOR", 7, 171, &lv_font_montserrat_24, TEXT);
    label(tile, "SYSTEM + AI", 8, 203, &lv_font_montserrat_12, DIM);
    label(s, "Tap an app to open", 56, 430, &lv_font_montserrat_18, DIM);
    online_label = label(s, "MAC OFFLINE", 625, 435, &lv_font_montserrat_12, DIM);
}

static lv_obj_t *new_chart(lv_obj_t *parent, int x, int y, int h, uint32_t ink,
                           lv_chart_series_t **series) {
    lv_obj_t *chart = lv_chart_create(parent);
    lv_obj_set_pos(chart, x, y);
    lv_obj_set_size(chart, 478, h);
    lv_obj_set_style_bg_color(chart, color(PANEL), 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_radius(chart, 7, 0);
    lv_obj_set_style_line_color(chart, color(0x28404C), LV_PART_MAIN);
    lv_obj_set_style_line_opa(chart, LV_OPA_40, LV_PART_MAIN);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_size(chart, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_gesture_bubble(chart, true);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(chart, 120);
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
    lv_chart_set_div_line_count(chart, 3, 0);
    *series = lv_chart_add_series(chart, color(ink), LV_CHART_AXIS_PRIMARY_Y);
    return chart;
}

static void create_system(void) {
    lv_obj_t *s = screens[1] = base_screen();
    add_home(s);
    status_label = label(s, "WAITING FOR MAC", 124, 17, &lv_font_montserrat_24, DIM);
    label(s, "CPU", 16, 72, &lv_font_montserrat_18, DIM);
    cpu_label = label(s, "--%", 16, 93, &lv_font_montserrat_48, TEXT);
    label(s, "APP DATA vs RAM", 16, 176, &lv_font_montserrat_18, DIM);
    memory_label = label(s, "--", 16, 202, &lv_font_montserrat_24, TEXT);
    details_label = label(s, "Waiting for data", 16, 237, &lv_font_montserrat_12, DIM);
    label(s, "TOP", 16, 263, &lv_font_montserrat_18, DIM);
    for (int i = 0; i < 5; ++i)
        process_labels[i] = label(s, "", 16, 286 + i * 34, &lv_font_montserrat_12, TEXT);
    label(s, "CPU  last 2 min", 302, 66, &lv_font_montserrat_12, DIM);
    cpu_chart = new_chart(s, 302, 84, 169, BLUE, &cpu_series);
    label(s, "100", 265, 86, &lv_font_montserrat_12, DIM);
    label(s, "0", 280, 235, &lv_font_montserrat_12, DIM);
    for (int i = 0; i < 16; ++i) {
        core_bars[i] = lv_bar_create(s);
        lv_obj_set_pos(core_bars[i], 302 + i * 30, 262);
        lv_obj_set_size(core_bars[i], 23, 34);
        lv_bar_set_range(core_bars[i], 0, 100);
        lv_bar_set_value(core_bars[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(core_bars[i], color(PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(core_bars[i], color(BLUE), LV_PART_INDICATOR);
        lv_obj_set_gesture_bubble(core_bars[i], true);
    }
    swap_label = label(s, "SWAP I/O  (0 = healthy)", 302, 307, &lv_font_montserrat_12, DIM);
    swap_chart = new_chart(s, 302, 327, 130, PURPLE, &swap_series);
    add_dots(s, 0);
}

static void fmt_dur(char *dst, size_t size, int seconds) {
    if (seconds < 0) snprintf(dst, size, "reset time unknown");
    else if (seconds >= 86400) snprintf(dst, size, "resets in %dd %dh", seconds / 86400, seconds % 86400 / 3600);
    else if (seconds >= 3600) snprintf(dst, size, "resets in %dh %dm", seconds / 3600, seconds % 3600 / 60);
    else snprintf(dst, size, "resets in %dm", (seconds + 59) / 60);
}

static void create_usage_section(lv_obj_t *s, int k, int y, const char *title) {
    ai_title[k] = label(s, title, 16, y, &lv_font_montserrat_24, TEXT);
    ai_status[k] = label(s, "waiting", 530, y + 8, &lv_font_montserrat_12, DIM);
    for (int row = 0; row < 2; ++row) {
        int ry = y + 42 + row * 72;
        label(s, row ? "WEEKLY" : "5 HOUR", 16, ry + 8, &lv_font_montserrat_18, DIM);
        usage_bar[k][row] = lv_bar_create(s);
        lv_obj_set_pos(usage_bar[k][row], 130, ry);
        lv_obj_set_size(usage_bar[k][row], 500, 32);
        lv_bar_set_range(usage_bar[k][row], 0, 100);
        lv_bar_set_value(usage_bar[k][row], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(usage_bar[k][row], color(PANEL), LV_PART_MAIN);
        lv_obj_set_style_bg_color(usage_bar[k][row], color(BLUE), LV_PART_INDICATOR);
        lv_obj_set_gesture_bubble(usage_bar[k][row], true);
        usage_pct[k][row] = label(s, "--", 650, ry + 2, &lv_font_montserrat_24, TEXT);
        usage_reset[k][row] = label(s, "", 130, ry + 38, &lv_font_montserrat_12, DIM);
    }
}

static void create_ai(void) {
    lv_obj_t *s = screens[2] = base_screen();
    add_home(s);
    label(s, "AI USAGE", 124, 16, &lv_font_montserrat_24, TEXT);
    lv_obj_t *button = card(s, 600, 7, 184, 48);
    lv_obj_set_style_bg_color(button, color(BLUE), 0);
    lv_obj_add_event_cb(button, on_refresh, LV_EVENT_CLICKED, NULL);
    refresh_label = label(button, "REFRESH", 24, 2, &lv_font_montserrat_24, 0xFFFFFF);
    create_usage_section(s, 0, 72, "CLAUDE CODE");
    create_usage_section(s, 1, 264, "CODEX");
    add_dots(s, 1);
}

static void read_touch(lv_indev_t *indev, lv_indev_data_t *data) {
    touch_gt911_point_t touch = touch_gt911_read_point(1);
    data->state = touch.cnt ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    if (touch.cnt) {
        data->point.x = touch.x[0];
        data->point.y = touch.y[0];
    }
}

void ui_init(esp_lcd_panel_handle_t panel) {
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));
    const lvgl_port_display_cfg_t display_cfg = {
        .panel_handle = panel, .hres = EXAMPLE_LCD_H_RES, .vres = EXAMPLE_LCD_V_RES,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES, .double_buffer = true,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = { .full_refresh = true },
    };
    const lvgl_port_display_rgb_cfg_t rgb_cfg = { .flags = { .bb_mode = true, .avoid_tearing = true } };
    lv_display_t *display = lvgl_port_add_disp_rgb(&display_cfg, &rgb_cfg);
    ESP_ERROR_CHECK(display ? ESP_OK : ESP_ERR_NO_MEM);
    lvgl_port_lock(0);
    create_launcher();
    create_system();
    create_ai();
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, read_touch);
    lv_indev_set_display(input, display);
    enter(0);
    lvgl_port_unlock();
}

static void update_chart(lv_obj_t *chart, lv_chart_series_t *series, const uint8_t *data, int n) {
    if (n < 0) n = 0;
    if (n > 120) n = 120;
    for (int i = 0; i < 120; ++i)
        lv_chart_set_value_by_id(chart, series, i, i < 120 - n ? LV_CHART_POINT_NONE : data[i - (120 - n)]);
    lv_chart_refresh(chart);
}

void ui_present(const stats_t *stats, const ai_t ai[2], bool mac_online, const char *last_error) {
    if (!lvgl_port_lock(100)) return;
    char line[120];
    lv_label_set_text(online_label, mac_online ? "MAC ONLINE" : "MAC OFFLINE");
    lv_obj_set_style_text_color(online_label, color(mac_online ? 0x37D790 : 0xE3B35F), 0);
    const char *banner = !stats->valid ? "WAITING FOR MAC" : stats->mp >= 4 ? "MEMORY CRITICAL - STOP" :
                         stats->mp >= 2 ? "MEMORY WARNING" : "MEMORY OK";
    snprintf(line, sizeof line, "%s%s%s", banner, mac_online ? "" : "  /  ", mac_online ? "" : last_error);
    lv_label_set_text(status_label, line);
    lv_obj_set_style_text_color(status_label, color(stats->mp >= 4 ? HOT : stats->mp >= 2 ? 0xE3B35F : DIM), 0);
    snprintf(line, sizeof line, "%d%%  load %.2f", stats->cpu, stats->load);
    lv_label_set_text(cpu_label, line);
    snprintf(line, sizeof line, "%.1fG / %.0fG", stats->mdemand, stats->mtot);
    lv_label_set_text(memory_label, line);
    snprintf(line, sizeof line, "zip %.1fG -> %.1fG\nram %.1fG  swap %.1fG", stats->mstored, stats->mcomp, stats->mused, stats->swap);
    lv_label_set_text(details_label, line);
    for (int i = 0; i < 5; ++i) {
        if (i < stats->ntop) snprintf(line, sizeof line, "%.19s  %d%%  %dM", stats->top[i].name, stats->top[i].cpu, stats->top[i].rss);
        else line[0] = 0;
        lv_label_set_text(process_labels[i], line);
    }
    for (int i = 0; i < 16; ++i) {
        lv_obj_set_style_opa(core_bars[i], i < stats->ncores ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        if (i < stats->ncores) lv_bar_set_value(core_bars[i], stats->cores[i], LV_ANIM_OFF);
    }
    update_chart(cpu_chart, cpu_series, stats->hcpu, stats->nh);
    int peak = 0;
    for (int i = 0; i < stats->nh && i < 120; ++i) if (stats->hsw[i] > peak) peak = stats->hsw[i];
    static const int ceilings[] = {10, 20, 50, 100, 200, 500, 1000, 2000, 5000};
    int ceiling = 5000;
    for (int i = 0; i < 9; ++i) if (peak <= ceilings[i]) { ceiling = ceilings[i]; break; }
    uint8_t swap_values[120] = {0};
    for (int i = 0; i < stats->nh && i < 120; ++i) {
        int pct = stats->hsw[i] * 100 / ceiling;
        swap_values[i] = pct > 100 ? 100 : pct;
    }
    update_chart(swap_chart, swap_series, swap_values, stats->nh);
    snprintf(line, sizeof line, "SWAP I/O  now %.0f MB/s  (0 = healthy; scale %d)", stats->swapr, ceiling);
    lv_label_set_text(swap_label, line);
    lv_label_set_text(refresh_label, ai[0].busy || ai[1].busy ? "UPDATING" : "REFRESH");
    for (int k = 0; k < 2; ++k) {
        snprintf(line, sizeof line, "%s%s%s", k ? "CODEX" : "CLAUDE CODE", ai[k].plan[0] ? "  /  " : "", ai[k].plan);
        lv_label_set_text(ai_title[k], line);
        if (ai[k].err[0]) snprintf(line, sizeof line, "%.36s", ai[k].err);
        else if (ai[k].age >= 0) snprintf(line, sizeof line, "updated %dm ago", ai[k].age / 60);
        else snprintf(line, sizeof line, "waiting");
        lv_label_set_text(ai_status[k], line);
        for (int row = 0; row < 2; ++row) {
            int pct = row ? ai[k].wk : ai[k].h5;
            int reset = row ? ai[k].wkr : ai[k].h5r;
            lv_bar_set_value(usage_bar[k][row], pct < 0 ? 0 : pct > 100 ? 100 : pct, LV_ANIM_OFF);
            lv_obj_set_style_bg_color(usage_bar[k][row], color(pct >= 90 ? HOT : pct >= 70 ? 0xFEB340 : BLUE), LV_PART_INDICATOR);
            if (pct < 0) snprintf(line, sizeof line, "--");
            else snprintf(line, sizeof line, "%d%%", pct);
            lv_label_set_text(usage_pct[k][row], line);
            fmt_dur(line, sizeof line, reset);
            lv_label_set_text(usage_reset[k][row], line);
        }
    }
    lvgl_port_unlock();
}

bool ui_take_refresh_request(void) {
    if (!lvgl_port_lock(100)) return false;
    bool result = refresh_requested;
    refresh_requested = false;
    lvgl_port_unlock();
    return result;
}

bool ui_take_brightness_request(void) {
    if (!lvgl_port_lock(100)) return false;
    bool result = brightness_requested;
    brightness_requested = false;
    lvgl_port_unlock();
    return result;
}
