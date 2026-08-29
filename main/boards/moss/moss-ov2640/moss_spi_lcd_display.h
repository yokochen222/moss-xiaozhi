#ifndef MOSS_SPI_LCD_DISPLAY_H
#define MOSS_SPI_LCD_DISPLAY_H

#include "device/face_tracker.h"
#include "lcd_display.h"

#include <functional>
#include <mutex>
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
    void PauseBackgroundAnimation(bool wait_bus = true);
    void ResumeBackgroundAnimation();
    // Pause code scroll without the photo scroll_paused_ latch.
    void YieldLcdForDvp();
    void UnpauseLcdAfterFailedDvp();
    // Face-track camera init: kill the scroll task so SPI DMA is free (MCP Start is not prio 1).
    void StopLcdForTracking();

    // true: 对话中停掉熄屏计时并亮屏; false: 回到待命后重新 60s 计时.
    void OnConversationKeepAwake(std::function<void(bool keep_awake)> cb) {
        keep_awake_cb_ = std::move(cb);
    }

    // Face-track UI: stop code scroll and show detection HUD.
    void EnterFaceTrackMode();
    void ExitFaceTrackMode();
    void UpdateFaceTrackOverlay(const FaceTrackerStatus& status);

protected:
    bool Lock(int timeout_ms = 0) override;
    void Unlock() override;

private:
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t* splash_out_buf_ = nullptr;
    std::string current_status_;
    bool screen_on_ = true;
    bool face_track_mode_ = false;
    bool scroll_paused_ = false;
    std::mutex face_ui_mutex_;
    std::function<void(bool)> keep_awake_cb_;

    void StartSplashLoop();
};

#endif  // MOSS_SPI_LCD_DISPLAY_H
