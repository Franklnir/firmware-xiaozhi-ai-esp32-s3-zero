#pragma once

#include <esp_log.h>
#include <driver/gpio.h>
#include <lvgl.h>
#include <cmath>
#include <algorithm>
#include <string>
#include <functional>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "audio_codec.h"

class MicTestView {
public:
    using ModeSwitchCallback = std::function<void()>;

    MicTestView() = default;
    ~MicTestView() {
        Stop();
    }

    void Init(int width, int height, AudioCodec* codec, gpio_num_t down_gpio, gpio_num_t boot_gpio, ModeSwitchCallback on_switch) {
        width_ = width;
        height_ = height;
        codec_ = codec;
        down_gpio_ = down_gpio;
        boot_gpio_ = boot_gpio;
        on_switch_ = on_switch;

        ESP_LOGI("MicTestView", "Initializing Mic Test View (%dx%d)...", width_, height_);

        if (codec_) {
            codec_->Start();
            codec_->EnableInput(true);
        }

        // Configure GPIO pins for button detection
        if (down_gpio_ != GPIO_NUM_NC) {
            gpio_config_t conf = {};
            conf.pin_bit_mask = (1ULL << (int)down_gpio_);
            conf.mode = GPIO_MODE_INPUT;
            conf.pull_up_en = GPIO_PULLUP_ENABLE;
            conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            conf.intr_type = GPIO_INTR_DISABLE;
            gpio_config(&conf);
        }
        if (boot_gpio_ != GPIO_NUM_NC) {
            gpio_config_t conf = {};
            conf.pin_bit_mask = (1ULL << (int)boot_gpio_);
            conf.mode = GPIO_MODE_INPUT;
            conf.pull_up_en = GPIO_PULLUP_ENABLE;
            conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
            conf.intr_type = GPIO_INTR_DISABLE;
            gpio_config(&conf);
        }

        CreateUI();

        running_ = true;
        // Start background audio sampling task
        xTaskCreatePinnedToCore(
            AudioTaskEntry,
            "mic_test_audio",
            4096,
            this,
            5,
            &audio_task_handle_,
            1
        );

        // Start LVGL animation & button timer (35ms = ~28 FPS)
        ui_timer_ = lv_timer_create(
            [](lv_timer_t* t) {
                auto* self = static_cast<MicTestView*>(lv_timer_get_user_data(t));
                if (self) {
                    self->UpdateUI();
                    self->PollButtons();
                }
            },
            35,
            this
        );
    }

    void Stop() {
        running_ = false;
        if (ui_timer_) {
            lv_timer_delete(ui_timer_);
            ui_timer_ = nullptr;
        }
        if (audio_task_handle_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            audio_task_handle_ = nullptr;
        }
        if (root_ && lv_obj_is_valid(root_)) {
            lv_obj_del(root_);
            root_ = nullptr;
        }
    }

private:
    struct ButtonTracker {
        int hold_count = 0;
        bool was_pressed = false;
        int click_count = 0;
        int release_wait = 0;
        bool long_press_fired = false;

        int Update(bool pressed, bool& out_long_press) {
            out_long_press = false;
            if (pressed) {
                hold_count++;
                was_pressed = true;
                if (hold_count >= 30 && !long_press_fired) {
                    long_press_fired = true;
                    out_long_press = true;
                }
            } else {
                if (was_pressed) {
                    if (long_press_fired) {
                        long_press_fired = false;
                        click_count = 0;
                    } else if (hold_count >= 1 && hold_count < 30) {
                        click_count++;
                        release_wait = 7; // 350ms window
                    }
                    hold_count = 0;
                    was_pressed = false;
                } else if (click_count > 0) {
                    release_wait--;
                    if (release_wait <= 0) {
                        int final_clicks = click_count;
                        click_count = 0;
                        return final_clicks;
                    }
                }
            }
            return 0;
        }
    };

    int width_ = 240;
    int height_ = 240;
    AudioCodec* codec_ = nullptr;
    gpio_num_t down_gpio_ = GPIO_NUM_NC;
    gpio_num_t boot_gpio_ = GPIO_NUM_NC;
    ModeSwitchCallback on_switch_;

    volatile bool running_ = false;
    TaskHandle_t audio_task_handle_ = nullptr;
    lv_timer_t* ui_timer_ = nullptr;

    // Audio metrics (thread-safe values updated by audio task)
    volatile float left_level_ = 0.0f;       // 0.0 .. 1.0 (smooth envelope on dB scale)
    volatile float right_level_ = 0.0f;      // 0.0 .. 1.0 (smooth envelope on dB scale)
    volatile float left_db_ = -60.0f;        // In dBFS (-60 .. 0 dB)
    volatile float right_db_ = -60.0f;       // In dBFS (-60 .. 0 dB)
    volatile int left_peak_raw_ = 0;
    volatile int right_peak_raw_ = 0;
    volatile bool left_detected_ = false;
    volatile bool right_detected_ = false;
    int left_confirm_cnt_ = 0;
    int right_confirm_cnt_ = 0;

    ButtonTracker down_tracker_;
    ButtonTracker boot_tracker_;

    float anim_phase_ = 0.0f;

    // LVGL widgets
    lv_obj_t* root_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* status_badge_ = nullptr;
    lv_obj_t* status_label_ = nullptr;

    // Left card
    lv_obj_t* left_card_ = nullptr;
    lv_obj_t* left_badge_ = nullptr;
    lv_obj_t* left_bars_[5] = {nullptr};
    lv_obj_t* left_vu_ = nullptr;
    lv_obj_t* left_vol_label_ = nullptr;

    // Right card
    lv_obj_t* right_card_ = nullptr;
    lv_obj_t* right_badge_ = nullptr;
    lv_obj_t* right_bars_[5] = {nullptr};
    lv_obj_t* right_vu_ = nullptr;
    lv_obj_t* right_vol_label_ = nullptr;

    lv_obj_t* footer_hint_ = nullptr;

    static void AudioTaskEntry(void* param) {
        auto* self = static_cast<MicTestView*>(param);
        self->AudioTask();
        vTaskDelete(NULL);
    }

    void AudioTask() {
        constexpr int kSamples = 256;
        std::vector<int16_t> l_buf(kSamples);
        std::vector<int16_t> r_buf(kSamples);
        int log_counter = 0;

        while (running_) {
            if (!codec_) {
                vTaskDelay(pdMS_TO_TICKS(50));
                continue;
            }

            int n = codec_->ReadStereo(l_buf.data(), r_buf.data(), kSamples);
            if (n <= 0) {
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }

            // 1. Calculate DC mean for Left and Right to eliminate hardware bias offset
            int64_t sum_l = 0;
            int64_t sum_r = 0;
            for (int i = 0; i < n; i++) {
                sum_l += l_buf[i];
                sum_r += r_buf[i];
            }
            int32_t mean_l = (int32_t)(sum_l / n);
            int32_t mean_r = (int32_t)(sum_r / n);

            // 2. Calculate pure AC acoustic amplitude and RMS energy
            int32_t peak_ac_l = 0;
            int32_t peak_ac_r = 0;
            int64_t sum_sq_l = 0;
            int64_t sum_sq_r = 0;

            for (int i = 0; i < n; i++) {
                int32_t ac_l = (int32_t)l_buf[i] - mean_l;
                int32_t ac_r = (int32_t)r_buf[i] - mean_r;
                int32_t abs_l = std::abs(ac_l);
                int32_t abs_r = std::abs(ac_r);
                if (abs_l > peak_ac_l) peak_ac_l = abs_l;
                if (abs_r > peak_ac_r) peak_ac_r = abs_r;
                sum_sq_l += (int64_t)ac_l * ac_l;
                sum_sq_r += (int64_t)ac_r * ac_r;
            }

            float rms_l = sqrtf((float)sum_sq_l / n);
            float rms_r = sqrtf((float)sum_sq_r / n);

            // 3. Compute Decibels relative to Full Scale (dBFS)
            // 0 dBFS is peak digital ceiling (32767).
            // Silence is <= -45 dBFS. Normal voice is -28 to -14 dBFS. Loud shout is -8 to 0 dBFS.
            float cur_db_l = (rms_l > 1.0f) ? (20.0f * log10f(rms_l / 32767.0f)) : -60.0f;
            float cur_db_r = (rms_r > 1.0f) ? (20.0f * log10f(rms_r / 32767.0f)) : -60.0f;
            if (cur_db_l < -60.0f) cur_db_l = -60.0f;
            if (cur_db_r < -60.0f) cur_db_r = -60.0f;
            if (cur_db_l > 0.0f) cur_db_l = 0.0f;
            if (cur_db_r > 0.0f) cur_db_r = 0.0f;

            // Smooth the dB measurement so numbers don't flicker harshly
            if (cur_db_l > left_db_) {
                left_db_ = cur_db_l; // instant attack
            } else {
                left_db_ = left_db_ * 0.70f + cur_db_l * 0.30f; // smooth decay
            }

            if (cur_db_r > right_db_) {
                right_db_ = cur_db_r;
            } else {
                right_db_ = right_db_ * 0.70f + cur_db_r * 0.30f;
            }

            left_peak_raw_ = peak_ac_l;
            right_peak_raw_ = peak_ac_r;

            // 4. Presence Detection based on real acoustic activity
            // Silence / disconnected pin: peak_ac is < 80.
            // Voice / speech / clapping: peak_ac is > 220 or dB > -40 dBFS.
            if (peak_ac_l > 220 || cur_db_l > -40.0f) {
                left_confirm_cnt_++;
                if (left_confirm_cnt_ >= 2 || peak_ac_l > 600) {
                    left_detected_ = true;
                }
            }
            if (peak_ac_r > 220 || cur_db_r > -40.0f) {
                right_confirm_cnt_++;
                if (right_confirm_cnt_ >= 2 || peak_ac_r > 600) {
                    right_detected_ = true;
                }
            }

            // 5. Normalized level for VU meter & animations (Logarithmic Decibel Scale!)
            // Standard VU Meter range: -45 dBFS (bottom) to 0 dBFS (top)
            constexpr float kMinDb = -45.0f;
            float target_l = (cur_db_l > kMinDb) ? ((cur_db_l - kMinDb) / -kMinDb) : 0.0f;
            float target_r = (cur_db_r > kMinDb) ? ((cur_db_r - kMinDb) / -kMinDb) : 0.0f;
            target_l = std::max(0.0f, std::min(1.0f, target_l));
            target_r = std::max(0.0f, std::min(1.0f, target_r));

            // Fast attack, smooth decay envelope filter
            if (target_l > left_level_) {
                left_level_ = target_l; // instant response
            } else {
                left_level_ = left_level_ * 0.72f + target_l * 0.28f;
                if (left_level_ < 0.005f) left_level_ = 0.0f;
            }

            if (target_r > right_level_) {
                right_level_ = target_r;
            } else {
                right_level_ = right_level_ * 0.72f + target_r * 0.28f;
                if (right_level_ < 0.005f) right_level_ = 0.0f;
            }

            log_counter++;
            if (log_counter >= 40) { // ~ every 600ms
                log_counter = 0;
                ESP_LOGI("MicTestView", "AC Audio: L=%.1fdB (lvl=%.2f, det=%d), R=%.1fdB (lvl=%.2f, det=%d)",
                         left_db_, left_level_, left_detected_ ? 1 : 0,
                         right_db_, right_level_, right_detected_ ? 1 : 0);
            }

            vTaskDelay(pdMS_TO_TICKS(15));
        }
    }

    void CreateUI() {
        // Full screen root container
        root_ = lv_obj_create(lv_screen_active());
        lv_obj_set_size(root_, width_, height_);
        lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_set_style_bg_color(root_, lv_color_hex(0x060913), 0); // Deep modern dark
        lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(root_, 0, 0);
        lv_obj_set_style_radius(root_, 0, 0);
        lv_obj_set_style_pad_all(root_, 0, 0);
        lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

        // Header Title
        title_label_ = lv_label_create(root_);
        lv_label_set_text(title_label_, LV_SYMBOL_AUDIO "  DUAL MIC TEST MONITOR");
        lv_obj_set_style_text_color(title_label_, lv_color_hex(0x38BDF8), 0); // Vibrant sky blue
        lv_obj_set_style_text_font(title_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(title_label_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 10 : 6);

        // Top Status Badge (Smart Summary)
        status_badge_ = lv_obj_create(root_);
        lv_obj_set_size(status_badge_, width_ - 16, (height_ >= 280) ? 28 : 24);
        lv_obj_align(status_badge_, LV_ALIGN_TOP_MID, 0, (height_ >= 280) ? 30 : 25);
        lv_obj_set_style_radius(status_badge_, 6, 0);
        lv_obj_set_style_pad_all(status_badge_, 0, 0);
        lv_obj_set_style_border_width(status_badge_, 1, 0);
        lv_obj_clear_flag(status_badge_, LV_OBJ_FLAG_SCROLLABLE);

        status_label_ = lv_label_create(status_badge_);
        lv_label_set_text(status_label_, "BICARA / TERIAK KE MIC...");
        lv_obj_set_style_text_font(status_label_, &lv_font_montserrat_14, 0);
        lv_obj_center(status_label_);

        // Layout Card dimensions
        int card_w = (width_ - 18) / 2; // ~111 px
        int card_h = (height_ >= 280) ? 172 : 142;
        int card_y = (height_ >= 280) ? 64 : 53;

        // --- 1. LEFT MIC CARD ---
        left_card_ = lv_obj_create(root_);
        lv_obj_set_size(left_card_, card_w, card_h);
        lv_obj_set_pos(left_card_, 6, card_y);
        lv_obj_set_style_bg_color(left_card_, lv_color_hex(0x0C1527), 0);
        lv_obj_set_style_bg_opa(left_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(left_card_, 1, 0);
        lv_obj_set_style_border_color(left_card_, lv_color_hex(0x1E3A8A), 0);
        lv_obj_set_style_radius(left_card_, 10, 0);
        lv_obj_set_style_pad_all(left_card_, 4, 0);
        lv_obj_clear_flag(left_card_, LV_OBJ_FLAG_SCROLLABLE);

        // Header Left Card
        lv_obj_t* l_title = lv_label_create(left_card_);
        lv_label_set_text(l_title, "MIC (KIRI)");
        lv_obj_set_style_text_color(l_title, lv_color_hex(0x93C5FD), 0);
        lv_obj_set_style_text_font(l_title, &lv_font_montserrat_14, 0);
        lv_obj_align(l_title, LV_ALIGN_TOP_MID, 0, 2);

        left_badge_ = lv_label_create(left_card_);
        lv_label_set_text(left_badge_, "MENUNGGU SUARA");
        lv_obj_set_style_text_color(left_badge_, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(left_badge_, &lv_font_montserrat_14, 0);
        lv_obj_align(left_badge_, LV_ALIGN_TOP_MID, 0, 18);

        // Wave Spectrum Bars (5 bars)
        int bar_w = 11;
        int bar_gap = 5;
        int total_wave_w = 5 * bar_w + 4 * bar_gap; // 55 + 20 = 75px
        int start_x = (card_w - total_wave_w) / 2;
        int wave_base_y = (height_ >= 280) ? 122 : 100;

        for (int i = 0; i < 5; i++) {
            left_bars_[i] = lv_obj_create(left_card_);
            lv_obj_set_size(left_bars_[i], bar_w, 4);
            lv_obj_set_pos(left_bars_[i], start_x + i * (bar_w + bar_gap), wave_base_y);
            lv_obj_set_style_radius(left_bars_[i], 3, 0);
            lv_obj_set_style_bg_color(left_bars_[i], lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_border_width(left_bars_[i], 0, 0);
            lv_obj_clear_flag(left_bars_[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        // Horizontal Level VU Bar
        left_vu_ = lv_bar_create(left_card_);
        lv_obj_set_size(left_vu_, card_w - 16, 5);
        lv_obj_align(left_vu_, LV_ALIGN_BOTTOM_MID, 0, (height_ >= 280) ? -20 : -16);
        lv_bar_set_range(left_vu_, 0, 100);
        lv_bar_set_value(left_vu_, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(left_vu_, lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_bg_color(left_vu_, lv_color_hex(0x38BDF8), LV_PART_INDICATOR);

        // Volume label (Decibels)
        left_vol_label_ = lv_label_create(left_card_);
        lv_label_set_text(left_vol_label_, "<-45 dB");
        lv_obj_set_style_text_color(left_vol_label_, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(left_vol_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(left_vol_label_, LV_ALIGN_BOTTOM_MID, 0, -2);

        // --- 2. RIGHT MIC CARD ---
        right_card_ = lv_obj_create(root_);
        lv_obj_set_size(right_card_, card_w, card_h);
        lv_obj_set_pos(right_card_, width_ - card_w - 6, card_y);
        lv_obj_set_style_bg_color(right_card_, lv_color_hex(0x130D24), 0);
        lv_obj_set_style_bg_opa(right_card_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(right_card_, 1, 0);
        lv_obj_set_style_border_color(right_card_, lv_color_hex(0x581C87), 0);
        lv_obj_set_style_radius(right_card_, 10, 0);
        lv_obj_set_style_pad_all(right_card_, 4, 0);
        lv_obj_clear_flag(right_card_, LV_OBJ_FLAG_SCROLLABLE);

        // Header Right Card
        lv_obj_t* r_title = lv_label_create(right_card_);
        lv_label_set_text(r_title, "MIC (KANAN)");
        lv_obj_set_style_text_color(r_title, lv_color_hex(0xE9D5FF), 0);
        lv_obj_set_style_text_font(r_title, &lv_font_montserrat_14, 0);
        lv_obj_align(r_title, LV_ALIGN_TOP_MID, 0, 2);

        right_badge_ = lv_label_create(right_card_);
        lv_label_set_text(right_badge_, "MENUNGGU SUARA");
        lv_obj_set_style_text_color(right_badge_, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(right_badge_, &lv_font_montserrat_14, 0);
        lv_obj_align(right_badge_, LV_ALIGN_TOP_MID, 0, 18);

        // Right Wave Spectrum Bars (5 bars)
        for (int i = 0; i < 5; i++) {
            right_bars_[i] = lv_obj_create(right_card_);
            lv_obj_set_size(right_bars_[i], bar_w, 4);
            lv_obj_set_pos(right_bars_[i], start_x + i * (bar_w + bar_gap), wave_base_y);
            lv_obj_set_style_radius(right_bars_[i], 3, 0);
            lv_obj_set_style_bg_color(right_bars_[i], lv_color_hex(0xC084FC), 0);
            lv_obj_set_style_border_width(right_bars_[i], 0, 0);
            lv_obj_clear_flag(right_bars_[i], LV_OBJ_FLAG_SCROLLABLE);
        }

        // Horizontal Level VU Bar Right
        right_vu_ = lv_bar_create(right_card_);
        lv_obj_set_size(right_vu_, card_w - 16, 5);
        lv_obj_align(right_vu_, LV_ALIGN_BOTTOM_MID, 0, (height_ >= 280) ? -20 : -16);
        lv_bar_set_range(right_vu_, 0, 100);
        lv_bar_set_value(right_vu_, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(right_vu_, lv_color_hex(0x1E293B), 0);
        lv_obj_set_style_bg_color(right_vu_, lv_color_hex(0xC084FC), LV_PART_INDICATOR);

        // Volume label Right (Decibels)
        right_vol_label_ = lv_label_create(right_card_);
        lv_label_set_text(right_vol_label_, "<-45 dB");
        lv_obj_set_style_text_color(right_vol_label_, lv_color_hex(0x94A3B8), 0);
        lv_obj_set_style_text_font(right_vol_label_, &lv_font_montserrat_14, 0);
        lv_obj_align(right_vol_label_, LV_ALIGN_BOTTOM_MID, 0, -2);

        // Bottom Footer Hint
        footer_hint_ = lv_label_create(root_);
        lv_label_set_text(footer_hint_, "Bicara/Teriak untuk tes | 2x Down: Ganti");
        lv_obj_set_style_text_color(footer_hint_, lv_color_hex(0x64748B), 0);
        lv_obj_set_style_text_font(footer_hint_, &lv_font_montserrat_14, 0);
        lv_obj_align(footer_hint_, LV_ALIGN_BOTTOM_MID, 0, (height_ >= 280) ? -8 : -3);
    }

    void UpdateUI() {
        if (!root_ || !lv_obj_is_valid(root_)) return;

        anim_phase_ += 0.22f;
        if (anim_phase_ > 6.28318f) anim_phase_ -= 6.28318f;

        bool has_l = left_detected_;
        bool has_r = right_detected_;
        float lvl_l = left_level_;
        float lvl_r = right_level_;

        // 1. Update Top Overall Status Badge
        if (has_l && has_r) {
            lv_label_set_text(status_label_, LV_SYMBOL_OK " DUAL MIC (L + R) STEREO");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x34D399), 0);
            lv_obj_set_style_bg_color(status_badge_, lv_color_hex(0x064E3B), 0);
            lv_obj_set_style_border_color(status_badge_, lv_color_hex(0x10B981), 0);
        } else if (has_l && !has_r) {
            lv_label_set_text(status_label_, LV_SYMBOL_RIGHT " 1 MIC (HANYA KIRI)");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x38BDF8), 0);
            lv_obj_set_style_bg_color(status_badge_, lv_color_hex(0x0C4A6E), 0);
            lv_obj_set_style_border_color(status_badge_, lv_color_hex(0x0284C7), 0);
        } else if (!has_l && has_r) {
            lv_label_set_text(status_label_, LV_SYMBOL_LEFT " 1 MIC (HANYA KANAN)");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFBBF24), 0);
            lv_obj_set_style_bg_color(status_badge_, lv_color_hex(0x78350F), 0);
            lv_obj_set_style_border_color(status_badge_, lv_color_hex(0xD97706), 0);
        } else {
            lv_label_set_text(status_label_, LV_SYMBOL_AUDIO " BICARA / TERIAK KE MIC...");
            lv_obj_set_style_text_color(status_label_, lv_color_hex(0x94A3B8), 0);
            lv_obj_set_style_bg_color(status_badge_, lv_color_hex(0x1E293B), 0);
            lv_obj_set_style_border_color(status_badge_, lv_color_hex(0x475569), 0);
        }

        // 2. Update Left Card Badges & Indicators
        if (has_l) {
            lv_label_set_text(left_badge_, "TERDETEKSI");
            lv_obj_set_style_text_color(left_badge_, lv_color_hex(0x34D399), 0);
            lv_obj_set_style_border_color(left_card_, lv_color_hex(0x0284C7), 0);
        } else {
            lv_label_set_text(left_badge_, "MENUNGGU SUARA");
            lv_obj_set_style_text_color(left_badge_, lv_color_hex(0x64748B), 0);
            lv_obj_set_style_border_color(left_card_, lv_color_hex(0x1E293B), 0);
        }

        // 3. Update Right Card Badges & Indicators
        if (has_r) {
            lv_label_set_text(right_badge_, "TERDETEKSI");
            lv_obj_set_style_text_color(right_badge_, lv_color_hex(0x34D399), 0);
            lv_obj_set_style_border_color(right_card_, lv_color_hex(0x9333EA), 0);
        } else {
            lv_label_set_text(right_badge_, "MENUNGGU SUARA");
            lv_obj_set_style_text_color(right_badge_, lv_color_hex(0x64748B), 0);
            lv_obj_set_style_border_color(right_card_, lv_color_hex(0x1E293B), 0);
        }

        // 4. Animate Soundwave Bars
        int max_h = (height_ >= 280) ? 68 : 54;
        int base_y = (height_ >= 280) ? 122 : 100;
        float weights[5] = {0.45f, 0.78f, 1.0f, 0.78f, 0.45f};

        for (int i = 0; i < 5; i++) {
            // Left bar animation
            int bar_h_l = 4;
            if (lvl_l > 0.02f) {
                float ripple_l = 0.15f * sinf(anim_phase_ + i * 1.25f);
                float effective_l = lvl_l * (weights[i] + ripple_l);
                bar_h_l = std::max(4, std::min(max_h, (int)(effective_l * max_h)));
            } else if (has_l) {
                bar_h_l = 4 + (int)(1.2f * (sinf(anim_phase_ + i * 0.8f) + 1.0f));
            }
            lv_obj_set_height(left_bars_[i], bar_h_l);
            lv_obj_set_y(left_bars_[i], base_y - bar_h_l);

            // Left Bar color transition based on loudness
            if (lvl_l >= 0.70f) {
                lv_obj_set_style_bg_color(left_bars_[i], lv_color_hex(0xEF4444), 0); // Red on loud shout!
            } else if (lvl_l >= 0.25f) {
                lv_obj_set_style_bg_color(left_bars_[i], lv_color_hex(0xFBBF24), 0); // Amber on speech
            } else {
                lv_obj_set_style_bg_color(left_bars_[i], lv_color_hex(has_l ? 0x38BDF8 : 0x334155), 0);
            }

            // Right bar animation
            int bar_h_r = 4;
            if (lvl_r > 0.02f) {
                float ripple_r = 0.15f * sinf(anim_phase_ + i * 1.25f + 1.57f);
                float effective_r = lvl_r * (weights[i] + ripple_r);
                bar_h_r = std::max(4, std::min(max_h, (int)(effective_r * max_h)));
            } else if (has_r) {
                bar_h_r = 4 + (int)(1.2f * (sinf(anim_phase_ + i * 0.8f + 1.57f) + 1.0f));
            }
            lv_obj_set_height(right_bars_[i], bar_h_r);
            lv_obj_set_y(right_bars_[i], base_y - bar_h_r);

            // Right Bar color transition based on loudness
            if (lvl_r >= 0.70f) {
                lv_obj_set_style_bg_color(right_bars_[i], lv_color_hex(0xEF4444), 0); // Red on loud shout!
            } else if (lvl_r >= 0.25f) {
                lv_obj_set_style_bg_color(right_bars_[i], lv_color_hex(0xFBBF24), 0);
            } else {
                lv_obj_set_style_bg_color(right_bars_[i], lv_color_hex(has_r ? 0xC084FC : 0x334155), 0);
            }
        }

        // 5. Update VU Bars and Decibel Labels
        int pct_l = (int)(lvl_l * 100.0f);
        int pct_r = (int)(lvl_r * 100.0f);
        if (pct_l < 2) pct_l = 0;
        if (pct_r < 2) pct_r = 0;

        lv_bar_set_value(left_vu_, pct_l, LV_ANIM_OFF);
        lv_bar_set_value(right_vu_, pct_r, LV_ANIM_OFF);

        char buf_l[32];
        if (left_db_ <= -45.0f || !has_l) {
            snprintf(buf_l, sizeof(buf_l), "<-45 dB");
            lv_obj_set_style_text_color(left_vol_label_, lv_color_hex(0x64748B), 0);
        } else {
            snprintf(buf_l, sizeof(buf_l), "%.1f dB", left_db_);
            if (left_db_ >= -6.0f) {
                lv_obj_set_style_text_color(left_vol_label_, lv_color_hex(0xEF4444), 0); // Red on loud shout
            } else if (left_db_ >= -20.0f) {
                lv_obj_set_style_text_color(left_vol_label_, lv_color_hex(0xFBBF24), 0); // Amber on clear voice
            } else {
                lv_obj_set_style_text_color(left_vol_label_, lv_color_hex(0x38BDF8), 0); // Cyan on soft sound
            }
        }
        lv_label_set_text(left_vol_label_, buf_l);

        char buf_r[32];
        if (right_db_ <= -45.0f || !has_r) {
            snprintf(buf_r, sizeof(buf_r), "<-45 dB");
            lv_obj_set_style_text_color(right_vol_label_, lv_color_hex(0x64748B), 0);
        } else {
            snprintf(buf_r, sizeof(buf_r), "%.1f dB", right_db_);
            if (right_db_ >= -6.0f) {
                lv_obj_set_style_text_color(right_vol_label_, lv_color_hex(0xEF4444), 0);
            } else if (right_db_ >= -20.0f) {
                lv_obj_set_style_text_color(right_vol_label_, lv_color_hex(0xFBBF24), 0);
            } else {
                lv_obj_set_style_text_color(right_vol_label_, lv_color_hex(0xC084FC), 0);
            }
        }
        lv_label_set_text(right_vol_label_, buf_r);
    }

    void PollButtons() {
        // 1. Check Down button (GPIO 46) -> Double Click: Switch back to XiaoZhi!
        if (down_gpio_ != GPIO_NUM_NC) {
            bool pressed = (gpio_get_level(down_gpio_) == 0);
            bool lp = false;
            int clicks = down_tracker_.Update(pressed, lp);
            if (clicks >= 2) {
                ESP_LOGI("MicTestView", "Down button double-clicked -> switching mode!");
                if (on_switch_) {
                    on_switch_();
                }
            } else if (clicks == 1) {
                // Single click resets detection if user wants to re-test
                ESP_LOGI("MicTestView", "Down button single-clicked -> resetting mic detection state");
                left_detected_ = false;
                right_detected_ = false;
                left_confirm_cnt_ = 0;
                right_confirm_cnt_ = 0;
            }
        }

        // 2. Check BOOT button (GPIO 0) -> Triple Click: Switch back to XiaoZhi!
        if (boot_gpio_ != GPIO_NUM_NC) {
            bool pressed = (gpio_get_level(boot_gpio_) == 0);
            bool lp = false;
            int clicks = boot_tracker_.Update(pressed, lp);
            if (clicks >= 3) {
                ESP_LOGI("MicTestView", "BOOT button 3x clicked -> switching mode!");
                if (on_switch_) {
                    on_switch_();
                }
            } else if (clicks == 1) {
                ESP_LOGI("MicTestView", "BOOT button single-clicked -> resetting mic detection state");
                left_detected_ = false;
                right_detected_ = false;
                left_confirm_cnt_ = 0;
                right_confirm_cnt_ = 0;
            }
        }
    }
};
