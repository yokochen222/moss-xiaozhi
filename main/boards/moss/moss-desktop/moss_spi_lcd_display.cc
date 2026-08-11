#include "moss_spi_lcd_display.h"
#include "assets/lang_config.h"
#include "splash_player.h"
#include "moss_splash.h"

#include <cstring>
#include <cstdlib>
#include <mutex>
#include <esp_log.h>
#include <esp_err.h>
#include <esp_heap_caps.h>

#define TAG "MossSpiLcd"

namespace {
// splash task 读的 dialog state. 由 SetStatus/ShowNotification 更新.
// sticky: active=true 时, splash 每帧在屏幕底部画一次 title + underline.
// active=false 时, splash 只画 EAF 背景, 不画 dialog.
//
// 优先级系统:
//   kPriorityState(1): 聆听说/待命/连接中 → 不弹窗
//   kPriorityInfo(2):  普通通知 → 弹窗
//   kPriorityWarning(3): 警告/升级
//   kPriorityError(4):   错误/失败
//
// 更高优先级可以覆盖低优先级弹窗 (但静默状态不会覆盖)。

// 科技终端风格配色 (RGB565)
// 青色/蓝绿 - 默认弹窗 (科幻终端感)
static constexpr uint16_t kDialogColor   = 0x07D0;  // cyan-ish (0x07D0)
// 红色 - 错误/失败
static constexpr uint16_t kAlertColor    = 0xF800;  // red
// 琥珀色 - 警告/升级
static constexpr uint16_t kWarnColor     = 0xFC00;  // orange-amber
// 绿色 - 成功
static constexpr uint16_t kSuccessColor  = 0x07E0;  // green
// 白色 - 纯白高亮
static constexpr uint16_t kWhiteColor    = 0xFFFF;

moss_splash::DialogState s_dialog{};

// 去除 printf 格式占位符 (%d/%s 等), 避免屏幕上显示 "正在初始化...%d".
// 所有字符 (ASCII + UTF-8 中文) 原样保留, font_puhui_basic_14_1 可直接渲染.
const char* map_status_to_ascii(const char* s) {
    if (s == nullptr) return "";
    static char cleaned[moss_splash::DIALOG_TITLE_MAX + 1];
    int n = 0;
    for (const unsigned char* p = (const unsigned char*)s; *p && n < moss_splash::DIALOG_TITLE_MAX; p++) {
        // 跳过 %d, %s, %. 等格式说明符, 保留后面的内容.
        if (*p == '%' && (p[1] == 'd' || p[1] == 's' || p[1] == '.')) {
            // 跳过 %d 中的 %, 让循环在 d 处继续 (跳过 %d 两个字节)
            if (p[1] == 'd') {
                p++;  // 跳过 %
                continue;  // 下轮在 'd', for 会 p++ 到下一字节
            }
            // %s 或 %. : 跳过 %, 下轮处理 s 或 .
            p++;
            continue;
        }
        cleaned[n++] = (char)*p;
    }
    cleaned[n] = '\0';
    return cleaned;
}

// 状态文本 -> 颜色 + 优先级分类.
// 后端传入的字符串可能是英文 key (例如 "ERROR", "WARNING") 或中文文本 (例如 "错误", "警告").
void classify_color_priority(const char* s, uint16_t& color, uint8_t& priority) {
    if (s == nullptr) {
        color = kDialogColor;
        priority = moss_splash::kPriorityInfo;
        return;
    }
    // ERROR 类 (红色, 最高优先级)
    if (strstr(s, "ERROR") || strstr(s, "error")
        || strstr(s, "FAIL") || strstr(s, "fail")
        || strstr(s, "NO SIM") || strstr(s, "NO NET")
        || strstr(s, "TIMEOUT") || strstr(s, "timeout")
        || strstr(s, "错误") || strstr(s, "失败")
        || strstr(s, "MUTED") || strstr(s, "BATTERY_NEED_CHARGE")
        || strstr(s, "电量不足") || strstr(s, "无法连接")
        || strstr(s, "请充电")) {
        color = kAlertColor;
        priority = moss_splash::kPriorityError;
    }
    // WARNING 类 (amber, 中高优先级)
    else if (strstr(s, "WARNING") || strstr(s, "warning")
        || strstr(s, "OTA") || strstr(s, "ota")
        || strstr(s, "BATTERY_LOW")
        || strstr(s, "MAX VOL") || strstr(s, "升级")
        || strstr(s, "警告")) {
        color = kWarnColor;
        priority = moss_splash::kPriorityWarning;
    }
    // SUCCESS 类 (绿色, 普通优先级)
    else if (strstr(s, "SUCCESS") || strstr(s, "success")
        || strstr(s, "HELLO") || strstr(s, "BATTERY_FULL")
        || strstr(s, "NEW VER") || strstr(s, "已连接")
        || strstr(s, "已满") || strstr(s, "你好")
        || strstr(s, "成功")) {
        color = kSuccessColor;
        priority = moss_splash::kPriorityInfo;
    }
    // 默认 (amber)
    else {
        color = kDialogColor;
        priority = moss_splash::kPriorityInfo;
    }
}

// 判断状态字符串是否静默 (不需要弹窗).
// 聆听说/待命/连接中等状态不弹窗，只更新内部状态.
bool is_silent_status(const char* s) {
    if (s == nullptr) return false;
    // 注意: 后端传的实际上是 Lang::Strings::LISTENING 等常量字符串值,
    // 不是英文 key 名. 所以这里比较实际文本内容.
    // 聆听说/待命/连接中 → 不弹窗 (只在串口 log 显示)
    if (strstr(s, "聆听") || strstr(s, "说话") || strstr(s, "待命")
        || strstr(s, "连接中") || strstr(s, "已连接")) {
        return true;
    }
    return false;
}

}  // namespace

MossSpiLcdDisplay::MossSpiLcdDisplay(esp_lcd_panel_io_handle_t panel_io,
                                     esp_lcd_panel_handle_t panel,
                                     int width, int height,
                                     int offset_x, int offset_y,
                                     bool mirror_x, bool mirror_y,
                                     bool swap_xy)
    : LcdDisplay(panel_io, panel, width, height) {
    (void)offset_x; (void)offset_y; (void)mirror_x; (void)mirror_y; (void)swap_xy;
    panel_ = panel;

    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_, true));

    // 启动一次性 splash (start.eaf), 它会直接画 panel.
    moss_splash::play_splash(panel_,
                             moss_splash::emote_assets_bin,
                             moss_splash::emote_assets_bin_size(),
                             "start.eaf");

    // PSRAM 分配 splash loop 用的 out_buf (160x80x2 bytes).
    const size_t buf_size = (size_t)width * height * sizeof(uint16_t);
    splash_out_buf_ = (uint16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (splash_out_buf_ == nullptr) splash_out_buf_ = (uint16_t*)malloc(buf_size);
    if (splash_out_buf_ == nullptr) {
        ESP_LOGE(TAG, "Failed to alloc splash_out_buf (%zu bytes)", buf_size);
        return;
    }
    std::memset(splash_out_buf_, 0, buf_size);

    StartSplashLoop();

    ESP_LOGI(TAG, "MossSpiLcdDisplay ready (no LVGL, splash owns panel)");
}

MossSpiLcdDisplay::~MossSpiLcdDisplay() {
    moss_splash::stop_code_scroll_loop();
    if (splash_out_buf_ != nullptr) {
        free(splash_out_buf_);
        splash_out_buf_ = nullptr;
    }
    s_dialog.active = false;
}

void MossSpiLcdDisplay::StartSplashLoop() {
    if (panel_ == nullptr || splash_out_buf_ == nullptr) {
        ESP_LOGE(TAG, "StartSplashLoop skipped: panel or out_buf is null");
        return;
    }
    if (moss_splash::is_code_scroll_running()) {
        ESP_LOGW(TAG, "Code scroll already running");
        return;
    }

    // 第一条 status = BOOTING (ASCII, splash 立刻能画).
    moss_splash::set_dialog_state(&s_dialog, true, "BOOTING", kWarnColor);

    // 启动代码无限滚动 (替代 EAF code_scroll.eaf)
    moss_splash::LoopConfig cfg{};
    cfg.panel = panel_;
    cfg.out_buf = splash_out_buf_;
    cfg.panel_w = width_;
    cfg.panel_h = height_;
    cfg.dialog_state_ptr = &s_dialog;

    moss_splash::start_code_scroll_loop(cfg);
}

void MossSpiLcdDisplay::SetStatus(const char* status) {
    if (status == nullptr) return;
    current_status_ = status;

    {
        std::lock_guard<std::mutex> lock(face_ui_mutex_);
        if (face_track_mode_) {
            return;  // HUD owns the panel while tracking.
        }
    }

    // 静默状态 (聆听说/待命/连接中) 不弹窗，只清空弹窗内容
    if (is_silent_status(status)) {
        uint16_t color;
        uint8_t priority;
        classify_color_priority(status, color, priority);
        // 直接清空 title 让弹窗不显示，不需要 priority 保护
        moss_splash::set_dialog_state(&s_dialog, false, "", kDialogColor, moss_splash::kPriorityNone);
        return;
    }

    uint16_t color;
    uint8_t priority;
    classify_color_priority(status, color, priority);
    const char* text = map_status_to_ascii(status);
    moss_splash::set_dialog_state(&s_dialog, true, text, color, priority);
}

void MossSpiLcdDisplay::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;  // sticky 时长忽略 (弹窗一直显示)
    SetStatus(notification);
}

void MossSpiLcdDisplay::ShowNotification(const std::string& notification, int duration_ms) {
    (void)duration_ms;
    SetStatus(notification.c_str());
}

void MossSpiLcdDisplay::ShowNotification(const char* title, const char* body, int duration_ms) {
    (void)duration_ms;
    if (title == nullptr) return;
    uint16_t color;
    uint8_t priority;
    classify_color_priority(title, color, priority);
    const char* text = map_status_to_ascii(title);
    moss_splash::set_dialog_state(&s_dialog, true, text, color, priority, body);
}

void MossSpiLcdDisplay::UpdateStatusBar(bool update_all) {
    (void)update_all;
}

// 这些方法基类 (LcdDisplay) 会调 LVGL; 但我们完全不用 LVGL, 直接 no-op.
// 避免 Application / 其它调用方调它们时崩在 lvgl_port_lock 断言.
void MossSpiLcdDisplay::SetChatMessage(const char* role, const char* content) {
    (void)role; (void)content;
}

void MossSpiLcdDisplay::SetEmotion(const char* emotion) {
    (void)emotion;
}

void MossSpiLcdDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    (void)image;
}

void MossSpiLcdDisplay::SetTheme(Theme* theme) {
    (void)theme;
}

void MossSpiLcdDisplay::SetPowerSaveMode(bool on) {
    (void)on;
}

void MossSpiLcdDisplay::SetupUI() {
    setup_ui_called_ = true;
}

void MossSpiLcdDisplay::ClearChatMessages() {}

bool MossSpiLcdDisplay::Lock(int timeout_ms) {
    (void)timeout_ms;
    return true;
}

void MossSpiLcdDisplay::Unlock() {}

void MossSpiLcdDisplay::SetScreenOn(bool on) {
    if (screen_on_ == on) {
        return;
    }
    screen_on_ = on;
    if (panel_ != nullptr) {
        esp_err_t err = esp_lcd_panel_disp_on_off(panel_, on);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "disp_on_off(%d) failed: %s", on, esp_err_to_name(err));
        }
    }
    ESP_LOGI(TAG, "Screen %s", on ? "ON" : "OFF");
}

void MossSpiLcdDisplay::DismissDialog() {
    moss_splash::dismiss_dialog(&s_dialog);
    ESP_LOGI(TAG, "Dialog dismissed");
}

void MossSpiLcdDisplay::EnterFaceTrackMode() {
    std::lock_guard<std::mutex> lock(face_ui_mutex_);
    if (face_track_mode_) {
        return;
    }
    ESP_LOGI(TAG, "Enter face-track UI (stop code scroll)");
    moss_splash::stop_code_scroll_loop();
    moss_splash::wait_code_scroll_stopped(1500);
    moss_splash::set_dialog_state(&s_dialog, false, "", kDialogColor, moss_splash::kPriorityNone);
    face_track_mode_ = true;
    if (panel_ && splash_out_buf_) {
        moss_splash::draw_face_track_hud(panel_, splash_out_buf_, width_, height_, false, 0, 0, 0, 0,
                                         0, 0, 0, 0, 0, 320, 240, 0, false, nullptr, 0, 0);
    }
}

void MossSpiLcdDisplay::ExitFaceTrackMode() {
    std::lock_guard<std::mutex> lock(face_ui_mutex_);
    if (!face_track_mode_) {
        return;
    }
    face_track_mode_ = false;
    ESP_LOGI(TAG, "Exit face-track UI (restart code scroll)");
    StartSplashLoop();
}

void MossSpiLcdDisplay::UpdateFaceTrackOverlay(const FaceTrackerStatus& status) {
    std::lock_guard<std::mutex> lock(face_ui_mutex_);
    if (!face_track_mode_ || panel_ == nullptr || splash_out_buf_ == nullptr) {
        return;
    }
    moss_splash::draw_face_track_hud(
        panel_, splash_out_buf_, width_, height_, status.has_face, static_cast<int>(status.faces),
        status.err_x, status.err_y, status.face_w, status.face_h, status.box_x1, status.box_y1,
        status.box_x2, status.box_y2, status.frame_w, status.frame_h, status.detect_ms,
        status.gimbal_moving, status.preview_rgb565, status.preview_w, status.preview_h);
}