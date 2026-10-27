#ifndef LCD_DISPLAY_H
#define LCD_DISPLAY_H

#include "gif/lvgl_gif.h"
#include "lvgl_display.h"

#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <atomic>
#include <memory>

#define PREVIEW_IMAGE_DURATION_MS 5000

class LcdDisplay : public LvglDisplay {
protected:
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    lv_draw_buf_t draw_buf_;
    lv_obj_t* top_bar_ = nullptr;
    lv_obj_t* status_bar_ = nullptr;
    lv_obj_t* content_ = nullptr;
    lv_obj_t* container_ = nullptr;
    lv_obj_t* side_bar_ = nullptr;
    lv_obj_t* bottom_bar_ = nullptr;
    lv_obj_t* preview_image_ = nullptr;
    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* emoji_image_ = nullptr;
    std::unique_ptr<LvglGif> gif_controller_ = nullptr;
    lv_obj_t* emoji_box_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    esp_timer_handle_t preview_timer_ = nullptr;
    std::unique_ptr<LvglImage> preview_image_cached_ = nullptr;
    bool hide_subtitle_ = false;  // Control whether to hide chat messages/subtitles
    bool is_zoomed_ = false;  // Camera zoom state (backwards compatibility)
    int zoom_level_ = 1;      // Camera zoom level (1x to 10x)

    // DJ Music visualizer
    lv_obj_t* music_container_ = nullptr;
    lv_obj_t* dj_icon_label_ = nullptr;
    lv_obj_t* music_title_label_ = nullptr;
    lv_obj_t* music_sub_label_ = nullptr;
    lv_obj_t* music_duration_label_ = nullptr;
    lv_obj_t* music_progress_bar_ = nullptr;
    lv_obj_t* dj_bars_[7] = {nullptr};
    lv_timer_t* music_anim_timer_ = nullptr;
    int music_anim_phase_ = 0;
    bool music_visualizer_active_ = false;
    int music_total_seconds_ = 0;
    int music_current_seconds_ = 0;

    int ui_theme_id_ = 1;
    bool is_st7789_ = false;
    int emoji_scale_ = 4;
    lv_obj_t* avatar_circle_ = nullptr;

    void SetupUI_Theme1();
    void SetupUI_Theme2();
    void DestroyUI();
    void SetChatMessage_Theme1(const char* role, const char* content);
    void SetChatMessage_Theme2(const char* role, const char* content);
    void SetPreviewImage_Theme1(std::unique_ptr<LvglImage> image);
    void SetPreviewImage_Theme2(std::unique_ptr<LvglImage> image);
    void ClearChatMessages_Theme1();
    void ClearChatMessages_Theme2();

    void CreateMusicVisualizerUI();

    void InitializeLcdThemes();
    virtual bool Lock(int timeout_ms = 0) override;
    virtual void Unlock() override;

protected:
    // Add protected constructor
    LcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
               int height);

public:
    ~LcdDisplay();
    virtual void SetStatus(const char* status) override;
    virtual void SetEmotion(const char* emotion) override;
    virtual void SetChatMessage(const char* role, const char* content) override;
    virtual void ClearChatMessages() override;
    virtual void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void ShowCaptureFlash();
    void SetZoom(bool zoomed);
    void SetZoomLevel(int level);
    int GetZoomLevel() const { return zoom_level_; }
    int ZoomIn();
    int ZoomOut();
    virtual void SetupUI() override;
    // Add theme switching function
    virtual void SetTheme(Theme* theme) override;
    virtual void SetBrightness(uint8_t brightness) override;

    virtual int GetUiThemeId() const override { return ui_theme_id_; }
    virtual bool SetUiThemeId(int theme_id) override;
    virtual void SetIsSt7789(bool is_st7789) override { is_st7789_ = is_st7789; }
    virtual bool IsSt7789() const override { return is_st7789_; }
    virtual int GetEmojiScale() const override { return emoji_scale_; }
    virtual void SetEmojiScale(int scale) override;

    // Set whether to hide chat messages/subtitles
    void SetHideSubtitle(bool hide);

    // DJ Music Visualizer
    virtual void StartMusicVisualizer(const char* title = nullptr, int total_sec = 0) override;
    virtual void SetMusicProgress(int current_sec, int total_sec) override;
    virtual void StopMusicVisualizer() override;
    virtual bool IsMusicVisualizerActive() const override { return music_visualizer_active_; }
    void AnimateMusicVisualizer();
};

// SPI LCD display
class SpiLcdDisplay : public LcdDisplay {
public:
    SpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// RGB LCD display
class RgbLcdDisplay : public LcdDisplay {
public:
    RgbLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                  int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                  bool swap_xy);
};

// MIPI LCD display
class MipiLcdDisplay : public LcdDisplay {
public:
    MipiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                   int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                   bool swap_xy);
};

#endif  // LCD_DISPLAY_H
