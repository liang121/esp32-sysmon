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
static lv_obj_t *status_label, *ai_offline_label, *cpu_label, *load_label, *memory_label, *details_label;
static lv_obj_t *process_names[5], *process_values[5], *core_bars[16], *core_indices[16];
static lv_obj_t *core_count_label, *cpu_chart, *swap_chart;
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
    label(screen, selected == 0 ? "●  ○" : "○  ●", 376, 459, &lv_font_montserrat_12, TEXT);
}

static void create_launcher(void) {
    lv_obj_t *s = screens[0] = base_screen();
    label(s, "APPS", 32, 29, &lv_font_montserrat_32, TEXT);
    lv_obj_t *app = lv_obj_create(s);
    lv_obj_set_pos(app, 27, 96);
    lv_obj_set_size(app, 112, 140);
    lv_obj_set_style_bg_opa(app, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(app, 0, 0);
    lv_obj_set_style_shadow_width(app, 0, 0);
    lv_obj_set_style_pad_all(app, 0, 0);
    lv_obj_set_scrollable(app, false);
    lv_obj_add_event_cb(app, on_enter, LV_EVENT_CLICKED, NULL);
    lv_obj_t *icon = card(app, 13, 12, 86, 86);
    lv_obj_set_style_bg_color(icon, color(0x143356), 0);
    lv_obj_set_style_radius(icon, 18, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_set_clickable(icon, false);
    for (int i = 0; i < 3; ++i) {
        const int heights[] = {24, 40, 56};
        lv_obj_t *bar = lv_obj_create(icon);
        lv_obj_set_pos(bar, 18 + i * 22, 70 - heights[i]);
        lv_obj_set_size(bar, 14, heights[i]);
        lv_obj_set_style_bg_color(bar, color(i == 2 ? PURPLE : BLUE), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_radius(bar, 3, 0);
        lv_obj_set_scrollable(bar, false);
        lv_obj_set_clickable(bar, false);
    }
    lv_obj_t *name = label(app, "Monitor", 0, 108, &lv_font_montserrat_18, TEXT);
    lv_obj_align(name, LV_ALIGN_TOP_MID, 0, 108);
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
    label(s, "SYSTEM", 80, 17, &lv_font_montserrat_24, TEXT);
    status_label = label(s, "WAITING FOR MAC", 440, 24, &lv_font_montserrat_12, DIM);
    lv_obj_set_width(status_label, 340);
    lv_label_set_long_mode(status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_t *cpu_panel = card(s, 16, 72, 246, 98);
    lv_obj_t *memory_panel = card(s, 16, 180, 246, 100);
    lv_obj_t *process_panel = card(s, 16, 290, 246, 154);
    lv_obj_set_clickable(cpu_panel, false);
    lv_obj_set_clickable(memory_panel, false);
    lv_obj_set_clickable(process_panel, false);
    label(s, "CPU", 30, 83, &lv_font_montserrat_12, DIM);
    cpu_label = label(s, "--%", 30, 105, &lv_font_montserrat_32, TEXT);
    load_label = label(s, "load --", 145, 133, &lv_font_montserrat_12, DIM);
    label(s, "APP DATA vs RAM", 30, 189, &lv_font_montserrat_12, DIM);
    memory_label = label(s, "--", 30, 210, &lv_font_montserrat_24, TEXT);
    details_label = label(s, "Waiting for data", 30, 242, &lv_font_montserrat_12, DIM);
    label(s, "TOP PROCESSES", 30, 299, &lv_font_montserrat_12, DIM);
    for (int i = 0; i < 5; ++i) {
        process_names[i] = label(s, "", 30, 321 + i * 23, &lv_font_montserrat_12, BLUE);
        lv_obj_set_width(process_names[i], 128);
        lv_label_set_long_mode(process_names[i], LV_LABEL_LONG_DOT);
        process_values[i] = label(s, "", 161, 321 + i * 23, &lv_font_montserrat_12, DIM);
        lv_obj_set_width(process_values[i], 87);
        lv_obj_set_style_text_align(process_values[i], LV_TEXT_ALIGN_RIGHT, 0);
    }
    label(s, "CPU  /  last 2 min", 302, 68, &lv_font_montserrat_12, DIM);
    cpu_chart = new_chart(s, 302, 90, 130, BLUE, &cpu_series);
    label(s, "100", 267, 91, &lv_font_montserrat_12, DIM);
    label(s, "0", 282, 200, &lv_font_montserrat_12, DIM);
    lv_obj_t *cores_panel = card(s, 302, 233, 478, 108);
    lv_obj_set_style_bg_color(cores_panel, color(0x1A2E3B), 0);
    lv_obj_set_clickable(cores_panel, false);
    label(s, "CPU cores", 318, 238, &lv_font_montserrat_12, TEXT);
    core_count_label = label(s, "0 active", 662, 238, &lv_font_montserrat_12, DIM);
    lv_obj_set_width(core_count_label, 102);
    lv_obj_set_style_text_align(core_count_label, LV_TEXT_ALIGN_RIGHT, 0);
    for (int i = 0; i < 16; ++i) {
        core_bars[i] = lv_bar_create(s);
        lv_obj_set_pos(core_bars[i], 302 + i * 30, 265);
        lv_obj_set_size(core_bars[i], 20, 52);
        lv_bar_set_orientation(core_bars[i], LV_BAR_ORIENTATION_VERTICAL);
        lv_bar_set_range(core_bars[i], 0, 100);
        lv_bar_set_value(core_bars[i], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(core_bars[i], color(0x294356), LV_PART_MAIN);
        lv_obj_set_style_bg_color(core_bars[i], color(BLUE), LV_PART_INDICATOR);
        lv_obj_set_style_radius(core_bars[i], 3, LV_PART_MAIN);
        lv_obj_set_style_radius(core_bars[i], 2, LV_PART_INDICATOR);
        lv_obj_set_gesture_bubble(core_bars[i], true);
        core_indices[i] = label(s, "", 0, 317, &lv_font_montserrat_12, DIM);
        lv_obj_set_style_text_align(core_indices[i], LV_TEXT_ALIGN_CENTER, 0);
    }
    swap_label = label(s, "SWAP I/O  (0 = healthy)", 302, 347, &lv_font_montserrat_12, DIM);
    swap_chart = new_chart(s, 302, 367, 77, PURPLE, &swap_series);
    add_dots(s, 0);
}

static void fmt_dur(char *dst, size_t size, int seconds) {
    if (seconds < 0) snprintf(dst, size, "reset time unknown");
    else if (seconds >= 86400) snprintf(dst, size, "resets in %dd %dh", seconds / 86400, seconds % 86400 / 3600);
    else if (seconds >= 3600) snprintf(dst, size, "resets in %dh %dm", seconds / 3600, seconds % 3600 / 60);
    else snprintf(dst, size, "resets in %dm", (seconds + 59) / 60);
}

static void create_usage_section(lv_obj_t *s, int k, int y, const char *title) {
    lv_obj_t *section = card(s, 16, y, 768, 184);
    lv_obj_set_clickable(section, false);
    ai_title[k] = label(s, title, 30, y + 9, &lv_font_montserrat_24, TEXT);
    ai_status[k] = label(s, "waiting", 516, y + 18, &lv_font_montserrat_12, DIM);
    lv_obj_set_width(ai_status[k], 252);
    lv_label_set_long_mode(ai_status[k], LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(ai_status[k], LV_TEXT_ALIGN_RIGHT, 0);
    for (int row = 0; row < 2; ++row) {
        int ry = y + 46 + row * 72;
        label(s, row ? "WEEKLY" : "5 HOUR", 30, ry + 8, &lv_font_montserrat_18, DIM);
        usage_bar[k][row] = lv_bar_create(s);
        lv_obj_set_pos(usage_bar[k][row], 142, ry);
        lv_obj_set_size(usage_bar[k][row], 480, 32);
        lv_bar_set_range(usage_bar[k][row], 0, 100);
        lv_bar_set_value(usage_bar[k][row], 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(usage_bar[k][row], color(0x2B3C49), LV_PART_MAIN);
        lv_obj_set_style_bg_color(usage_bar[k][row], color(BLUE), LV_PART_INDICATOR);
        lv_obj_set_gesture_bubble(usage_bar[k][row], true);
        usage_pct[k][row] = label(s, "--", 650, ry + 2, &lv_font_montserrat_24, TEXT);
        usage_reset[k][row] = label(s, "", 142, ry + 38, &lv_font_montserrat_12, DIM);
    }
}

static void create_ai(void) {
    lv_obj_t *s = screens[2] = base_screen();
    add_home(s);
    label(s, "AI USAGE", 80, 17, &lv_font_montserrat_24, TEXT);
    ai_offline_label = label(s, "", 318, 25, &lv_font_montserrat_12, HOT);
    lv_obj_t *button = card(s, 630, 8, 154, 44);
    lv_obj_set_style_bg_color(button, color(BLUE), 0);
    lv_obj_add_event_cb(button, on_refresh, LV_EVENT_CLICKED, NULL);
    refresh_label = label(button, "REFRESH", 0, 0, &lv_font_montserrat_18, 0xFFFFFF);
    lv_obj_align(refresh_label, LV_ALIGN_CENTER, 0, 0);
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
    lv_label_set_text(ai_offline_label, mac_online ? "" : "Mac offline");
    const char *banner = !stats->valid ? "WAITING FOR MAC" : stats->mp >= 4 ? "MEMORY CRITICAL - STOP" :
                         stats->mp >= 2 ? "MEMORY WARNING" : "MEMORY OK";
    snprintf(line, sizeof line, "%s%s%s", banner, mac_online ? "" : "  /  ", mac_online ? "" : last_error);
    lv_label_set_text(status_label, line);
    lv_obj_set_style_text_color(status_label, color(stats->mp >= 4 ? HOT : stats->mp >= 2 ? 0xE3B35F : DIM), 0);
    snprintf(line, sizeof line, "%d%%", stats->cpu);
    lv_label_set_text(cpu_label, line);
    snprintf(line, sizeof line, "load %.2f", stats->load);
    lv_label_set_text(load_label, line);
    snprintf(line, sizeof line, "%.1fG / %.0fG", stats->mdemand, stats->mtot);
    lv_label_set_text(memory_label, line);
    snprintf(line, sizeof line, "zip %.1fG -> %.1fG\nram %.1fG  swap %.1fG", stats->mstored, stats->mcomp, stats->mused, stats->swap);
    lv_label_set_text(details_label, line);
    for (int i = 0; i < 5; ++i) {
        lv_label_set_text(process_names[i], i < stats->ntop ? stats->top[i].name : "");
        if (i < stats->ntop) snprintf(line, sizeof line, "%d%% %dM", stats->top[i].cpu, stats->top[i].rss);
        else line[0] = 0;
        lv_label_set_text(process_values[i], line);
    }
    int ncores = stats->ncores > 16 ? 16 : stats->ncores;
    if (ncores < 0) ncores = 0;
    snprintf(line, sizeof line, "%d active", ncores);
    lv_label_set_text(core_count_label, line);
    for (int i = 0; i < 16; ++i) {
        bool visible = i < ncores;
        lv_obj_set_hidden(core_bars[i], !visible);
        lv_obj_set_hidden(core_indices[i], !visible);
        if (visible) {
            int slot_left = 314 + 454 * i / ncores;
            int slot_right = 314 + 454 * (i + 1) / ncores;
            int slot_width = slot_right - slot_left;
            int bar_width = slot_width - 9 < 36 ? slot_width - 9 : 36;
            lv_obj_set_pos(core_bars[i], slot_left + (slot_width - bar_width) / 2, 265);
            lv_obj_set_width(core_bars[i], bar_width);
            lv_bar_set_value(core_bars[i], stats->cores[i], LV_ANIM_OFF);
            snprintf(line, sizeof line, "%d", i);
            lv_label_set_text(core_indices[i], line);
            lv_obj_set_pos(core_indices[i], slot_left, 318);
            lv_obj_set_width(core_indices[i], slot_width);
        }
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
    snprintf(line, sizeof line, "SWAP I/O  now %.1f MB/s  (0 = healthy; scale %d)", stats->swapr, ceiling);
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
