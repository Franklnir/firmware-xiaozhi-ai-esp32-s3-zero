#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "display/lcd_display.h"
#include "boards/common/backlight.h"
#include "application.h"
#include "button.h"
#include "adc_battery_monitor.h"
#include "led/single_led.h"
#include "config.h"
#include "assets/lang_config.h"

#include "settings.h"
#include <esp_log.h>
#include <esp_bt.h>
#include <esp_lvgl_port.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/ledc.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <vector>
#include <esp_attr.h>
#include <cmath>
#include <algorithm>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <esp_adc/adc_cali_scheme.h>

#include "chronchi/chronchi_alert.h"
#include "chronchi/chronchi_ble.h"
#include "chronchi/chronchi_display.h"
#include "chronchi/mode_selector.h"

// Shared battery state in RTC memory that persists across mode switches and soft-resets
RTC_DATA_ATTR static int s_shared_battery_level = -1;
RTC_DATA_ATTR static bool s_shared_charging = false;

#undef lv_lock
#undef lv_unlock
#define lv_lock() lvgl_port_lock(0)
#define lv_unlock() lvgl_port_unlock()

#define TAG "WaveshareEsp32S3ZeroBoard"

class WaveshareEsp32S3ZeroBoard : public WifiBoard {
private:
    Button boot_button_;
    Button chat_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    adc_oneshot_unit_handle_t adc_handle_ = nullptr;
    adc_cali_handle_t adc_cali_handle_ = nullptr;
    bool has_adc_cali_ = false;
    Display* display_ = nullptr;
    Backlight* backlight_ = nullptr;
    i2c_master_bus_handle_t display_i2c_bus_ = nullptr;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    bool chronchi_mode_ = false;
    ChronchiNotification last_notif_;
    ChronchiNavigation last_nav_;
    ChronchiWeather last_weather_;
    ChronchiPhoneStatus last_phone_status_;
    ChronchiMusic last_music_;
    ChronchiBle* chronchi_ble_ = nullptr;
    ChronchiDisplay* chronchi_display_ = nullptr;
    bool ble_connected_flag_ = false;
    bool ble_notif_flag_ = false;
    bool ble_nav_flag_ = false;
    bool ble_weather_flag_ = false;
    bool ble_status_flag_ = false;
    bool ble_music_flag_ = false;
    bool ble_find_watch_flag_ = false;
    bool ble_find_watch_active_ = false;
    bool ble_camera_flag_ = false;
    bool ble_camera_ready_ = false;

    void SwitchToMode(DeviceMode target_mode) {
        const char* name = "XiaoZhi";
        if (target_mode == DeviceMode::CHRONCHI) name = "Chronchi";

        ESP_LOGI(TAG, "Switching mode to %s...", name);
        Settings settings("chronchi", true);
        settings.SetInt("mode", (int32_t)target_mode);

        if (chronchi_display_) {
            ChronchiNotification n;
            n.app = "System";
            n.title = "Mode Switch";
            n.message = std::string("Beralih ke mode ") + name + "...";
            chronchi_display_->showNotification(n);
        } else if (display_) {
            char buf[64];
            snprintf(buf, sizeof(buf), "Switching to %s...", name);
            display_->ShowNotification(buf, 1500);
        }

        esp_timer_handle_t timer;
        esp_timer_create_args_t timer_args = {
            .callback = [](void* arg) {
                esp_restart();
            },
            .name = "restart_timer"
        };
        esp_timer_create(&timer_args, &timer);
        esp_timer_start_once(timer, 400 * 1000);
    }

    void InitializeDisplay() {
        int width    = DISPLAY_WIDTH;    // 240
        int height   = DISPLAY_HEIGHT;   // 280
        int offset_x = DISPLAY_OFFSET_X;  // 0
        int offset_y = DISPLAY_OFFSET_Y;  // 20
        bool mirror_x = DISPLAY_MIRROR_X; // false
        bool mirror_y = DISPLAY_MIRROR_Y; // false
        bool swap_xy  = DISPLAY_SWAP_XY;  // false

        int scl_pin = (int)DISPLAY_SCL_PIN; // 9
        int sda_pin = (int)DISPLAY_SDA_PIN; // 8
        int dc_pin  = (int)DISPLAY_DC_PIN;  // 10
        int rst_pin = (int)DISPLAY_RST_PIN; // 11
        int cs_pin  = (int)DISPLAY_CS_PIN;  // 12
        int bl_pin  = (int)DISPLAY_BL_PIN;  // 13

        // 1. Force Pin 13 Backlight to full power immediately
        gpio_reset_pin((gpio_num_t)bl_pin);
        gpio_set_direction((gpio_num_t)bl_pin, GPIO_MODE_OUTPUT);
        gpio_set_drive_capability((gpio_num_t)bl_pin, GPIO_DRIVE_CAP_3);
        gpio_set_level((gpio_num_t)bl_pin, 1);

        ESP_LOGI(TAG, "Initializing ST7789 (240x280) SCL=%d, SDA=%d, DC=%d, RST=%d, CS=%d, BL=%d",
                 scl_pin, sda_pin, dc_pin, rst_pin, cs_pin, bl_pin);

        // 2. Configure Pin Drive Capabilities
        gpio_reset_pin((gpio_num_t)scl_pin);
        gpio_reset_pin((gpio_num_t)sda_pin);
        gpio_reset_pin((gpio_num_t)dc_pin);
        gpio_reset_pin((gpio_num_t)cs_pin);
        gpio_reset_pin((gpio_num_t)rst_pin);

        gpio_set_drive_capability((gpio_num_t)scl_pin, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)sda_pin, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)dc_pin, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)cs_pin, GPIO_DRIVE_CAP_3);
        gpio_set_drive_capability((gpio_num_t)rst_pin, GPIO_DRIVE_CAP_3);

        // 3. Clean Hardware Reset Pulse on RST Pin 11
        gpio_set_direction((gpio_num_t)rst_pin, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)rst_pin, 0);
        vTaskDelay(pdMS_TO_TICKS(50));
        gpio_set_level((gpio_num_t)rst_pin, 1);
        vTaskDelay(pdMS_TO_TICKS(150));

        // 4. Initialize SPI Bus (SPI2_HOST)
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = (gpio_num_t)sda_pin;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = (gpio_num_t)scl_pin;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = width * height * sizeof(uint16_t);

        esp_err_t ret = spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize SPI bus: %s", esp_err_to_name(ret));
            display_ = nullptr;
            return;
        }

        // 5. Initialize Panel IO (SPI Mode 0, 20MHz)
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = (gpio_num_t)cs_pin;
        io_config.dc_gpio_num = (gpio_num_t)dc_pin;
        io_config.spi_mode = 0;
        io_config.pclk_hz = 20 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;

        ret = esp_lcd_new_panel_io_spi(SPI2_HOST, &io_config, &panel_io_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create panel io: %s", esp_err_to_name(ret));
            display_ = nullptr;
            return;
        }

        // 6. Install ST7789 driver
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = (gpio_num_t)rst_pin;
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;

        ret = esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "Failed to create ST7789 panel: %s", esp_err_to_name(ret));
            display_ = nullptr;
            return;
        }

        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_invert_color(panel_, true);
        esp_lcd_panel_disp_on_off(panel_, true);

        esp_lcd_panel_swap_xy(panel_, swap_xy);
        esp_lcd_panel_mirror(panel_, mirror_x, mirror_y);

        // 7. Visual Hardware Test Pattern: Draw Green Screen for 1 frame
        // This confirms SPI transmission and ST7789 pixels work before LVGL takes over
        std::vector<uint16_t> green_line(width, 0x07E0); // Bright Green
        for (int y = 0; y < height; y++) {
            esp_lcd_panel_draw_bitmap(panel_, 0, y, width, y + 1, green_line.data());
        }

        // 8. Create SpiLcdDisplay for XiaoZhi LVGL UI
        display_ = new SpiLcdDisplay(panel_io_, panel_, width, height,
                                     offset_x, offset_y, mirror_x, mirror_y, swap_xy);
        ((LcdDisplay*)display_)->SetIsSt7789(true);

        // 9. Attach PWM Backlight at 100% duty cycle
        backlight_ = new PwmBacklight((gpio_num_t)bl_pin, false, 5000);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 1023);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
        backlight_->SetBrightness(100);

        ESP_LOGI(TAG, "ST7789 Display (240x280) and Backlight on Pin 13 initialized successfully!");
    }

    void InitializeBatteryMonitor() {
        if (BATTERY_ADC_PIN != GPIO_NUM_NC) {
            adc_oneshot_unit_init_cfg_t init_cfg = {
                .unit_id = BATTERY_ADC_UNIT,
                .ulp_mode = ADC_ULP_MODE_DISABLE,
            };
            if (adc_oneshot_new_unit(&init_cfg, &adc_handle_) == ESP_OK) {
                adc_oneshot_chan_cfg_t chan_cfg = {
                    .atten = ADC_ATTEN_DB_12,
                    .bitwidth = ADC_BITWIDTH_DEFAULT,
                };
                adc_oneshot_config_channel(adc_handle_, BATTERY_ADC_CHANNEL, &chan_cfg);

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
                adc_cali_curve_fitting_config_t cali_config = {
                    .unit_id = BATTERY_ADC_UNIT,
                    .chan = BATTERY_ADC_CHANNEL,
                    .atten = ADC_ATTEN_DB_12,
                    .bitwidth = ADC_BITWIDTH_DEFAULT,
                };
                if (adc_cali_create_scheme_curve_fitting(&cali_config, &adc_cali_handle_) == ESP_OK) {
                    has_adc_cali_ = true;
                }
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
                adc_cali_line_fitting_config_t cali_config = {
                    .unit_id = BATTERY_ADC_UNIT,
                    .atten = ADC_ATTEN_DB_12,
                    .bitwidth = ADC_BITWIDTH_DEFAULT,
                };
                if (adc_cali_create_scheme_line_fitting(&cali_config, &adc_cali_handle_) == ESP_OK) {
                    has_adc_cali_ = true;
                }
#endif
                ESP_LOGI(TAG, "Direct calibrated battery monitor initialized on GPIO %d (cali=%s)",
                         (int)BATTERY_ADC_PIN, has_adc_cali_ ? "yes" : "no");
            }
        }
    }

    void InitializeButtons() {
        auto chat_handler = [this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        };

        boot_button_.OnClick(chat_handler);
        boot_button_.OnDoubleClick([this]() {
            ESP_LOGI(TAG, "BOOT button double click: switching to Chronchi mode");
            SwitchToMode(DeviceMode::CHRONCHI);
        });
        boot_button_.OnMultipleClick([this]() {
            ESP_LOGI(TAG, "BOOT button 3x click: switching to Chronchi mode");
            SwitchToMode(DeviceMode::CHRONCHI);
        }, 3);

        if (CHAT_BUTTON_GPIO != GPIO_NUM_NC) {
            chat_button_.OnClick(chat_handler);
            ESP_LOGI(TAG, "Chat button initialized on GPIO %d", (int)CHAT_BUTTON_GPIO);
        }

        if (VOLUME_UP_BUTTON_GPIO != GPIO_NUM_NC) {
            volume_up_button_.OnClick([this]() {
                auto codec = GetAudioCodec();
                auto volume = codec->output_volume() + 10;
                if (volume > 100) {
                    volume = 100;
                }
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            });

            volume_up_button_.OnLongPress([this]() {
                GetAudioCodec()->SetOutputVolume(100);
                GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
            });
            ESP_LOGI(TAG, "Volume UP button initialized on GPIO %d", (int)VOLUME_UP_BUTTON_GPIO);
        }

        if (VOLUME_DOWN_BUTTON_GPIO != GPIO_NUM_NC) {
            volume_down_button_.OnClick([this]() {
                auto codec = GetAudioCodec();
                auto volume = codec->output_volume() - 10;
                if (volume < 0) {
                    volume = 0;
                }
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            });

            volume_down_button_.OnDoubleClick([this]() {
                ESP_LOGI(TAG, "Volume DOWN double click: switching to Chronchi mode");
                SwitchToMode(DeviceMode::CHRONCHI);
            });

            volume_down_button_.OnLongPress([this]() {
                GetAudioCodec()->SetOutputVolume(0);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            });
            ESP_LOGI(TAG, "Volume DOWN button initialized on GPIO %d", (int)VOLUME_DOWN_BUTTON_GPIO);
        }
    }

public:
    WaveshareEsp32S3ZeroBoard() :
        boot_button_(BOOT_BUTTON_GPIO),
        chat_button_(CHAT_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        
        InitializeDisplay();

        DeviceMode mode = ModeSelector::CheckBoot(BOOT_BUTTON_GPIO);
        if (gpio_get_level(BOOT_BUTTON_GPIO) == 0) {
            ESP_LOGI(TAG, "BOOT button held at boot -> switching to Chronchi mode");
            mode = DeviceMode::CHRONCHI;
        }

        if (mode == DeviceMode::CHRONCHI) {
            ESP_LOGI(TAG, "=== CHRONCHI MODE (boot) ===");
            chronchi_mode_ = true;
            InitializeBatteryMonitor();
            auto backlight = GetBacklight();
            if (backlight != nullptr) {
                backlight->SetBrightness(100);
            }
            return;
        }

        ESP_LOGI(TAG, "=== XIAOZHI MODE ===");
        chronchi_mode_ = false;
        ESP_LOGI(TAG, "Releasing Bluetooth controller memory in XiaoZhi mode (~80KB SRAM freed)");
        esp_bt_controller_mem_release(ESP_BT_MODE_BTDM);
        InitializeBatteryMonitor();
        InitializeButtons();

        // Ensure volume is active (restores volume if previously near 0 in NVS)
        if (GetAudioCodec()->output_volume() < 80) {
            GetAudioCodec()->SetOutputVolume(90);
        }
    }

    bool IsChronchiMode() override { return chronchi_mode_; }

    void StartChronchi() override {
        ESP_LOGI(TAG, "Initializing Chronchi mode...");
        vTaskDelay(pdMS_TO_TICKS(500)); // Wait for LVGL to be ready

        ChronchiAlert::GetInstance().Initialize(GPIO_NUM_NC, BUILTIN_LED_GPIO);

        chronchi_display_ = new ChronchiDisplay();
        chronchi_display_->init(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        chronchi_display_->showHome();

        // Update initial battery status immediately
        int init_batt_level = 0;
        bool init_chg = false, init_dis = false;
        if (GetBatteryLevel(init_batt_level, init_chg, init_dis)) {
            chronchi_display_->updateBattery(init_batt_level, init_chg);
        }

        // Ensure audio codec is initialized and output is enabled for alerts & notifications
        auto* codec = GetAudioCodec();
        if (codec) {
            codec->Start();
            codec->SetOutputVolume(100);
            codec->EnableOutput(true);
            ESP_LOGI(TAG, "Audio codec started and output enabled for Chronchi notifications/alarms");
        }

        chronchi_ble_ = new ChronchiBle();
        uint8_t screen_conf = (DISPLAY_HEIGHT >= 280) ? 10 : 1;
        chronchi_ble_->setScreenConfig(screen_conf);
        chronchi_ble_->begin("Chronos ESP32");

        chronchi_ble_->setConnectionCallback([this](bool connected) {
            ESP_LOGI(TAG, "Chronchi BLE: %s", connected ? "Connected" : "Disconnected");
            ble_connected_flag_ = connected;
            if (connected && chronchi_ble_) {
                chronchi_ble_->syncRequest();
                int level = 0;
                bool chg = false, dis = false;
                if (GetBatteryLevel(level, chg, dis)) {
                    chronchi_ble_->setBattery(level, chg);
                }
            }
        });

        chronchi_ble_->setNotificationCallback([this](const ChronchiNotification& notif) {
            ESP_LOGI(TAG, "Notification received: [%s] %s - %s", notif.app.c_str(), notif.title.c_str(), notif.message.c_str());
            last_notif_ = notif;
            ble_notif_flag_ = true;
            // Always ring sound and vibration for notifications as requested by user
            ChronchiAlert::GetInstance().VibrateNotification();
        });

        chronchi_ble_->setNavigationCallback([this](const ChronchiNavigation& nav) {
            ESP_LOGI(TAG, "Navigation: %s - %s", nav.title.c_str(), nav.directions.c_str());
            last_nav_ = nav;
            ble_nav_flag_ = true;
        });

        chronchi_ble_->setWeatherCallback([this](const ChronchiWeather& weather) {
            ESP_LOGI(TAG, "Weather: %.0f°C %s", weather.temperatureC, weather.condition.c_str());
            last_weather_ = weather;
            ble_weather_flag_ = true;
        });

        chronchi_ble_->setPhoneStatusCallback([this](const ChronchiPhoneStatus& status) {
            ESP_LOGI(TAG, "Phone: %d%% %s %s", status.batteryLevel,
                     status.network.transport.c_str(), status.network.generation.c_str());
            last_phone_status_ = status;
            ble_status_flag_ = true;
        });

        chronchi_ble_->setRingerCallback([this](const std::string& caller, bool active) {
            ESP_LOGI(TAG, "Chronos Call: %s (active: %d)", caller.c_str(), active);
            if (active) {
                ChronchiAlert::GetInstance().StartCallAlert();
                ChronchiNotification call_notif;
                call_notif.icon = 0x01;
                call_notif.app = "Phone Call";
                call_notif.title = "Incoming Call";
                call_notif.message = caller.empty() ? "Incoming Call" : caller;
                time_t now = time(nullptr);
                struct tm tm_info;
                localtime_r(&now, &tm_info);
                char tbuf[16];
                snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
                call_notif.time = tbuf;
                last_notif_ = call_notif;
                ble_notif_flag_ = true;
            } else {
                ChronchiAlert::GetInstance().StopAlert();
            }
        });

        chronchi_ble_->setMusicCallback([this](const ChronchiMusic& music) {
            ESP_LOGI(TAG, "Chronos Music: %s - %s (%s)", music.artist.c_str(), music.title.c_str(), music.app.c_str());
            last_music_ = music;
            ble_music_flag_ = true;
        });

        chronchi_ble_->setWatchfaceCallback([this](uint32_t color, uint32_t layout) {
            ESP_LOGI(TAG, "Watchface customized via Chronos: color=0x%06lX layout=%lu",
                     (unsigned long)color, (unsigned long)layout);
            lv_lock();
            if (chronchi_display_) {
                chronchi_display_->setWatchfaceTheme(color, layout);
            }
            lv_unlock();
        });

        chronchi_ble_->setFindWatchCallback([this](bool active) {
            ESP_LOGI(TAG, "Chronos Find Watch: %d", active);
            ble_find_watch_active_ = active;
            ble_find_watch_flag_ = true;
            if (active) {
                ChronchiAlert::GetInstance().StartFindDeviceAlarm();
            } else {
                ChronchiAlert::GetInstance().StopAlert();
            }
        });

        chronchi_ble_->setCameraStateCallback([this](bool ready) {
            ESP_LOGI(TAG, "Chronos Camera Ready: %d", ready);
            ble_camera_ready_ = ready;
            ble_camera_flag_ = true;
        });

        chronchi_ble_->setTimeFormatCallback([this](bool is_24h) {
            ESP_LOGI(TAG, "Chronos Time Format 24h: %d", is_24h);
            lv_lock();
            if (chronchi_display_) {
                chronchi_display_->set24HourFormat(is_24h);
            }
            lv_unlock();
        });

        chronchi_ble_->setAlarmCallback([this](uint8_t index, const ChronchiAlarm& alarm) {
            ESP_LOGI(TAG, "Chronos Alarm #%d: %02d:%02d enabled=%d", index, alarm.hour, alarm.minute, alarm.enabled);
        });

        chronchi_ble_->setLocationCallback([this](const ChronchiLocation& loc) {
            ESP_LOGI(TAG, "Chronos Location: %s, %s (lat=%.3f, lon=%.3f)", loc.city.c_str(), loc.country.c_str(), loc.latitude, loc.longitude);
            if (loc.valid && !loc.city.empty()) {
                last_weather_.location = loc.city;
                ble_weather_flag_ = true;
            }
        });

        chronchi_ble_->setReminderCallback([this](const std::string& type, const ChronchiReminder& reminder) {
            ESP_LOGI(TAG, "Chronos Reminder updated: %s (enabled=%d, interval=%d min)", type.c_str(), reminder.enabled, reminder.intervalMinutes);
        });

        ESP_LOGI(TAG, "Chronchi started. BLE: Chronos ESP32");

        // LVGL timer to check BLE flags and update display (runs in LVGL context)
        lv_timer_create(
            [](lv_timer_t* t) {
                auto* board = static_cast<WaveshareEsp32S3ZeroBoard*>(lv_timer_get_user_data(t));
                if (!board || !board->chronchi_display_)
                    return;

                board->chronchi_display_->updateBtStatus(board->ble_connected_flag_);

                if (board->ble_notif_flag_) {
                    board->chronchi_display_->showNotification(board->last_notif_);
                    board->ble_notif_flag_ = false;
                }
                if (board->ble_nav_flag_) {
                    if (board->last_nav_.active) {
                        board->chronchi_display_->showNavigation(board->last_nav_);
                    } else {
                        board->chronchi_display_->showHome();
                    }
                    board->ble_nav_flag_ = false;
                }
                if (board->ble_weather_flag_) {
                    board->chronchi_display_->showWeather(board->last_weather_);
                    board->chronchi_display_->updateHomeWeather(board->last_weather_);
                    board->ble_weather_flag_ = false;
                }
                if (board->ble_status_flag_) {
                    board->chronchi_display_->showPhoneStatus(board->last_phone_status_);
                    board->ble_status_flag_ = false;
                }
                if (board->ble_music_flag_) {
                    board->chronchi_display_->showMusic(board->last_music_);
                    board->ble_music_flag_ = false;
                }
                if (board->ble_find_watch_flag_) {
                    board->chronchi_display_->showFindWatchAlert(board->ble_find_watch_active_);
                    board->ble_find_watch_flag_ = false;
                }
                if (board->ble_camera_flag_) {
                    board->chronchi_display_->showCameraPrompt(board->ble_camera_ready_);
                    board->ble_camera_flag_ = false;
                }
            },
            200, this);

        // Buttons in Chronchi mode
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = (1ULL << (int)BOOT_BUTTON_GPIO);
        if (CHAT_BUTTON_GPIO != GPIO_NUM_NC) {
            io_conf.pin_bit_mask |= (1ULL << (int)CHAT_BUTTON_GPIO);
        }
        if (VOLUME_UP_BUTTON_GPIO != GPIO_NUM_NC) {
            io_conf.pin_bit_mask |= (1ULL << (int)VOLUME_UP_BUTTON_GPIO);
        }
        if (VOLUME_DOWN_BUTTON_GPIO != GPIO_NUM_NC) {
            io_conf.pin_bit_mask |= (1ULL << (int)VOLUME_DOWN_BUTTON_GPIO);
        }
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io_conf);

        struct SingleButtonTracker {
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
                    if (hold_count >= 30 && !long_press_fired) { // 30 * 50ms = 1.5s
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
                            release_wait = 7; // wait 350ms (7 * 50ms)
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

        struct ChronchiMultiButtonState {
            SingleButtonTracker btn_boot;  // BOOT (GPIO 0)
            SingleButtonTracker btn_chat;  // Chat / Action (GPIO 14)
            SingleButtonTracker btn_up;    // Vol Up (GPIO 15)
            SingleButtonTracker btn_down;  // Vol Down (GPIO 16)
            WaveshareEsp32S3ZeroBoard* board;
        };
        auto* btn_state = new ChronchiMultiButtonState();
        btn_state->board = this;

        lv_timer_create(
            [](lv_timer_t* t) {
                auto* state = static_cast<ChronchiMultiButtonState*>(lv_timer_get_user_data(t));

                // 1. Physical button state sampling
                bool boot_pressed = (gpio_get_level(BOOT_BUTTON_GPIO) == 0);
                bool chat_pressed = (CHAT_BUTTON_GPIO != GPIO_NUM_NC && gpio_get_level(CHAT_BUTTON_GPIO) == 0);
                bool up_pressed   = (VOLUME_UP_BUTTON_GPIO != GPIO_NUM_NC && gpio_get_level(VOLUME_UP_BUTTON_GPIO) == 0);
                bool down_pressed = (VOLUME_DOWN_BUTTON_GPIO != GPIO_NUM_NC && gpio_get_level(VOLUME_DOWN_BUTTON_GPIO) == 0);

                // --- INSTANT DISMISS FOR ALARM / FIND WATCH / CALL ALERT ---
                // Saat mode 'Temukan Jam Tangan' aktif atau alarm bunyi, menekan tombol apa saja langsung mematikan alarm seketika!
                bool is_alerting = ChronchiAlert::GetInstance().IsAlarmActive() || 
                                   state->board->ble_find_watch_active_ ||
                                   (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::ALERT);

                if (is_alerting && (boot_pressed || chat_pressed || up_pressed || down_pressed)) {
                    ESP_LOGI("Chronchi", "Alarm / Find Watch alert stopped INSTANTLY by button press!");
                    ChronchiAlert::GetInstance().StopAlert();
                    state->board->ble_find_watch_active_ = false;
                    if (state->board->chronchi_ble_) {
                        state->board->chronchi_ble_->findPhone(false);
                    }
                    if (state->board->chronchi_display_) {
                        state->board->chronchi_display_->showHome();
                    }
                    // Reset all button trackers so release doesn't trigger subsequent clicks
                    state->btn_boot = {};
                    state->btn_chat = {};
                    state->btn_up = {};
                    state->btn_down = {};
                    return;
                }

                // Normal button event processing
                bool boot_lp = false;
                int clicks_boot = state->btn_boot.Update(boot_pressed, boot_lp);
                if (boot_lp) {
                    if (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::MUSIC && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "BOOT Long Press (on Music screen): Music Previous Track");
                        state->board->chronchi_ble_->musicControl(MUSIC_PREVIOUS);
                        ChronchiAlert::GetInstance().VibrateNotification();
                    } else if (state->board->ble_find_watch_active_) {
                        ESP_LOGI("Chronchi", "BOOT Long Press: Dismissing Find Watch Alert");
                        ChronchiAlert::GetInstance().StopAlert();
                        state->board->ble_find_watch_active_ = false;
                        if (state->board->chronchi_ble_) {
                            state->board->chronchi_ble_->findPhone(false);
                        }
                    } else {
                        ESP_LOGI("Chronchi", "BOOT Long Press (1.5s): Finding Phone!");
                        if (state->board->chronchi_ble_) {
                            state->board->chronchi_ble_->findPhone(true);
                        }
                        if (state->board->chronchi_display_) {
                            ChronchiNotification fn;
                            fn.icon = 0xC0;
                            fn.app = "Find Phone";
                            fn.title = "Mencari Ponsel...";
                            fn.message = "Perintah cari ponsel telah dikirim via Bluetooth!";
                            time_t now = time(nullptr);
                            struct tm tm_info;
                            localtime_r(&now, &tm_info);
                            char tbuf[16];
                            snprintf(tbuf, sizeof(tbuf), "%02d:%02d", tm_info.tm_hour, tm_info.tm_min);
                            fn.time = tbuf;
                            state->board->chronchi_display_->showNotification(fn);
                        }
                        ChronchiAlert::GetInstance().VibrateNotification();
                    }
                } else if (clicks_boot >= 4) {
                    ESP_LOGI("Chronchi", "BOOT 4x click: Toggle Dark/Light color mode");
                    if (state->board->chronchi_display_) {
                        state->board->chronchi_display_->toggleColorMode();
                    }
                } else if (clicks_boot == 3) {
                    ESP_LOGI("Chronchi", "BOOT 3x click: Switching to XiaoZhi mode!");
                    state->board->SwitchToMode(DeviceMode::XIAOZHI);
                } else if (clicks_boot == 2) {
                    if (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::MUSIC && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "BOOT 2x click (on Music screen): Music Next Track");
                        state->board->chronchi_ble_->musicControl(MUSIC_NEXT);
                    } else if (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::WEATHER && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "BOOT 2x click (on Weather screen): Requesting Sync from phone");
                        state->board->chronchi_ble_->syncRequest();
                        ChronchiAlert::GetInstance().VibrateNotification();
                    } else {
                        ESP_LOGI("Chronchi", "BOOT 2x click: Cycling Watchface theme");
                        if (state->board->chronchi_display_) {
                            state->board->chronchi_display_->cycleTheme();
                        }
                    }
                } else if (clicks_boot == 1) {
                    if (state->board->chronchi_ble_ && state->board->chronchi_ble_->isCameraReady()) {
                        ESP_LOGI("Chronchi", "BOOT 1x click: Capture Photo (Camera active)");
                        state->board->chronchi_ble_->capturePhoto();
                        ChronchiAlert::GetInstance().VibrateNotification();
                    } else if (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::MUSIC && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "BOOT 1x click (on Music screen): Music Play/Pause toggle");
                        state->board->chronchi_ble_->musicControl(MUSIC_TOGGLE);
                    } else {
                        ESP_LOGI("Chronchi", "BOOT 1x click: Cycling screen");
                        if (state->board->chronchi_display_) {
                            state->board->chronchi_display_->cycleScreen();
                        }
                    }
                }

                // 2. Chat Button (GPIO 14 - External Button 1)
                if (CHAT_BUTTON_GPIO != GPIO_NUM_NC) {
                    bool chat_pressed = (gpio_get_level(CHAT_BUTTON_GPIO) == 0);
                    bool chat_lp = false;
                    int clicks_chat = state->btn_chat.Update(chat_pressed, chat_lp);
                    if (chat_lp && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "Chat button long press: Music Play/Pause toggle");
                        state->board->chronchi_ble_->musicControl(MUSIC_TOGGLE);
                    } else if (clicks_chat >= 2) {
                        ESP_LOGI("Chronchi", "Chat button 2x click: Show Home Watchface");
                        if (state->board->chronchi_display_) {
                            state->board->chronchi_display_->showHome();
                        }
                    } else if (clicks_chat == 1 && state->board->chronchi_ble_) {
                        if (state->board->chronchi_ble_->isCameraReady()) {
                            ESP_LOGI("Chronchi", "Chat button 1x click: capture photo");
                            state->board->chronchi_ble_->capturePhoto();
                            ChronchiAlert::GetInstance().VibrateNotification();
                        } else if (state->board->chronchi_display_ && state->board->chronchi_display_->getScreenState() == ChronchiDisplay::MUSIC) {
                            ESP_LOGI("Chronchi", "Chat button 1x click: Music Play/Pause toggle");
                            state->board->chronchi_ble_->musicControl(MUSIC_TOGGLE);
                        } else {
                            ESP_LOGI("Chronchi", "Chat button 1x click: Cycle Screen");
                            if (state->board->chronchi_display_) {
                                state->board->chronchi_display_->cycleScreen();
                            }
                        }
                    }
                }

                // 3. Volume UP Button (GPIO 15 - External Button 2)
                if (VOLUME_UP_BUTTON_GPIO != GPIO_NUM_NC) {
                    bool up_pressed = (gpio_get_level(VOLUME_UP_BUTTON_GPIO) == 0);
                    bool up_lp = false;
                    int clicks_up = state->btn_up.Update(up_pressed, up_lp);
                    if (up_lp && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "Vol UP long press: Music Volume Up");
                        state->board->chronchi_ble_->musicControl(VOLUME_UP);
                    } else if (clicks_up >= 2) {
                        ESP_LOGI("Chronchi", "Vol UP 2x click: Cycle Watchface Theme");
                        if (state->board->chronchi_display_) {
                            state->board->chronchi_display_->cycleTheme();
                        }
                    } else if (clicks_up == 1 && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "Vol UP 1x click: Music Next");
                        state->board->chronchi_ble_->musicControl(MUSIC_NEXT);
                    }
                }

                // 4. Volume DOWN Button (GPIO 16 - External Button 3)
                if (VOLUME_DOWN_BUTTON_GPIO != GPIO_NUM_NC) {
                    bool down_pressed = (gpio_get_level(VOLUME_DOWN_BUTTON_GPIO) == 0);
                    bool down_lp = false;
                    int clicks_down = state->btn_down.Update(down_pressed, down_lp);
                    if (down_lp && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "Vol DOWN long press: Music Volume Down");
                        state->board->chronchi_ble_->musicControl(VOLUME_DOWN);
                    } else if (clicks_down >= 2) {
                        ESP_LOGI("Chronchi", "Vol DOWN 2x click: Switching to XiaoZhi mode!");
                        state->board->SwitchToMode(DeviceMode::XIAOZHI);
                    } else if (clicks_down == 1 && state->board->chronchi_ble_) {
                        ESP_LOGI("Chronchi", "Vol DOWN 1x click: Music Previous");
                        state->board->chronchi_ble_->musicControl(MUSIC_PREVIOUS);
                    }
                }
            },
            50, btn_state);

        // LVGL timer for time & reminder updates (1000ms)
        lv_timer_create(
            [](lv_timer_t* t) {
                auto* board = static_cast<WaveshareEsp32S3ZeroBoard*>(lv_timer_get_user_data(t));
                if (!board || !board->chronchi_display_) return;
                time_t now = time(NULL);
                struct tm* tm_info = localtime(&now);
                board->chronchi_display_->updateTime(tm_info->tm_hour, tm_info->tm_min, tm_info->tm_sec);

                if (tm_info->tm_sec == 0 && board->chronchi_ble_) {
                    for (uint8_t i = 0; i < 8; i++) {
                        ChronchiAlarm al = board->chronchi_ble_->getAlarm(i);
                        if (al.enabled && al.hour == tm_info->tm_hour && al.minute == tm_info->tm_min) {
                            bool repeat_match = false;
                            if (al.repeat == 0 || al.repeat == 0x80 || al.repeat == 0x7F) {
                                repeat_match = true;
                            } else {
                                int day = tm_info->tm_wday; // 0=Sun..6=Sat
                                if (day == 0) {
                                    repeat_match = (al.repeat & (1 << 6)) != 0;
                                } else {
                                    repeat_match = (al.repeat & (1 << (day - 1))) != 0;
                                }
                            }
                            if (repeat_match) {
                                ESP_LOGI("Chronchi", "Alarm fired! #%d %02d:%02d", i, al.hour, al.minute);
                                board->chronchi_display_->showAlarmAlert(al.hour, al.minute);
                                ChronchiAlert::GetInstance().StartFindDeviceAlarm();
                                break;
                            }
                        }
                    }

                    // Sedentary Reminder
                    static int last_sedentary_min = -1;
                    ChronchiReminder sed = board->chronchi_ble_->getSedentaryReminder();
                    if (sed.enabled && sed.intervalMinutes > 0) {
                        int cur_min = tm_info->tm_hour * 60 + tm_info->tm_min;
                        int s_min = sed.startHour * 60 + sed.startMinute;
                        int e_min = sed.endHour * 60 + sed.endMinute;
                        bool in_window = (s_min <= e_min) ? (cur_min >= s_min && cur_min <= e_min) : (cur_min >= s_min || cur_min <= e_min);
                        if (in_window) {
                            if (last_sedentary_min < 0 || (cur_min - last_sedentary_min) >= sed.intervalMinutes) {
                                last_sedentary_min = cur_min;
                                ESP_LOGI("Chronchi", "Sedentary reminder fired!");
                                board->chronchi_display_->showReminderAlert("Waktunya Bergerak!", "Anda sudah cukup lama duduk.\nYuk berdiri dan regangkan tubuh!");
                                ChronchiAlert::GetInstance().VibrateNotification();
                            }
                        }
                    }

                    // Water Reminder
                    static int last_water_min = -1;
                    ChronchiReminder wtr = board->chronchi_ble_->getWaterReminder();
                    if (wtr.enabled && wtr.intervalMinutes > 0) {
                        int cur_min = tm_info->tm_hour * 60 + tm_info->tm_min;
                        int s_min = wtr.startHour * 60 + wtr.startMinute;
                        int e_min = wtr.endHour * 60 + wtr.endMinute;
                        bool in_window = (s_min <= e_min) ? (cur_min >= s_min && cur_min <= e_min) : (cur_min >= s_min || cur_min <= e_min);
                        if (in_window) {
                            if (last_water_min < 0 || (cur_min - last_water_min) >= wtr.intervalMinutes) {
                                last_water_min = cur_min;
                                ESP_LOGI("Chronchi", "Water reminder fired!");
                                board->chronchi_display_->showReminderAlert("Waktunya Minum Air!", "Tetap terhidrasi sepanjang hari.\nMinum segelas air putih.");
                                ChronchiAlert::GetInstance().VibrateNotification();
                            }
                        }
                    }
                }
            },
            1000, this);

        // LVGL timer for battery update (2000ms)
        lv_timer_create(
            [](lv_timer_t* t) {
                auto* board = static_cast<WaveshareEsp32S3ZeroBoard*>(lv_timer_get_user_data(t));
                int level = 0;
                bool chg = false;
                bool dis = false;
                if (board->GetBatteryLevel(level, chg, dis)) {
                    if (board->chronchi_display_)
                        board->chronchi_display_->updateBattery(level, chg);
                    if (board->chronchi_ble_)
                        board->chronchi_ble_->setBattery(level, chg);
                }
            },
            2000, this);
    }

    void ChronchiLoop() override {
        if (!chronchi_mode_ || !chronchi_ble_)
            return;

        chronchi_ble_->loop();

        // Check navigation state
        static uint32_t last_crc = 0;
        ChronchiNavigation nav = chronchi_ble_->getNavigation();
        if (nav.active && nav.iconCRC != last_crc) {
            last_crc = nav.iconCRC;
            if (chronchi_display_ && nav.hasIcon)
                chronchi_display_->setNavigationIcon(nav.icon, 288);
        }
        if (!nav.active)
            last_crc = 0;

        vTaskDelay(pdMS_TO_TICKS(100));
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, I2S_STD_SLOT_BOTH,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN, I2S_STD_SLOT_LEFT);
#else
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        if (display_ == nullptr) {
            return Board::GetDisplay();
        }
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        return backlight_;
    }

    virtual bool GetBatteryLevel(int &level, bool& charging, bool& discharging) override {
        if (adc_handle_ == nullptr) {
            if (s_shared_battery_level >= 0) {
                level = s_shared_battery_level;
                charging = s_shared_charging;
                discharging = !charging;
                return true;
            }
            return false;
        }

        // 32-sample oversampling with trimmed outlier rejection to eliminate Wi-Fi RF burst dips
        constexpr int kSamples = 32;
        int raw_vals[kSamples];
        for (int i = 0; i < kSamples; i++) {
            adc_oneshot_read(adc_handle_, BATTERY_ADC_CHANNEL, &raw_vals[i]);
        }
        std::sort(raw_vals, raw_vals + kSamples);

        // Discard 6 lowest and 6 highest samples, average the middle 20
        int valid_sum = 0;
        constexpr int kTrim = 6;
        for (int i = kTrim; i < kSamples - kTrim; i++) {
            valid_sum += raw_vals[i];
        }
        int avg_raw = valid_sum / (kSamples - 2 * kTrim);

        int pin_mv = 0;
        if (has_adc_cali_) {
            adc_cali_raw_to_voltage(adc_cali_handle_, avg_raw, &pin_mv);
        } else {
            pin_mv = avg_raw * 3100 / 4095;
        }

        // 100k / 100k voltage divider: battery_mv = pin_mv * 2
        int batt_mv = pin_mv * 2;

        // Wi-Fi load compensation:
        // In XiaoZhi mode (!chronchi_mode_), active Wi-Fi and 240MHz CPU draw ~180mA,
        // causing ~110mV voltage drop on small LiPo batteries.
        // In Chronchi mode (BLE only), current draw is negligible (~15mA).
        if (!chronchi_mode_) {
            batt_mv += 110;
        }

        // Charging detection:
        charging = (batt_mv >= 4200);
        discharging = !charging;

        // Accurate LiPo discharge curve (mV -> %)
        struct BattPoint { int mv; int pct; };
        static const BattPoint kCurve[] = {
            { 4180, 100 },
            { 4060, 90 },
            { 3960, 80 },
            { 3880, 70 },
            { 3810, 60 },
            { 3750, 50 },
            { 3690, 40 },
            { 3630, 30 },
            { 3560, 20 },
            { 3450, 10 },
            { 3300, 0 }
        };
        constexpr int kPoints = sizeof(kCurve) / sizeof(kCurve[0]);

        int calculated_pct = 0;
        if (batt_mv >= kCurve[0].mv) {
            calculated_pct = 100;
        } else if (batt_mv <= kCurve[kPoints - 1].mv) {
            calculated_pct = 0;
        } else {
            for (int i = 0; i < kPoints - 1; i++) {
                if (batt_mv <= kCurve[i].mv && batt_mv >= kCurve[i + 1].mv) {
                    float ratio = (float)(batt_mv - kCurve[i + 1].mv) / (float)(kCurve[i].mv - kCurve[i + 1].mv);
                    calculated_pct = kCurve[i + 1].pct + (int)(ratio * (kCurve[i].pct - kCurve[i + 1].pct) + 0.5f);
                    break;
                }
            }
        }

        // Smooth stabilization & rate limiting:
        // Never ratchets down permanently like adc_battery_estimation!
        if (s_shared_battery_level < 0) {
            s_shared_battery_level = calculated_pct;
        } else {
            float smoothed = 0.92f * s_shared_battery_level + 0.08f * calculated_pct;
            int next_level = (int)(smoothed + 0.5f);

            // Rate limiter: maximum 1% change per update to prevent sudden jumps
            if (next_level > s_shared_battery_level) {
                if (charging || (next_level - s_shared_battery_level > 3)) {
                    s_shared_battery_level += 1;
                }
            } else if (next_level < s_shared_battery_level) {
                s_shared_battery_level -= 1;
            }
        }

        s_shared_charging = charging;
        level = s_shared_battery_level;
        return true;
    }
};

DECLARE_BOARD(WaveshareEsp32S3ZeroBoard);
