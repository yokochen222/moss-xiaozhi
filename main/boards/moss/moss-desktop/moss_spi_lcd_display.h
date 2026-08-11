#ifndef MOSS_SPI_LCD_DISPLAY_H
#define MOSS_SPI_LCD_DISPLAY_H

#include "lcd_display.h"

#include <string>

// 0.96" LCD board display: splash task owns the panel SPI (no LVGL).
class MossSpiLcdDisplay : public LcdDisplay {
public:
    MossSpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_handle_t panel, int width,
                      int height, int offset_x, int offset_y, bool mirror_x, bool mirror_y,
                      bool swap_xy);

    ~MossSpiLcdDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void ShowNotification(const char* title, const char* body, int duration_ms = 3000);
    void UpdateStatusBar(bool update_all = false) override;

    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void SetEmotion(const char* emotion) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override;
    void DismissDialog() override;

    void SetScreenOn(bool on);
    bool IsScreenOn() const { return screen_on_; }

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t* splash_out_buf_ = nullptr;
    std::string current_status_;
    bool screen_on_ = true;

    void StartSplashLoop();
};

#endif  // MOSS_SPI_LCD_DISPLAY_H
