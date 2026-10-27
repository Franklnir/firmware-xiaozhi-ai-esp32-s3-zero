#include "chronchi_display.h"
#include "chronchi_ble.h"
#include "chronchi_alert.h"
#include <esp_lvgl_port.h>
#include <esp_log.h>
#include <settings.h>
#include <cstring>
#include <cstdio>
#include <cmath>

#undef lv_lock
#undef lv_unlock
#define lv_lock() lvgl_port_lock(0)
#define lv_unlock() lvgl_port_unlock()

#define TAG "ChronchiDisplay"

LV_FONT_DECLARE(lv_font_montserrat_48);
LV_FONT_DECLARE(lv_font_montserrat_24);

struct BackgroundTheme {
    uint32_t bg;
    uint32_t card_bg;
    uint32_t card_border;
    uint32_t text;
    uint32_t dim;
    uint32_t accent;
    const char* name;
};

// Preset background themes selectable from Chronos app or physical button
static const BackgroundTheme PRESET_THEMES[] = {
    // 0: Classic Black (Hitam Polos Standar Smartwatch)
    { 0x000000, 0x111827, 0x1F2937, 0xFFFFFF, 0x94A3B8, 0x00E5FF, "Classic Black" },
    // 1: Pure White (Putih Bersih)
    { 0xFFFFFF, 0xF1F5F9, 0xCBD5E1, 0x0F172A, 0x64748B, 0x0284C7, "Pure White" },
    // 2: Midnight Navy (Biru Malam Gelap)
    { 0x070D1F, 0x0F1C3F, 0x1E3A8A, 0x38BDF8, 0x7DD3FC, 0x00F0FF, "Midnight Navy" },
    // 3: Deep Emerald (Hijau Zamrud Gelap)
    { 0x03180E, 0x062E1C, 0x047857, 0x34D399, 0xA7F3D0, 0x10B981, "Deep Emerald" },
    // 4: Sunset Crimson (Merah Gelap Elegan)
    { 0x1A0505, 0x2E0B0B, 0x7F1D1D, 0xFB923C, 0xFCA5A5, 0xF97316, "Sunset Crimson" },
    // 5: Dark Amethyst (Ungu Gelap Mewah)
    { 0x0F051D, 0x200C3B, 0x581C87, 0xC084FC, 0xE9D5FF, 0xA855F7, "Dark Amethyst" },
    // 6: Warm Cream (Krem Hangat Minimalis)
    { 0xF7F4EB, 0xEBE6D8, 0xD6CEBA, 0x1C1917, 0x78716C, 0xC2410C, "Warm Cream" },
};
static constexpr int NUM_THEMES = sizeof(PRESET_THEMES) / sizeof(PRESET_THEMES[0]);

static lv_obj_t* create_card(lv_obj_t* parent, int32_t w, int32_t h, int32_t radius) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_radius(card, radius, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_pad_all(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

ChronchiDisplay::ChronchiDisplay() = default;
ChronchiDisplay::~ChronchiDisplay() {
    if (nav_icon_buf_) { free(nav_icon_buf_); nav_icon_buf_ = nullptr; }
}

void ChronchiDisplay::init(int width, int height) {
    width_ = width;
    height_ = height;
    ESP_LOGI(TAG, "init %dx%d (Borderless Chronchi Watchface)", width_, height_);

    // Load saved watchface theme and color from NVS
    Settings settings("chronchi", true);
    current_layout_ = settings.GetInt("layout", 0) % NUM_THEMES;
    theme_color_ = settings.GetInt("color", 0x00E5FF);
    light_mode_ = (current_layout_ == 1 || current_layout_ == 6);
    ESP_LOGI(TAG, "Initial watchface: layout=%lu (%s), color=0x%06lX",
             (unsigned long)current_layout_, PRESET_THEMES[current_layout_].name, (unsigned long)theme_color_);

    lv_lock();

    // Allocate navigation icon buffer (48x48 RGB565)
    nav_icon_buf_ = (uint8_t*)heap_caps_malloc(48 * 48 * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!nav_icon_buf_) nav_icon_buf_ = (uint8_t*)malloc(48 * 48 * 2);
    if (nav_icon_buf_) {
        memset(nav_icon_buf_, light_mode_ ? 0xFF : 0x00, 48 * 48 * 2);
        memset(&nav_icon_dsc_, 0, sizeof(nav_icon_dsc_));
        nav_icon_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        nav_icon_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        nav_icon_dsc_.header.w = 48;
        nav_icon_dsc_.header.h = 48;
        nav_icon_dsc_.header.stride = 48 * 2;
        nav_icon_dsc_.data_size = 48 * 48 * 2;
        nav_icon_dsc_.data = nav_icon_buf_;
    }

    createHomeScreen();
    createNavScreen();
    createNotifScreen();
    createWeatherScreen();
    createMusicScreen();
    createStatusScreen();
    createAlertScreen();

    applyColorMode();
    showHome();

    lv_unlock();
}

// Convert 24-bit RGB hex to RGB565 for seamless canvas blending
static inline uint16_t hex_to_rgb565(uint32_t hex) {
    uint8_t r = (hex >> 16) & 0xFF;
    uint8_t g = (hex >> 8) & 0xFF;
    uint8_t b = hex & 0xFF;
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---- NAVIGATION ICON DRAWING (Google Maps style) ----
static inline void put_pixel(uint8_t* buf, int x, int y, uint16_t color) {
    if (x < 0 || x >= 48 || y < 0 || y >= 48) return;
    int idx = (y * 48 + x) * 2;
    buf[idx] = color & 0xFF;
    buf[idx + 1] = (color >> 8) & 0xFF;
}

static void draw_filled_circle(uint8_t* buf, int cx, int cy, int r, uint16_t color) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx*dx + dy*dy <= r*r)
                put_pixel(buf, cx+dx, cy+dy, color);
}

// Modern arrow up
static void draw_arrow_up(uint8_t* buf, uint16_t color) {
    for (int y = 24; y <= 42; y++)
        for (int x = 21; x <= 26; x++)
            put_pixel(buf, x, y, color);
    draw_filled_circle(buf, 24, 42, 3, color);
    for (int i = 0; i < 16; i++) {
        int half = 2 + (i * 8) / 16;
        for (int x = 24 - half; x <= 24 + half; x++)
            put_pixel(buf, x, 6 + i, color);
    }
}

// Modern arrow left
static void draw_arrow_left(uint8_t* buf, uint16_t color) {
    for (int y = 21; y <= 26; y++)
        for (int x = 12; x <= 34; x++)
            put_pixel(buf, x, y, color);
    draw_filled_circle(buf, 34, 24, 3, color);
    for (int i = 0; i < 16; i++) {
        int half = 2 + (i * 8) / 16;
        for (int y = 24 - half; y <= 24 + half; y++)
            put_pixel(buf, 6 + i, y, color);
    }
}

// Modern arrow right
static void draw_arrow_right(uint8_t* buf, uint16_t color) {
    for (int y = 21; y <= 26; y++)
        for (int x = 13; x <= 35; x++)
            put_pixel(buf, x, y, color);
    draw_filled_circle(buf, 13, 24, 3, color);
    for (int i = 0; i < 16; i++) {
        int half = 2 + (i * 8) / 16;
        for (int y = 24 - half; y <= 24 + half; y++)
            put_pixel(buf, 41 - i, y, color);
    }
}

// Modern slight left
static void draw_arrow_slight_left(uint8_t* buf, uint16_t color) {
    for (int i = 0; i < 20; i++) {
        int x = 32 - i;
        int y = 34 - i;
        for (int d = -2; d <= 2; d++) {
            put_pixel(buf, x + d, y, color);
            put_pixel(buf, x, y + d, color);
        }
    }
    draw_filled_circle(buf, 32, 34, 3, color);
    for (int i = 0; i < 12; i++) {
        int half = 2 + (i * 5) / 12;
        int cx = 12 - i/2, cy = 14 - i/2;
        for (int d = -half; d <= half; d++)
            put_pixel(buf, cx + d, cy, color);
    }
}

// Modern slight right
static void draw_arrow_slight_right(uint8_t* buf, uint16_t color) {
    for (int i = 0; i < 20; i++) {
        int x = 15 + i;
        int y = 34 - i;
        for (int d = -2; d <= 2; d++) {
            put_pixel(buf, x + d, y, color);
            put_pixel(buf, x, y + d, color);
        }
    }
    draw_filled_circle(buf, 15, 34, 3, color);
    for (int i = 0; i < 12; i++) {
        int half = 2 + (i * 5) / 12;
        int cx = 35 + i/2, cy = 14 - i/2;
        for (int d = -half; d <= half; d++)
            put_pixel(buf, cx + d, cy, color);
    }
}

// Modern roundabout
static void draw_roundabout(uint8_t* buf, uint16_t color) {
    int r = 14;
    for (int a = 0; a < 360; a += 2) {
        float rad = a * 3.14159f / 180.0f;
        for (int t = -1; t <= 1; t++) {
            int x = 24 + (int)((r + t) * cosf(rad));
            int y = 24 + (int)((r + t) * sinf(rad));
            put_pixel(buf, x, y, color);
        }
    }
    for (int y = 21; y <= 26; y++)
        for (int x = 30; x <= 42; x++)
            put_pixel(buf, x, y, color);
    for (int i = 0; i < 8; i++) {
        int half = 1 + (i * 3) / 8;
        for (int y = 24 - half; y <= 24 + half; y++)
            put_pixel(buf, 42 - i, y, color);
    }
}

// Modern arrive (Google Maps pin)
static void draw_arrive(uint8_t* buf, uint16_t color, uint16_t cutout) {
    draw_filled_circle(buf, 24, 14, 9, color);
    draw_filled_circle(buf, 24, 14, 4, cutout);
    for (int i = 0; i < 16; i++) {
        int half = 9 - (i * 6) / 16;
        for (int x = 24 - half; x <= 24 + half; x++)
            put_pixel(buf, x, 23 + i, color);
    }
    put_pixel(buf, 24, 39, color);
    put_pixel(buf, 24, 40, color);
}

// Modern car icon
static void draw_car(uint8_t* buf, uint16_t color, uint16_t cutout) {
    for (int y = 24; y <= 36; y++)
        for (int x = 10; x <= 37; x++)
            put_pixel(buf, x, y, color);
    for (int y = 16; y <= 24; y++)
        for (int x = 15; x <= 32; x++)
            put_pixel(buf, x, y, color);
    for (int y = 17; y <= 23; y++)
        for (int x = 16; x <= 22; x++)
            put_pixel(buf, x, y, cutout);
    for (int y = 17; y <= 23; y++)
        for (int x = 25; x <= 31; x++)
            put_pixel(buf, x, y, cutout);
    draw_filled_circle(buf, 15, 37, 4, color);
    draw_filled_circle(buf, 32, 37, 4, color);
    put_pixel(buf, 10, 28, 0xFFE0);
    put_pixel(buf, 10, 29, 0xFFE0);
    put_pixel(buf, 37, 28, 0xFFE0);
    put_pixel(buf, 37, 29, 0xFFE0);
}

void ChronchiDisplay::setNavIcon(const char* maneuver) {
    if (!nav_icon_buf_) return;
    if (maneuver) last_maneuver_ = maneuver;
    const auto& th = PRESET_THEMES[current_layout_ % NUM_THEMES];
    uint16_t clear_color = hex_to_rgb565(th.bg);
    for (int i = 0; i < 48 * 48; i++) {
        nav_icon_buf_[i * 2] = clear_color & 0xFF;
        nav_icon_buf_[i * 2 + 1] = (clear_color >> 8) & 0xFF;
    }
    uint16_t fg = hex_to_rgb565(th.accent);
    if (fg == clear_color) {
        fg = light_mode_ ? 0x0290 : 0x07FF;
    }
    uint16_t cutout = clear_color;

    const char* m = last_maneuver_.c_str();
    if (strcmp(m, "STRAIGHT") == 0) draw_arrow_up(nav_icon_buf_, fg);
    else if (strcmp(m, "LEFT") == 0) draw_arrow_left(nav_icon_buf_, fg);
    else if (strcmp(m, "RIGHT") == 0) draw_arrow_right(nav_icon_buf_, fg);
    else if (strcmp(m, "SLIGHT_LEFT") == 0) draw_arrow_slight_left(nav_icon_buf_, fg);
    else if (strcmp(m, "SLIGHT_RIGHT") == 0) draw_arrow_slight_right(nav_icon_buf_, fg);
    else if (strcmp(m, "ROUNDABOUT") == 0) draw_roundabout(nav_icon_buf_, fg);
    else if (strcmp(m, "ARRIVE") == 0) draw_arrive(nav_icon_buf_, fg, cutout);
    else draw_car(nav_icon_buf_, fg, cutout);

    if (nav_icon_img_) lv_image_set_src(nav_icon_img_, &nav_icon_dsc_);
}

// ---- BORDERLESS AUTHENTIC CHRONCHI WATCHFACE ----
void ChronchiDisplay::createHomeScreen() {
    home_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(home_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(home_screen_, width_, height_);
    lv_obj_set_style_bg_opa(home_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(home_screen_, 0, 0);
    lv_obj_set_style_radius(home_screen_, 0, 0);
    lv_obj_set_style_pad_all(home_screen_, 0, 0);
    lv_obj_clear_flag(home_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // Top status pill (left): Bluetooth capsule
    home_bt_card_ = create_card(home_screen_, 36, 26, 13);
    lv_obj_align(home_bt_card_, LV_ALIGN_TOP_LEFT, 12, 10);
    home_bt_label_ = lv_label_create(home_bt_card_);
    lv_label_set_text(home_bt_label_, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(home_bt_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(home_bt_label_);

    // Top center branding: CHRONCHI
    home_chronchi_label_ = lv_label_create(home_screen_);
    lv_label_set_text(home_chronchi_label_, "CHRONCHI");
    lv_obj_set_style_text_font(home_chronchi_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(home_chronchi_label_, LV_ALIGN_TOP_MID, 0, 14);

    // Top status pill (right): Battery capsule with exact percentage and bar
    home_batt_card_ = create_card(home_screen_, 70, 26, 13);
    lv_obj_align(home_batt_card_, LV_ALIGN_TOP_RIGHT, -10, 10);
    lv_obj_set_flex_flow(home_batt_card_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(home_batt_card_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(home_batt_card_, 2, 0);
    lv_obj_set_style_pad_column(home_batt_card_, 2, 0);

    home_batt_label_ = lv_label_create(home_batt_card_);
    lv_label_set_text(home_batt_label_, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_font(home_batt_label_, &lv_font_montserrat_14, 0);
    lv_obj_add_flag(home_batt_label_, LV_OBJ_FLAG_HIDDEN);

    home_batt_pct_label_ = lv_label_create(home_batt_card_);
    lv_label_set_text(home_batt_pct_label_, "--%");
    lv_obj_set_style_text_font(home_batt_pct_label_, &lv_font_montserrat_14, 0);

    home_batt_bar_ = lv_obj_create(home_batt_card_);
    lv_obj_set_size(home_batt_bar_, 20, 10);
    lv_obj_set_style_radius(home_batt_bar_, 2, 0);
    lv_obj_set_style_border_width(home_batt_bar_, 1, 0);
    lv_obj_set_style_bg_opa(home_batt_bar_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(home_batt_bar_, 1, 0);
    lv_obj_clear_flag(home_batt_bar_, LV_OBJ_FLAG_SCROLLABLE);

    home_batt_bar_fill_ = lv_obj_create(home_batt_bar_);
    lv_obj_set_size(home_batt_bar_fill_, 16, 6);
    lv_obj_set_style_radius(home_batt_bar_fill_, 1, 0);
    lv_obj_set_style_bg_opa(home_batt_bar_fill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(home_batt_bar_fill_, 0, 0);
    lv_obj_set_style_pad_all(home_batt_bar_fill_, 0, 0);
    lv_obj_align(home_batt_bar_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(home_batt_bar_fill_, LV_OBJ_FLAG_SCROLLABLE);

    // Center Big Digital Time (Modern Smartwatch Display)
    home_time_label_ = lv_label_create(home_screen_);
    lv_label_set_text(home_time_label_, "00:00");
    lv_obj_set_style_text_font(home_time_label_, &lv_font_montserrat_48, 0);
    lv_obj_align(home_time_label_, LV_ALIGN_CENTER, 0, -22);

    // Date Capsule (Calendar Pill)
    home_date_card_ = create_card(home_screen_, 160, 28, 14);
    lv_obj_align(home_date_card_, LV_ALIGN_CENTER, 0, 24);
    home_date_label_ = lv_label_create(home_date_card_);
    lv_label_set_text(home_date_label_, "--, -- ---");
    lv_obj_set_style_text_font(home_date_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(home_date_label_);

    // Bottom Smart Glance (Borderless & Transparent, no bingkai)
    home_status_card_ = lv_obj_create(home_screen_);
    lv_obj_set_size(home_status_card_, 224, 28);
    lv_obj_align(home_status_card_, LV_ALIGN_BOTTOM_MID, 0, -12);
    lv_obj_set_style_bg_opa(home_status_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(home_status_card_, 0, 0);
    lv_obj_set_style_pad_all(home_status_card_, 0, 0);
    lv_obj_clear_flag(home_status_card_, LV_OBJ_FLAG_SCROLLABLE);

    home_status_label_ = lv_label_create(home_status_card_);
    lv_label_set_text(home_status_label_, "⚡ Chronos Smartwatch");
    lv_obj_set_style_text_font(home_status_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(home_status_label_, 220);
    lv_label_set_long_mode(home_status_label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(home_status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(home_status_label_);
}

// ---- NAVIGATION SCREEN ----
void ChronchiDisplay::createNavScreen() {
    nav_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(nav_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(nav_screen_, width_, height_);
    lv_obj_set_style_bg_opa(nav_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(nav_screen_, 0, 0);
    lv_obj_set_style_radius(nav_screen_, 0, 0);
    lv_obj_set_style_pad_all(nav_screen_, 0, 0);
    lv_obj_clear_flag(nav_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // 1. Header Capsule
    nav_header_card_ = create_card(nav_screen_, 224, 26, 13);
    lv_obj_align(nav_header_card_, LV_ALIGN_TOP_MID, 0, 6);
    nav_header_label_ = lv_label_create(nav_header_card_);
    lv_label_set_text(nav_header_label_, "🧭 GOOGLE MAPS");
    lv_obj_set_style_text_font(nav_header_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_header_label_, LV_ALIGN_LEFT_MID, 8, 0);

    nav_time_label_ = lv_label_create(nav_header_card_);
    lv_label_set_text(nav_time_label_, "00:00");
    lv_obj_set_style_text_font(nav_time_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_time_label_, LV_ALIGN_RIGHT_MID, -8, 0);

    // 2. Main Route & Maneuver Section (Completely transparent background, borderless & sleek)
    int nav_main_h = (height_ >= 280) ? 132 : 120;
    nav_main_card_ = lv_obj_create(nav_screen_);
    lv_obj_set_size(nav_main_card_, 224, nav_main_h);
    lv_obj_align(nav_main_card_, LV_ALIGN_CENTER, 0, (height_ >= 280) ? -10 : -8);
    lv_obj_set_style_bg_opa(nav_main_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_main_card_, 0, 0);
    lv_obj_set_style_pad_all(nav_main_card_, 0, 0);
    lv_obj_clear_flag(nav_main_card_, LV_OBJ_FLAG_SCROLLABLE);

    if (nav_icon_buf_) {
        nav_icon_img_ = lv_image_create(nav_main_card_);
        lv_image_set_src(nav_icon_img_, &nav_icon_dsc_);
        lv_image_set_scale(nav_icon_img_, 288);  // 1.125x scale (54x54 px) crisp turn arrow
        lv_obj_align(nav_icon_img_, LV_ALIGN_TOP_MID, 0, 2);
    }

    nav_dist_label_ = lv_label_create(nav_main_card_);
    lv_label_set_text(nav_dist_label_, "-- m");
    lv_obj_set_style_text_font(nav_dist_label_, &lv_font_montserrat_24, 0);
    lv_obj_align(nav_dist_label_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 58 : 52);

    nav_title_label_ = lv_label_create(nav_main_card_);
    lv_label_set_text(nav_title_label_, "📍 Rute Navigasi");
    lv_obj_set_style_text_font(nav_title_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nav_title_label_, 216);
    lv_obj_set_style_text_align(nav_title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nav_title_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(nav_title_label_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 86 : 78);

    nav_street_label_ = lv_label_create(nav_main_card_);
    lv_label_set_text(nav_street_label_, "Siap bernavigasi...");
    lv_obj_set_style_text_font(nav_street_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(nav_street_label_, 220);
    lv_obj_set_style_text_align(nav_street_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(nav_street_label_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(nav_street_label_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 106 : 96);

    // 3. Bottom Dashboard Stats (Borderless & Transparent, no bingkai)
    nav_stats_card_ = lv_obj_create(nav_screen_);
    lv_obj_set_size(nav_stats_card_, 224, 46);
    lv_obj_align(nav_stats_card_, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_opa(nav_stats_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_stats_card_, 0, 0);
    lv_obj_set_style_pad_all(nav_stats_card_, 0, 0);
    lv_obj_clear_flag(nav_stats_card_, LV_OBJ_FLAG_SCROLLABLE);

    nav_dur_label_ = lv_label_create(nav_stats_card_);
    lv_label_set_text(nav_dur_label_, "⏱ -- min");
    lv_obj_set_style_text_font(nav_dur_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_dur_label_, LV_ALIGN_TOP_LEFT, 10, 4);

    nav_eta_label_ = lv_label_create(nav_stats_card_);
    lv_label_set_text(nav_eta_label_, "🏁 ETA: --:--");
    lv_obj_set_style_text_font(nav_eta_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_eta_label_, LV_ALIGN_TOP_RIGHT, -10, 4);

    nav_speed_label_ = lv_label_create(nav_stats_card_);
    lv_label_set_text(nav_speed_label_, "⚡ Kecepatan: -- km/h");
    lv_obj_set_style_text_font(nav_speed_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(nav_speed_label_, LV_ALIGN_BOTTOM_MID, 0, -4);
}

// ---- NOTIFICATION SCREEN ----
void ChronchiDisplay::createNotifScreen() {
    notif_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(notif_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(notif_screen_, width_, height_);
    lv_obj_set_style_bg_opa(notif_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(notif_screen_, 0, 0);
    lv_obj_set_style_radius(notif_screen_, 0, 0);
    lv_obj_set_style_pad_all(notif_screen_, 0, 0);
    lv_obj_clear_flag(notif_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // App Header Badge Capsule
    notif_header_badge_ = create_card(notif_screen_, 216, 32, 14);
    lv_obj_align(notif_header_badge_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 10 : 8);

    notif_app_label_ = lv_label_create(notif_header_badge_);
    lv_label_set_text(notif_app_label_, "💬 NOTIFIKASI");
    lv_obj_set_style_text_font(notif_app_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(notif_app_label_, LV_ALIGN_LEFT_MID, 8, 0);

    notif_time_label_ = lv_label_create(notif_header_badge_);
    lv_label_set_text(notif_time_label_, "Baru saja");
    lv_obj_set_style_text_font(notif_time_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(notif_time_label_, LV_ALIGN_RIGHT_MID, -8, 0);

    // Main Notification Card (responsive to 240x240 vs 240x280)
    int card_h = (height_ >= 280) ? 175 : 145;
    int card_y = (height_ >= 280) ? 48 : 44;
    notif_card_ = create_card(notif_screen_, 216, card_h, 16);
    lv_obj_align(notif_card_, LV_ALIGN_TOP_MID, 0, card_y);

    notif_title_label_ = lv_label_create(notif_card_);
    lv_label_set_text(notif_title_label_, "Pengirim");
    lv_obj_set_style_text_font(notif_title_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(notif_title_label_, 196);
    lv_label_set_long_mode(notif_title_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(notif_title_label_, LV_ALIGN_TOP_LEFT, 8, 8);

    notif_line_ = lv_obj_create(notif_card_);
    lv_obj_set_size(notif_line_, 196, 2);
    lv_obj_align(notif_line_, LV_ALIGN_TOP_LEFT, 8, 30);
    lv_obj_set_style_bg_opa(notif_line_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(notif_line_, 0, 0);
    lv_obj_clear_flag(notif_line_, LV_OBJ_FLAG_SCROLLABLE);

    notif_msg_label_ = lv_label_create(notif_card_);
    lv_label_set_text(notif_msg_label_, "Pesan notifikasi...");
    lv_obj_set_style_text_font(notif_msg_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(notif_msg_label_, 196);
    lv_label_set_long_mode(notif_msg_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(notif_msg_label_, LV_ALIGN_TOP_LEFT, 8, 38);

    // Bottom Dismiss hint
    notif_icon_label_ = lv_label_create(notif_screen_);
    lv_label_set_text(notif_icon_label_, LV_SYMBOL_BELL " Tutup otomatis dalam 5s");
    lv_obj_set_style_text_font(notif_icon_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(notif_icon_label_, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// ---- WEATHER SCREEN ----
void ChronchiDisplay::createWeatherScreen() {
    weather_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(weather_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(weather_screen_, width_, height_);
    lv_obj_set_style_bg_opa(weather_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(weather_screen_, 0, 0);
    lv_obj_set_style_radius(weather_screen_, 0, 0);
    lv_obj_set_style_pad_all(weather_screen_, 0, 0);
    lv_obj_clear_flag(weather_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // Weather App Header
    weather_header_card_ = create_card(weather_screen_, 216, 28, 12);
    lv_obj_align(weather_header_card_, LV_ALIGN_TOP_MID, 0, 8);
    weather_header_label_ = lv_label_create(weather_header_card_);
    lv_label_set_text(weather_header_label_, "⛅ CUACA HARI INI");
    lv_obj_set_style_text_font(weather_header_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(weather_header_label_);

    // Weather Main Hero Card
    int main_h = (height_ >= 280) ? 90 : 84;
    weather_main_card_ = create_card(weather_screen_, 216, main_h, 16);
    lv_obj_align(weather_main_card_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 42 : 40);

    weather_temp_label_ = lv_label_create(weather_main_card_);
    lv_label_set_text(weather_temp_label_, "--°C");
    lv_obj_set_style_text_font(weather_temp_label_, &lv_font_montserrat_24, 0);
    lv_obj_align(weather_temp_label_, LV_ALIGN_LEFT_MID, 16, 0);

    weather_cond_label_ = lv_label_create(weather_main_card_);
    lv_label_set_text(weather_cond_label_, "Memuat...");
    lv_obj_set_style_text_font(weather_cond_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(weather_cond_label_, 110);
    lv_obj_set_style_text_align(weather_cond_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(weather_cond_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(weather_cond_label_, LV_ALIGN_TOP_RIGHT, -14, 18);

    weather_range_label_ = lv_label_create(weather_main_card_);
    lv_label_set_text(weather_range_label_, "H: --°  L: --°");
    lv_obj_set_style_text_font(weather_range_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(weather_range_label_, 110);
    lv_obj_set_style_text_align(weather_range_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(weather_range_label_, LV_ALIGN_BOTTOM_RIGHT, -14, -18);

    // Weather Stats Card
    int stats_y = (height_ >= 280) ? 138 : 130;
    int stats_h = (height_ >= 280) ? 60 : 54;
    weather_stats_card_ = create_card(weather_screen_, 216, stats_h, 14);
    lv_obj_align(weather_stats_card_, LV_ALIGN_TOP_MID, 0, stats_y);

    weather_humid_label_ = lv_label_create(weather_stats_card_);
    lv_label_set_text(weather_humid_label_, "💧 Kelembapan: --% | UV: -");
    lv_obj_set_style_text_font(weather_humid_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(weather_humid_label_, LV_ALIGN_TOP_LEFT, 8, 8);

    weather_press_label_ = lv_label_create(weather_stats_card_);
    lv_label_set_text(weather_press_label_, "⏱️ Tekanan: -- hPa");
    lv_obj_set_style_text_font(weather_press_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(weather_press_label_, LV_ALIGN_BOTTOM_LEFT, 8, -8);

    // Weather Location (Borderless & Transparent at bottom)
    weather_loc_card_ = lv_obj_create(weather_screen_);
    lv_obj_set_size(weather_loc_card_, 216, 28);
    lv_obj_align(weather_loc_card_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(weather_loc_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(weather_loc_card_, 0, 0);
    lv_obj_set_style_pad_all(weather_loc_card_, 0, 0);
    lv_obj_clear_flag(weather_loc_card_, LV_OBJ_FLAG_SCROLLABLE);
    weather_loc_label_ = lv_label_create(weather_loc_card_);
    lv_label_set_text(weather_loc_label_, "📍 Lokasi: --");
    lv_obj_set_style_text_font(weather_loc_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(weather_loc_label_, 200);
    lv_label_set_long_mode(weather_loc_label_, LV_LABEL_LONG_DOT);
    lv_obj_center(weather_loc_label_);
}

// ---- MUSIC SCREEN (Chronos Spotify / YT Music) ----
void ChronchiDisplay::createMusicScreen() {
    music_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(music_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(music_screen_, width_, height_);
    lv_obj_set_style_bg_opa(music_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_screen_, 0, 0);
    lv_obj_set_style_radius(music_screen_, 0, 0);
    lv_obj_set_style_pad_all(music_screen_, 0, 0);
    lv_obj_clear_flag(music_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // Music App Header
    music_header_card_ = create_card(music_screen_, 216, 28, 12);
    lv_obj_align(music_header_card_, LV_ALIGN_TOP_MID, 0, 8);
    music_header_label_ = lv_label_create(music_header_card_);
    lv_label_set_text(music_header_label_, "🎵 MEDIA PLAYER");
    lv_obj_set_style_text_font(music_header_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(music_header_label_, LV_ALIGN_LEFT_MID, 8, 0);

    music_state_label_ = lv_label_create(music_header_card_);
    lv_label_set_text(music_state_label_, "⏸ Jeda");
    lv_obj_set_style_text_font(music_state_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(music_state_label_, LV_ALIGN_RIGHT_MID, -8, 0);

    // Main Player Card
    music_card_ = create_card(music_screen_, 216, 142, 16);
    lv_obj_align(music_card_, LV_ALIGN_TOP_MID, 0, 42);

    music_app_label_ = lv_label_create(music_card_);
    lv_label_set_text(music_app_label_, "🎧 Chronos Audio");
    lv_obj_set_style_text_font(music_app_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(music_app_label_, LV_ALIGN_TOP_LEFT, 8, 8);

    music_title_label_ = lv_label_create(music_card_);
    lv_label_set_text(music_title_label_, "Tidak Ada Media");
    lv_obj_set_style_text_font(music_title_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(music_title_label_, 196);
    lv_label_set_long_mode(music_title_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(music_title_label_, LV_ALIGN_CENTER, 0, -12);

    music_artist_label_ = lv_label_create(music_card_);
    lv_label_set_text(music_artist_label_, "Putar musik di ponsel Anda");
    lv_obj_set_style_text_font(music_artist_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(music_artist_label_, 196);
    lv_label_set_long_mode(music_artist_label_, LV_LABEL_LONG_DOT);
    lv_obj_align(music_artist_label_, LV_ALIGN_CENTER, 0, 12);

    // Playback Progress Bar (Container + Fill)
    music_bar_ = create_card(music_card_, 196, 6, 3);
    lv_obj_align(music_bar_, LV_ALIGN_BOTTOM_MID, 0, -10);
    music_bar_fill_ = lv_obj_create(music_bar_);
    lv_obj_set_size(music_bar_fill_, 80, 6);
    lv_obj_set_style_radius(music_bar_fill_, 3, 0);
    lv_obj_set_style_bg_opa(music_bar_fill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(music_bar_fill_, 0, 0);
    lv_obj_set_style_pad_all(music_bar_fill_, 0, 0);
    lv_obj_align(music_bar_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(music_bar_fill_, LV_OBJ_FLAG_SCROLLABLE);

    // Controls Hint (Borderless & Transparent at bottom)
    music_hint_card_ = lv_obj_create(music_screen_);
    lv_obj_set_size(music_hint_card_, 216, 28);
    lv_obj_align(music_hint_card_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_opa(music_hint_card_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(music_hint_card_, 0, 0);
    lv_obj_set_style_pad_all(music_hint_card_, 0, 0);
    lv_obj_clear_flag(music_hint_card_, LV_OBJ_FLAG_SCROLLABLE);
    music_hint_label_ = lv_label_create(music_hint_card_);
    lv_label_set_text(music_hint_label_, "⏮ Vol- : Prev  |  ⏭ Vol+ : Next");
    lv_obj_set_style_text_font(music_hint_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(music_hint_label_);
}

// ---- STATUS SCREEN (Phone & Network) ----
void ChronchiDisplay::createStatusScreen() {
    status_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(status_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(status_screen_, width_, height_);
    lv_obj_set_style_bg_opa(status_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_screen_, 0, 0);
    lv_obj_set_style_radius(status_screen_, 0, 0);
    lv_obj_set_style_pad_all(status_screen_, 0, 0);
    lv_obj_clear_flag(status_screen_, LV_OBJ_FLAG_SCROLLABLE);

    // Status Header
    status_header_card_ = create_card(status_screen_, 216, 28, 12);
    lv_obj_align(status_header_card_, LV_ALIGN_TOP_MID, 0, 8);
    status_header_label_ = lv_label_create(status_header_card_);
    lv_label_set_text(status_header_label_, "📱 STATUS PONSEL");
    lv_obj_set_style_text_font(status_header_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(status_header_label_);

    // Phone Battery Card
    status_bat_card_ = create_card(status_screen_, 216, 68, 14);
    lv_obj_align(status_bat_card_, LV_ALIGN_TOP_MID, 0, 42);

    status_phone_bat_label_ = lv_label_create(status_bat_card_);
    lv_label_set_text(status_phone_bat_label_, "🔋 Baterai Ponsel");
    lv_obj_set_style_text_font(status_phone_bat_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(status_phone_bat_label_, LV_ALIGN_TOP_LEFT, 8, 8);

    status_bat_bar_ = create_card(status_bat_card_, 196, 8, 4);
    lv_obj_align(status_bat_bar_, LV_ALIGN_BOTTOM_MID, 0, -10);
    status_bat_bar_fill_ = lv_obj_create(status_bat_bar_);
    lv_obj_set_size(status_bat_bar_fill_, 100, 8);
    lv_obj_set_style_radius(status_bat_bar_fill_, 4, 0);
    lv_obj_set_style_bg_opa(status_bat_bar_fill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(status_bat_bar_fill_, 0, 0);
    lv_obj_set_style_pad_all(status_bat_bar_fill_, 0, 0);
    lv_obj_align(status_bat_bar_fill_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(status_bat_bar_fill_, LV_OBJ_FLAG_SCROLLABLE);

    // Network & Signal Card
    status_net_card_ = create_card(status_screen_, 216, 68, 14);
    lv_obj_align(status_net_card_, LV_ALIGN_TOP_MID, 0, 116);

    status_network_label_ = lv_label_create(status_net_card_);
    lv_label_set_text(status_network_label_, "📶 Jaringan: Seluler");
    lv_obj_set_style_text_font(status_network_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(status_network_label_, LV_ALIGN_TOP_LEFT, 8, 8);

    status_signal_label_ = lv_label_create(status_net_card_);
    lv_label_set_text(status_signal_label_, "Sinyal: --/4");
    lv_obj_set_style_text_font(status_signal_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(status_signal_label_, LV_ALIGN_BOTTOM_LEFT, 8, -8);

    // Bluetooth Info Pill
    status_bt_card_ = create_card(status_screen_, 216, 40, 14);
    lv_obj_align(status_bt_card_, LV_ALIGN_BOTTOM_MID, 0, -8);
    status_bt_label_ = lv_label_create(status_bt_card_);
    lv_label_set_text(status_bt_label_, "⚡ BLE Chronos Terhubung");
    lv_obj_set_style_text_font(status_bt_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(status_bt_label_);
}

// ---- ALERT SCREEN (Find Watch, Alarm, Camera trigger) ----
void ChronchiDisplay::createAlertScreen() {
    alert_screen_ = lv_obj_create(NULL);
    lv_obj_set_style_text_font(alert_screen_, &lv_font_montserrat_14, 0);
    lv_obj_set_size(alert_screen_, width_, height_);
    lv_obj_set_style_bg_opa(alert_screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(alert_screen_, 0, 0);
    lv_obj_set_style_radius(alert_screen_, 0, 0);
    lv_obj_set_style_pad_all(alert_screen_, 0, 0);
    lv_obj_clear_flag(alert_screen_, LV_OBJ_FLAG_SCROLLABLE);

    alert_card_ = create_card(alert_screen_, 216, 200, 18);
    lv_obj_align(alert_card_, LV_ALIGN_CENTER, 0, -14);

    alert_icon_label_ = lv_label_create(alert_card_);
    lv_label_set_text(alert_icon_label_, LV_SYMBOL_BELL "  " LV_SYMBOL_WARNING);
    lv_obj_set_style_text_font(alert_icon_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(alert_icon_label_, LV_ALIGN_TOP_MID, 0, 16);

    alert_title_label_ = lv_label_create(alert_card_);
    lv_label_set_text(alert_title_label_, "PERINGATAN");
    lv_obj_set_style_text_font(alert_title_label_, &lv_font_montserrat_14, 0);
    lv_obj_align(alert_title_label_, LV_ALIGN_TOP_MID, 0, 50);

    alert_msg_label_ = lv_label_create(alert_card_);
    lv_label_set_text(alert_msg_label_, "");
    lv_obj_set_style_text_font(alert_msg_label_, &lv_font_montserrat_14, 0);
    lv_obj_set_width(alert_msg_label_, 190);
    lv_label_set_long_mode(alert_msg_label_, LV_LABEL_LONG_WRAP);
    lv_obj_align(alert_msg_label_, LV_ALIGN_TOP_MID, 0, 85);

    alert_btn_ = create_card(alert_screen_, 180, 36, 18);
    lv_obj_align(alert_btn_, LV_ALIGN_BOTTOM_MID, 0, -12);
    alert_btn_label_ = lv_label_create(alert_btn_);
    lv_label_set_text(alert_btn_label_, "✕ MATIKAN / STOP");
    lv_obj_set_style_text_font(alert_btn_label_, &lv_font_montserrat_14, 0);
    lv_obj_center(alert_btn_label_);
}

// ---- DYNAMIC THEME & COLOR APPLICATION ----
void ChronchiDisplay::applyColorMode() {
    const auto& th = PRESET_THEMES[current_layout_ % NUM_THEMES];
    lv_color_t bg_color       = lv_color_hex(th.bg);
    lv_color_t card_bg_col    = lv_color_hex(th.card_bg);
    lv_color_t card_bdr_col   = lv_color_hex(th.card_border);
    lv_color_t text_color     = (theme_color_ != 0 && theme_color_ != 0x00E5FF) ? lv_color_hex(theme_color_) : lv_color_hex(th.text);
    lv_color_t dim_color      = lv_color_hex(th.dim);
    lv_color_t accent_color   = (theme_color_ != 0 && theme_color_ != 0x00E5FF) ? lv_color_hex(theme_color_) : lv_color_hex(th.accent);
    lv_color_t success_color  = light_mode_ ? lv_color_hex(0x16A34A) : lv_color_hex(0x22C55E);
    lv_color_t notif_line_col = light_mode_ ? lv_color_hex(0xCBD5E1) : lv_color_hex(0x334155);
    lv_color_t notif_msg_col  = light_mode_ ? lv_color_hex(0x334155) : lv_color_hex(0xCBD5E1);

    auto style_card = [](lv_obj_t* card, lv_color_t bg, lv_color_t border) {
        if (card && lv_obj_is_valid(card)) {
            lv_obj_set_style_bg_color(card, bg, 0);
            lv_obj_set_style_border_color(card, border, 0);
        }
    };

    // 1. Home Screen (Borderless Watchface with Card Pills)
    if (home_screen_) lv_obj_set_style_bg_color(home_screen_, bg_color, 0);
    style_card(home_bt_card_, card_bg_col, last_bt_connected_ ? success_color : card_bdr_col);
    if (home_bt_label_) lv_obj_set_style_text_color(home_bt_label_, last_bt_connected_ ? success_color : dim_color, 0);
    if (home_chronchi_label_) lv_obj_set_style_text_color(home_chronchi_label_, accent_color, 0);
    style_card(home_batt_card_, card_bg_col, last_batt_level_ <= 20 ? lv_color_hex(0xEA580C) : card_bdr_col);
    if (home_batt_label_) lv_obj_set_style_text_color(home_batt_label_, lv_color_hex(0xFACC15), 0);
    if (home_batt_pct_label_) lv_obj_set_style_text_color(home_batt_pct_label_, text_color, 0);
    if (home_batt_bar_) {
        lv_obj_set_style_border_color(home_batt_bar_, text_color, 0);
    }
    if (home_batt_bar_fill_) {
        lv_color_t bar_col = last_batt_charging_ ? lv_color_hex(0x00E5FF) :
                             (last_batt_level_ > 50) ? lv_color_hex(0x22C55E) :
                             (last_batt_level_ > 20) ? lv_color_hex(0xF59E0B) :
                             lv_color_hex(0xEF4444);
        lv_obj_set_style_bg_color(home_batt_bar_fill_, bar_col, 0);
    }
    if (home_time_label_) lv_obj_set_style_text_color(home_time_label_, text_color, 0);
    style_card(home_date_card_, card_bg_col, card_bdr_col);
    if (home_date_label_) lv_obj_set_style_text_color(home_date_label_, dim_color, 0);

    // Bottom status text: transparent & borderless (no bingkai)
    if (home_status_card_) {
        lv_obj_set_style_bg_opa(home_status_card_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(home_status_card_, 0, 0);
    }
    if (home_status_label_) lv_obj_set_style_text_color(home_status_label_, text_color, 0);

    // 2. Nav Screen
    if (nav_screen_) lv_obj_set_style_bg_color(nav_screen_, bg_color, 0);
    style_card(nav_header_card_, card_bg_col, lv_color_hex(0x2563EB));
    if (nav_header_label_) lv_obj_set_style_text_color(nav_header_label_, lv_color_hex(0x3B82F6), 0);
    if (nav_time_label_) lv_obj_set_style_text_color(nav_time_label_, text_color, 0);

    // Keep route area completely transparent and borderless
    if (nav_main_card_) {
        lv_obj_set_style_bg_opa(nav_main_card_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(nav_main_card_, 0, 0);
    }
    if (nav_dist_label_) lv_obj_set_style_text_color(nav_dist_label_, accent_color, 0);
    if (nav_title_label_) lv_obj_set_style_text_color(nav_title_label_, dim_color, 0);
    if (nav_street_label_) lv_obj_set_style_text_color(nav_street_label_, text_color, 0);

    // Bottom stats: transparent & borderless (no bingkai)
    if (nav_stats_card_) {
        lv_obj_set_style_bg_opa(nav_stats_card_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(nav_stats_card_, 0, 0);
    }
    if (nav_dur_label_) lv_obj_set_style_text_color(nav_dur_label_, text_color, 0);
    if (nav_eta_label_) lv_obj_set_style_text_color(nav_eta_label_, success_color, 0);
    if (nav_speed_label_) lv_obj_set_style_text_color(nav_speed_label_, accent_color, 0);
    setNavIcon(last_maneuver_.empty() ? "STRAIGHT" : last_maneuver_.c_str());

    // 3. Notif Screen
    if (notif_screen_) lv_obj_set_style_bg_color(notif_screen_, bg_color, 0);
    style_card(notif_header_badge_, card_bg_col, accent_color);
    if (notif_app_label_) lv_obj_set_style_text_color(notif_app_label_, accent_color, 0);
    if (notif_time_label_) lv_obj_set_style_text_color(notif_time_label_, dim_color, 0);
    style_card(notif_card_, card_bg_col, card_bdr_col);
    if (notif_line_) lv_obj_set_style_bg_color(notif_line_, notif_line_col, 0);
    if (notif_title_label_) lv_obj_set_style_text_color(notif_title_label_, text_color, 0);
    if (notif_msg_label_) lv_obj_set_style_text_color(notif_msg_label_, notif_msg_col, 0);
    if (notif_icon_label_) lv_obj_set_style_text_color(notif_icon_label_, dim_color, 0);

    // 4. Weather Screen
    if (weather_screen_) lv_obj_set_style_bg_color(weather_screen_, bg_color, 0);
    style_card(weather_header_card_, card_bg_col, lv_color_hex(0x0284C7));
    if (weather_header_label_) lv_obj_set_style_text_color(weather_header_label_, lv_color_hex(0x38BDF8), 0);
    style_card(weather_main_card_, card_bg_col, card_bdr_col);
    if (weather_temp_label_) lv_obj_set_style_text_color(weather_temp_label_, lv_color_hex(0xF59E0B), 0);
    if (weather_cond_label_) lv_obj_set_style_text_color(weather_cond_label_, text_color, 0);
    if (weather_range_label_) lv_obj_set_style_text_color(weather_range_label_, dim_color, 0);
    style_card(weather_stats_card_, card_bg_col, card_bdr_col);
    if (weather_humid_label_) lv_obj_set_style_text_color(weather_humid_label_, text_color, 0);
    if (weather_press_label_) lv_obj_set_style_text_color(weather_press_label_, dim_color, 0);
    // Weather location: transparent & borderless (no bingkai)
    if (weather_loc_card_) {
        lv_obj_set_style_bg_opa(weather_loc_card_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(weather_loc_card_, 0, 0);
    }
    if (weather_loc_label_) lv_obj_set_style_text_color(weather_loc_label_, lv_color_hex(0x00E5FF), 0);

    // 5. Music Screen
    if (music_screen_) lv_obj_set_style_bg_color(music_screen_, bg_color, 0);
    style_card(music_header_card_, card_bg_col, lv_color_hex(0x1DB954));
    if (music_header_label_) lv_obj_set_style_text_color(music_header_label_, lv_color_hex(0x1DB954), 0);
    if (music_state_label_) lv_obj_set_style_text_color(music_state_label_, success_color, 0);
    style_card(music_card_, card_bg_col, lv_color_hex(0x1DB954));
    if (music_app_label_) lv_obj_set_style_text_color(music_app_label_, lv_color_hex(0x38BDF8), 0);
    if (music_title_label_) lv_obj_set_style_text_color(music_title_label_, text_color, 0);
    if (music_artist_label_) lv_obj_set_style_text_color(music_artist_label_, dim_color, 0);
    if (music_bar_) {
        lv_obj_set_style_bg_color(music_bar_, card_bdr_col, 0);
        lv_obj_set_style_border_color(music_bar_, card_bdr_col, 0);
    }
    if (music_bar_fill_) {
        lv_obj_set_style_bg_color(music_bar_fill_, lv_color_hex(0x22C55E), 0);
    }

    // Music hint: transparent & borderless (no bingkai)
    if (music_hint_card_) {
        lv_obj_set_style_bg_opa(music_hint_card_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(music_hint_card_, 0, 0);
    }
    if (music_hint_label_) lv_obj_set_style_text_color(music_hint_label_, accent_color, 0);

    // 6. Status Screen
    if (status_screen_) lv_obj_set_style_bg_color(status_screen_, bg_color, 0);
    style_card(status_header_card_, card_bg_col, lv_color_hex(0x06B6D4));
    if (status_header_label_) lv_obj_set_style_text_color(status_header_label_, lv_color_hex(0x00E5FF), 0);
    style_card(status_bat_card_, card_bg_col, card_bdr_col);
    if (status_phone_bat_label_) lv_obj_set_style_text_color(status_phone_bat_label_, text_color, 0);
    if (status_bat_bar_) {
        lv_obj_set_style_bg_color(status_bat_bar_, card_bdr_col, 0);
        lv_obj_set_style_border_color(status_bat_bar_, card_bdr_col, 0);
    }
    if (status_bat_bar_fill_) {
        lv_obj_set_style_bg_color(status_bat_bar_fill_, success_color, 0);
    }
    style_card(status_net_card_, card_bg_col, card_bdr_col);
    if (status_network_label_) lv_obj_set_style_text_color(status_network_label_, accent_color, 0);
    if (status_signal_label_) lv_obj_set_style_text_color(status_signal_label_, dim_color, 0);
    style_card(status_bt_card_, card_bg_col, success_color);
    if (status_bt_label_) lv_obj_set_style_text_color(status_bt_label_, success_color, 0);

    // 7. Alert Screen
    if (alert_screen_) lv_obj_set_style_bg_color(alert_screen_, bg_color, 0);
    style_card(alert_card_, light_mode_ ? lv_color_hex(0xFEE2E2) : lv_color_hex(0x3B0707), lv_color_hex(0xEF4444));
    if (alert_icon_label_) lv_obj_set_style_text_color(alert_icon_label_, lv_color_hex(0xFACC15), 0);
    if (alert_title_label_) lv_obj_set_style_text_color(alert_title_label_, lv_color_hex(0xEF4444), 0);
    if (alert_msg_label_) lv_obj_set_style_text_color(alert_msg_label_, light_mode_ ? lv_color_hex(0x7F1D1D) : lv_color_hex(0xFCA5A5), 0);
    style_card(alert_btn_, lv_color_hex(0xDC2626), lv_color_hex(0xEF4444));
    if (alert_btn_label_) lv_obj_set_style_text_color(alert_btn_label_, lv_color_hex(0xFFFFFF), 0);
}

void ChronchiDisplay::setWatchfaceTheme(uint32_t color, uint32_t layout) {
    ESP_LOGI(TAG, "Watchface customized via Chronos: color=0x%06lX, layout=%lu",
             (unsigned long)color, (unsigned long)layout);

    current_layout_ = layout % NUM_THEMES;
    light_mode_ = (current_layout_ == 1 || current_layout_ == 6);

    if (color != 0) {
        theme_color_ = color;
        // Automatic contrast adjustment if color clashes with background
        int r = (color >> 16) & 0xFF;
        int g = (color >> 8) & 0xFF;
        int b = color & 0xFF;
        int lum = (r * 299 + g * 587 + b * 114) / 1000;
        if (lum < 60 && !light_mode_) {
            // Dark text sent -> use light background
            current_layout_ = 1;
            light_mode_ = true;
        } else if (lum > 190 && light_mode_) {
            // Very bright text sent -> use dark background
            current_layout_ = 0;
            light_mode_ = false;
        }
    }

    Settings settings("chronchi", true);
    settings.SetInt("layout", current_layout_);
    settings.SetInt("color", theme_color_);
    settings.SetBool("light_mode", light_mode_);

    applyColorMode();
}

void ChronchiDisplay::toggleColorMode() {
    current_layout_ = (current_layout_ + 1) % NUM_THEMES;
    light_mode_ = (current_layout_ == 1 || current_layout_ == 6);
    Settings settings("chronchi", true);
    settings.SetInt("layout", current_layout_);
    settings.SetBool("light_mode", light_mode_);
    ESP_LOGI(TAG, "Watchface theme toggled to: %lu (%s)",
             (unsigned long)current_layout_, PRESET_THEMES[current_layout_].name);
    lv_lock();
    applyColorMode();
    lv_unlock();
}

void ChronchiDisplay::setColorMode(bool light) {
    current_layout_ = light ? 1 : 0;
    light_mode_ = light;
    Settings settings("chronchi", true);
    settings.SetInt("layout", current_layout_);
    settings.SetBool("light_mode", light_mode_);
    ESP_LOGI(TAG, "Color mode set to: %s", light ? "LIGHT (White)" : "DARK (Black)");
    lv_lock();
    applyColorMode();
    lv_unlock();
}

void ChronchiDisplay::cycleTheme() {
    toggleColorMode();
}

void ChronchiDisplay::setTheme(int theme) {
    setWatchfaceTheme(theme_color_, theme);
}

void ChronchiDisplay::setThemeColor(uint32_t rgb) {
    setWatchfaceTheme(rgb, current_layout_);
}

void ChronchiDisplay::updateHomeWeather(const ChronchiWeather& weather) {
    lv_lock();
    if (home_status_label_ && lv_obj_is_valid(home_status_label_) && !google_maps_active_) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "%.0f°C  %s", weather.temperatureC, weather.condition.c_str());
        lv_label_set_text(home_status_label_, sbuf);
    }
    lv_unlock();
}

// ---- Screen management ----
void ChronchiDisplay::hideAllScreens() {
    if (home_screen_ && lv_screen_active() == home_screen_) lv_screen_load(lv_obj_get_screen(home_screen_));
}

void ChronchiDisplay::showScreen(lv_obj_t* screen) {
    if (screen) lv_screen_load(screen);
}

void ChronchiDisplay::setScreenState(ScreenState s) {
    prev_state_ = state_;
    state_ = s;
    switch (s) {
        case HOME: showScreen(home_screen_); break;
        case NAVIGATION: showScreen(nav_screen_); break;
        case NOTIFICATION: showScreen(notif_screen_); break;
        case WEATHER: showScreen(weather_screen_); break;
        case MUSIC: showScreen(music_screen_); break;
        case STATUS: showScreen(status_screen_); break;
        case ALERT: showScreen(alert_screen_); break;
    }
}

void ChronchiDisplay::showHome() {
    lv_lock();
    if (home_screen_) {
        lv_screen_load(home_screen_);
        ESP_LOGI(TAG, "Loaded Chronchi home watchface (Theme: %s)", PRESET_THEMES[current_layout_ % NUM_THEMES].name);
    }
    state_ = HOME;
    lv_unlock();
}

void ChronchiDisplay::showNavigation(const ChronchiNavigation& nav) {
    lv_lock();
    google_maps_active_ = nav.active;
    if (nav_title_label_) {
        if (!nav.title.empty()) {
            char tbuf[64];
            snprintf(tbuf, sizeof(tbuf), "📍 %s", nav.title.c_str());
            lv_label_set_text(nav_title_label_, tbuf);
        } else {
            lv_label_set_text(nav_title_label_, "📍 Rute Aktif");
        }
    }
    if (nav_street_label_) {
        lv_label_set_text(nav_street_label_, nav.directions.empty() ? "Ikuti rute jalan..." : nav.directions.c_str());
    }
    if (nav_dist_label_) {
        lv_label_set_text(nav_dist_label_, nav.distance.empty() ? "-- m" : nav.distance.c_str());
    }
    if (nav_dur_label_) {
        char buf[32];
        if (!nav.duration.empty()) {
            snprintf(buf, sizeof(buf), "⏱ %s", nav.duration.c_str());
            lv_label_set_text(nav_dur_label_, buf);
        } else {
            lv_label_set_text(nav_dur_label_, "⏱ -- min");
        }
    }
    if (nav_eta_label_) {
        char buf[32];
        if (!nav.eta.empty()) {
            if (strstr(nav.eta.c_str(), "ETA") || strstr(nav.eta.c_str(), "eta")) {
                snprintf(buf, sizeof(buf), "🏁 %s", nav.eta.c_str());
            } else {
                snprintf(buf, sizeof(buf), "🏁 ETA %s", nav.eta.c_str());
            }
            lv_label_set_text(nav_eta_label_, buf);
        } else {
            lv_label_set_text(nav_eta_label_, "🏁 ETA --:--");
        }
    }
    if (nav_speed_label_) {
        char buf[32];
        if (!nav.speed.empty()) {
            if (strstr(nav.speed.c_str(), "km") || strstr(nav.speed.c_str(), "m/h")) {
                snprintf(buf, sizeof(buf), "⚡ %s", nav.speed.c_str());
            } else {
                snprintf(buf, sizeof(buf), "⚡ %s km/h", nav.speed.c_str());
            }
            lv_label_set_text(nav_speed_label_, buf);
        } else {
            lv_label_set_text(nav_speed_label_, "⚡ -- km/h");
        }
    }

    if (!nav.maneuver.empty()) {
        setNavIcon(nav.maneuver.c_str());
    } else if (!nav.directions.empty()) {
        std::string d = nav.directions;
        for (auto& c : d) c = (char)tolower((unsigned char)c);
        if (d.find("slight left") != std::string::npos || d.find("serong kiri") != std::string::npos) {
            setNavIcon("SLIGHT_LEFT");
        } else if (d.find("slight right") != std::string::npos || d.find("serong kanan") != std::string::npos) {
            setNavIcon("SLIGHT_RIGHT");
        } else if (d.find("kiri") != std::string::npos || d.find("left") != std::string::npos) {
            setNavIcon("LEFT");
        } else if (d.find("kanan") != std::string::npos || d.find("right") != std::string::npos) {
            setNavIcon("RIGHT");
        } else if (d.find("bundaran") != std::string::npos || d.find("roundabout") != std::string::npos || d.find("putar") != std::string::npos) {
            setNavIcon("ROUNDABOUT");
        } else if (d.find("sampai") != std::string::npos || d.find("arrive") != std::string::npos || d.find("tujuan") != std::string::npos) {
            setNavIcon("ARRIVE");
        } else {
            setNavIcon("STRAIGHT");
        }
    } else {
        setNavIcon("STRAIGHT");
    }

    if (state_ != NAVIGATION) setScreenState(NAVIGATION);
    lv_unlock();
}

void ChronchiDisplay::showNotification(const ChronchiNotification& notif) {
    lv_lock();
    if (notif_app_label_) {
        char abuf[64];
        snprintf(abuf, sizeof(abuf), "💬 %s", notif.app.empty() ? "NOTIFIKASI" : notif.app.c_str());
        lv_label_set_text(notif_app_label_, abuf);
    }
    if (notif_time_label_) lv_label_set_text(notif_time_label_, notif.time.empty() ? "Baru saja" : notif.time.c_str());
    if (notif_title_label_) lv_label_set_text(notif_title_label_, notif.title.c_str());
    if (notif_msg_label_) lv_label_set_text(notif_msg_label_, notif.message.c_str());

    // App-specific header color badge (WhatsApp, Telegram, Instagram, Spotify, etc.)
    if (notif_header_badge_) {
        lv_color_t badge_bg = lv_color_hex(0x1E293B);
        lv_color_t badge_border = lv_color_hex(0x3B82F6);
        lv_color_t badge_text = lv_color_hex(0x60A5FA);

        if (strstr(notif.app.c_str(), "WhatsApp") || strstr(notif.app.c_str(), "WA")) {
            badge_bg = lv_color_hex(0x064E3B);
            badge_border = lv_color_hex(0x10B981);
            badge_text = lv_color_hex(0x34D399);
        } else if (strstr(notif.app.c_str(), "Telegram")) {
            badge_bg = lv_color_hex(0x0C4A6E);
            badge_border = lv_color_hex(0x0284C7);
            badge_text = lv_color_hex(0x38BDF8);
        } else if (strstr(notif.app.c_str(), "Instagram")) {
            badge_bg = lv_color_hex(0x4A044E);
            badge_border = lv_color_hex(0xC026D3);
            badge_text = lv_color_hex(0xF472B6);
        } else if (strstr(notif.app.c_str(), "Spotify")) {
            badge_bg = lv_color_hex(0x052E16);
            badge_border = lv_color_hex(0x15803D);
            badge_text = lv_color_hex(0x22C55E);
        } else if (strstr(notif.app.c_str(), "YouTube")) {
            badge_bg = lv_color_hex(0x450A0A);
            badge_border = lv_color_hex(0xDC2626);
            badge_text = lv_color_hex(0xF87171);
        }

        lv_obj_set_style_bg_color(notif_header_badge_, badge_bg, 0);
        lv_obj_set_style_border_color(notif_header_badge_, badge_border, 0);
        if (notif_app_label_) lv_obj_set_style_text_color(notif_app_label_, badge_text, 0);
    }

    prev_state_ = state_;
    setScreenState(NOTIFICATION);

    // Auto-dismiss setelah 6 detik
    if (notif_timer_) {
        lv_timer_delete(notif_timer_);
        notif_timer_ = nullptr;
    }
    notif_timer_ = lv_timer_create([](lv_timer_t* t) {
        auto* disp = static_cast<ChronchiDisplay*>(lv_timer_get_user_data(t));
        if (disp) {
            disp->notif_timer_ = nullptr;
            if (disp->state_ == NOTIFICATION) {
                if (disp->google_maps_active_) {
                    disp->setScreenState(NAVIGATION);
                } else {
                    disp->showHome();
                }
            }
        }
        lv_timer_delete(t);
    }, 6000, this);
    lv_timer_set_repeat_count(notif_timer_, 1);
    lv_unlock();
}

void ChronchiDisplay::set24HourFormat(bool is_24h) {
    is_24h_format_ = is_24h;
}

void ChronchiDisplay::updateTime(int hour, int minute, int second) {
    lv_lock();
    char buf[16];
    if (is_24h_format_) {
        snprintf(buf, sizeof(buf), "%02d:%02d", hour, minute);
    } else {
        int h = hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%02d:%02d", h, minute);
    }
    if (home_time_label_ && lv_obj_is_valid(home_time_label_)) lv_label_set_text(home_time_label_, buf);
    if (nav_time_label_ && lv_obj_is_valid(nav_time_label_)) lv_label_set_text(nav_time_label_, buf);

    // Update date in Calendar capsule
    time_t now = time(NULL);
    struct tm* tm_info = localtime(&now);
    char date_buf[32];
    const char* days[] = {"Min", "Sen", "Sel", "Rab", "Kam", "Jum", "Sab"};
    const char* months[] = {"Jan", "Feb", "Mar", "Apr", "Mei", "Jun", "Jul", "Ags", "Sep", "Okt", "Nov", "Des"};
    if (is_24h_format_) {
        snprintf(date_buf, sizeof(date_buf), "📅 %s, %d %s", days[tm_info->tm_wday], tm_info->tm_mday, months[tm_info->tm_mon]);
    } else {
        snprintf(date_buf, sizeof(date_buf), "%s | %s, %d %s", hour >= 12 ? "PM" : "AM", days[tm_info->tm_wday], tm_info->tm_mday, months[tm_info->tm_mon]);
    }
    if (home_date_label_ && lv_obj_is_valid(home_date_label_)) lv_label_set_text(home_date_label_, date_buf);
    lv_unlock();
}

void ChronchiDisplay::updateBattery(uint8_t level, bool charging) {
    lv_lock();
    last_batt_level_ = level;
    last_batt_charging_ = charging;

    if (home_batt_label_ && lv_obj_is_valid(home_batt_label_)) {
        if (charging) {
            lv_label_set_text(home_batt_label_, LV_SYMBOL_CHARGE);
            lv_obj_set_style_text_color(home_batt_label_, lv_color_hex(0xFACC15), 0);
            lv_obj_clear_flag(home_batt_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(home_batt_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (home_batt_pct_label_ && lv_obj_is_valid(home_batt_pct_label_)) {
        char pbuf[16];
        snprintf(pbuf, sizeof(pbuf), "%d%%", (int)level);
        lv_label_set_text(home_batt_pct_label_, pbuf);
    }

    if (home_batt_bar_fill_ && lv_obj_is_valid(home_batt_bar_fill_)) {
        int32_t fw = (16 * level) / 100;
        if (fw < 2 && level > 0) fw = 2;
        if (fw > 16) fw = 16;
        lv_obj_set_width(home_batt_bar_fill_, fw);

        lv_color_t bar_col = charging ? lv_color_hex(0x00E5FF) :
                             (level > 50) ? lv_color_hex(0x22C55E) :
                             (level > 20) ? lv_color_hex(0xF59E0B) :
                             lv_color_hex(0xEF4444);
        lv_obj_set_style_bg_color(home_batt_bar_fill_, bar_col, 0);
    }

    if (home_batt_card_ && lv_obj_is_valid(home_batt_card_)) {
        const auto& th = PRESET_THEMES[current_layout_ % NUM_THEMES];
        lv_color_t border_col = (level <= 20) ? lv_color_hex(0xEA580C) : lv_color_hex(th.card_border);
        lv_obj_set_style_border_color(home_batt_card_, border_col, 0);
    }
    lv_unlock();
}

void ChronchiDisplay::updateBtStatus(bool connected) {
    lv_lock();
    last_bt_connected_ = connected;
    const auto& th = PRESET_THEMES[current_layout_ % NUM_THEMES];
    lv_color_t success_color = light_mode_ ? lv_color_hex(0x16A34A) : lv_color_hex(0x22C55E);
    lv_color_t dim_color = lv_color_hex(th.dim);
    lv_color_t border_color = connected ? success_color : lv_color_hex(th.card_border);

    if (home_bt_label_ && lv_obj_is_valid(home_bt_label_)) {
        lv_obj_set_style_text_color(home_bt_label_, connected ? success_color : dim_color, 0);
    }
    if (home_bt_card_ && lv_obj_is_valid(home_bt_card_)) {
        lv_obj_set_style_border_color(home_bt_card_, border_color, 0);
    }
    lv_unlock();
}

void ChronchiDisplay::setNavigationIcon(uint8_t* icon_data, int size) {
    if (!nav_icon_buf_ || !icon_data || size < 288) return;
    lv_lock();
    const auto& th = PRESET_THEMES[current_layout_ % NUM_THEMES];
    uint16_t bg_color = hex_to_rgb565(th.bg);
    uint16_t fg_color = hex_to_rgb565(th.accent);
    if (fg_color == bg_color) {
        fg_color = light_mode_ ? 0x0290 : 0x07FF;
    }

    // Convert 1bpp 48x48 to RGB565 with transparent theme blending
    for (int y = 0; y < 48; y++) {
        for (int x = 0; x < 48; x++) {
            int byte_idx = (y * 48 + x) / 8;
            int bit_pos = 7 - (x % 8);
            bool on = (icon_data[byte_idx] >> bit_pos) & 0x01;
            uint16_t color = on ? fg_color : bg_color;
            int offset = (y * 48 + x) * 2;
            nav_icon_buf_[offset] = color & 0xFF;
            nav_icon_buf_[offset + 1] = (color >> 8) & 0xFF;
        }
    }
    if (nav_icon_img_) lv_image_set_src(nav_icon_img_, &nav_icon_dsc_);
    lv_unlock();
}

void ChronchiDisplay::showWeather(const ChronchiWeather& weather) {
    lv_lock();
    char buf[64];
    snprintf(buf, sizeof(buf), "%.0f°C", weather.temperatureC);
    if (weather_temp_label_) lv_label_set_text(weather_temp_label_, buf);
    if (weather_cond_label_) lv_label_set_text(weather_cond_label_, weather.condition.empty() ? "Cerah" : weather.condition.c_str());

    if (weather_range_label_) {
        if (weather.highC != 0 || weather.lowC != 0) {
            snprintf(buf, sizeof(buf), "H: %d°  L: %d°", weather.highC, weather.lowC);
        } else {
            snprintf(buf, sizeof(buf), "H: --°  L: --°");
        }
        lv_label_set_text(weather_range_label_, buf);
    }

    if (weather_humid_label_) {
        if (weather.uv > 0) {
            snprintf(buf, sizeof(buf), "💧 Kelembapan: %d%% | ☀️ UV: %d", weather.humidity, weather.uv);
        } else {
            snprintf(buf, sizeof(buf), "💧 Kelembapan: %d%%", weather.humidity);
        }
        lv_label_set_text(weather_humid_label_, buf);
    }

    if (weather_press_label_) {
        if (weather.pressure > 0) {
            snprintf(buf, sizeof(buf), "⏱️ Tekanan: %d hPa", weather.pressure);
        } else {
            snprintf(buf, sizeof(buf), "⏱️ Tekanan: 1012 hPa (Normal)");
        }
        lv_label_set_text(weather_press_label_, buf);
    }

    if (weather_loc_label_) {
        snprintf(buf, sizeof(buf), "📍 %s", weather.location.empty() ? "Prakiraan Cuaca" : weather.location.c_str());
        lv_label_set_text(weather_loc_label_, buf);
    }

    // Also update home status glance capsule if no navigation is active
    if (!google_maps_active_ && home_status_label_ && lv_obj_is_valid(home_status_label_)) {
        char status_buf[64];
        if (!weather.condition.empty()) {
            snprintf(status_buf, sizeof(status_buf), "⛅ %.0f°C  %s", weather.temperatureC, weather.condition.c_str());
            lv_label_set_text(home_status_label_, status_buf);
        }
    }

    // Do NOT automatically switch away from HOME to WEATHER; let the user stay on the digital clock!
    lv_unlock();
}

void ChronchiDisplay::showPhoneStatus(const ChronchiPhoneStatus& status) {
    lv_lock();
    char buf[64];
    const char* phoneChg = status.charging ? " ⚡" : "";
    const char* watchChg = last_batt_charging_ ? " ⚡" : "";
    snprintf(buf, sizeof(buf), "⌚ Jam: %d%%%s  |  📱 HP: %d%%%s",
             last_batt_level_, watchChg, status.batteryLevel, phoneChg);
    if (status_phone_bat_label_) lv_label_set_text(status_phone_bat_label_, buf);

    if (status_bat_bar_fill_) {
        int32_t bw = (196 * status.batteryLevel) / 100;
        if (bw < 0) bw = 0;
        if (bw > 196) bw = 196;
        lv_obj_set_width(status_bat_bar_fill_, bw);
        lv_color_t bar_col = status.batteryLevel > 50 ? lv_color_hex(0x10B981) :
                             status.batteryLevel > 20 ? lv_color_hex(0xF59E0B) :
                             lv_color_hex(0xEF4444);
        lv_obj_set_style_bg_color(status_bat_bar_fill_, bar_col, 0);
    }

    if (!status.network.transport.empty()) {
        snprintf(buf, sizeof(buf), "📶 Seluler: %s %s",
                 status.network.transport.c_str(), status.network.generation.c_str());
    } else {
        snprintf(buf, sizeof(buf), "📶 Koneksi: Bluetooth BLE");
    }
    if (status_network_label_) lv_label_set_text(status_network_label_, buf);

    if (status.network.signalLevel > 0) {
        const char* sigBars = (status.network.signalLevel >= 4) ? "●●●● (4/4)" :
                              (status.network.signalLevel == 3) ? "●●●○ (3/4)" :
                              (status.network.signalLevel == 2) ? "●●○○ (2/4)" :
                              (status.network.signalLevel == 1) ? "●○○○ (1/4)" : "○○○○ (0/4)";
        snprintf(buf, sizeof(buf), "Sinyal: %s", sigBars);
    } else {
        snprintf(buf, sizeof(buf), "Status: Terhubung Aktif");
    }
    if (status_signal_label_) lv_label_set_text(status_signal_label_, buf);

    // Do NOT automatically switch away from HOME to STATUS; let the user stay on the digital clock!
    lv_unlock();
}

void ChronchiDisplay::showMusic(const ChronchiMusic& music) {
    lv_lock();
    if (music_app_label_) {
        std::string appName = music.app.empty() ? "Chronos Audio" : music.app;
        lv_label_set_text_fmt(music_app_label_, "🎧 %s", appName.c_str());
    }
    if (music_state_label_) {
        bool playing = (music.state == 1);
        lv_label_set_text(music_state_label_, playing ? "▶ Putar" : "⏸ Jeda");
        lv_obj_set_style_text_color(music_state_label_,
            playing ? (light_mode_ ? lv_color_hex(0x16A34A) : lv_color_hex(0x22C55E)) :
                      lv_color_hex(0xF59E0B), 0);
    }
    if (music_title_label_) {
        lv_label_set_text(music_title_label_, music.title.empty() ? "Tidak Ada Media" : music.title.c_str());
    }
    if (music_artist_label_) {
        lv_label_set_text(music_artist_label_, music.artist.empty() ? "Putar musik di ponsel Anda" : music.artist.c_str());
    }
    if (music_bar_fill_) {
        int32_t mw = (music.state == 1) ? 120 : 30;
        lv_obj_set_width(music_bar_fill_, mw);
    }

    // Dynamic Home Screen status update: if music is active, show song on Home!
    if (!google_maps_active_ && home_status_label_ && lv_obj_is_valid(home_status_label_)) {
        if (music.state == 1 && !music.title.empty()) {
            char mbuf[64];
            snprintf(mbuf, sizeof(mbuf), "🎵 %s", music.title.c_str());
            lv_label_set_text(home_status_label_, mbuf);
        }
    }
    lv_unlock();
}

void ChronchiDisplay::showFindWatchAlert(bool active) {
    lv_lock();
    if (active) {
        if (alert_icon_label_) lv_label_set_text(alert_icon_label_, LV_SYMBOL_BELL "  " LV_SYMBOL_WARNING);
        if (alert_title_label_) lv_label_set_text(alert_title_label_, "CARI JAM TANGAN!");
        if (alert_msg_label_) lv_label_set_text(alert_msg_label_, "Ponsel Anda sedang mencari jam ini.\nGetaran & suara aktif.\n\nTekan tombol BOOT untuk matikan, atau stop di HP.");
        if (alert_btn_label_) lv_label_set_text(alert_btn_label_, "✕ TEKAN TOMBOL MATIKAN");
        setScreenState(ALERT);
    } else {
        ChronchiAlert::GetInstance().StopAlert();
        if (state_ == ALERT) showHome();
    }
    lv_unlock();
}

void ChronchiDisplay::showCameraPrompt(bool ready) {
    lv_lock();
    if (ready) {
        if (alert_icon_label_) lv_label_set_text(alert_icon_label_, LV_SYMBOL_IMAGE " KAMERA");
        if (alert_title_label_) lv_label_set_text(alert_title_label_, "KAMERA AKTIF");
        if (alert_msg_label_) lv_label_set_text(alert_msg_label_, "Kamera ponsel Anda siap.\nTekan Tombol Vol Up untuk memotret foto!");
        if (alert_btn_label_) lv_label_set_text(alert_btn_label_, "📸 JEPRET FOTO");
        setScreenState(ALERT);
    } else {
        if (state_ == ALERT) showHome();
    }
    lv_unlock();
}

void ChronchiDisplay::showAlarmAlert(int hour, int minute) {
    lv_lock();
    if (alert_icon_label_) lv_label_set_text(alert_icon_label_, LV_SYMBOL_BELL " ALARM");
    char tbuf[32];
    snprintf(tbuf, sizeof(tbuf), "WAKTU: %02d:%02d", hour, minute);
    if (alert_title_label_) lv_label_set_text(alert_title_label_, tbuf);
    if (alert_msg_label_) lv_label_set_text(alert_msg_label_, "Alarm Berbunyi!\nWaktunya beraktivitas.");
    if (alert_btn_label_) lv_label_set_text(alert_btn_label_, "✕ MATIKAN ALARM");
    setScreenState(ALERT);
    lv_unlock();
}

void ChronchiDisplay::showReminderAlert(const char* title, const char* message) {
    lv_lock();
    if (alert_icon_label_) lv_label_set_text(alert_icon_label_, LV_SYMBOL_BELL " PENGINGAT");
    if (alert_title_label_) lv_label_set_text(alert_title_label_, title ? title : "PENGINGAT");
    if (alert_msg_label_) lv_label_set_text(alert_msg_label_, message ? message : "Waktunya aktivitas!");
    if (alert_btn_label_) lv_label_set_text(alert_btn_label_, "✕ TUTUP");
    setScreenState(ALERT);
    lv_unlock();

    // Auto-dismiss after 8 seconds
    lv_timer_t* timer = lv_timer_create([](lv_timer_t* t) {
        auto* disp = static_cast<ChronchiDisplay*>(lv_timer_get_user_data(t));
        if (disp && disp->state_ == ALERT) {
            disp->showHome();
        }
        lv_timer_del(t);
    }, 8000, this);
    lv_timer_set_repeat_count(timer, 1);
}

void ChronchiDisplay::cycleScreen() {
    lv_lock();
    if (state_ == HOME) {
        setScreenState(WEATHER);
    } else if (state_ == WEATHER) {
        setScreenState(MUSIC);
    } else if (state_ == MUSIC) {
        setScreenState(NAVIGATION);
    } else if (state_ == NAVIGATION) {
        setScreenState(STATUS);
    } else {
        showHome();
    }
    lv_unlock();
}
