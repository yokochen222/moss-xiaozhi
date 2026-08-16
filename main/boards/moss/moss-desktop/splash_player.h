#pragma once

#include <esp_lcd_panel_ops.h>
#include <cstdint>

namespace moss_splash {

// 对话框文字状态 - splash task 每帧读取后直接画到 panel.
// 由 MossSpiLcdDisplay 写入, splash task 读取.
// 写入端持可重入锁 (单写者) 或原子写即可, 读取者 (splash task) 拿到 snapshot
// 后复制再画, 不需要强同步 - 短暂不一致人眼也看不出.
struct DialogState {
    bool active;
    uint16_t color;   // RGB565
    uint8_t  priority; // 0=无弹窗, 1=状态级(待命/连接中), 2=通知级, 3=警告级, 4=错误级
    uint8_t  style;    // kDialogStyleDefault / kDialogStyleSignal
    char title[64];    // 主文字 (居中, 带下划线)
    char body[64];     // 副内容 (可空, 空时只画一行)
    int64_t show_until_tick; // 弹窗自动关闭时刻 (esp_timer tick), 0=无超时
};

// 优先级常量
static constexpr uint8_t kPriorityNone    = 0;  // 不弹窗
static constexpr uint8_t kPriorityState    = 1;  // 待命/连接中 (不弹)
static constexpr uint8_t kPriorityInfo     = 2;  // 普通通知 / 聆听·说话信号弹窗
static constexpr uint8_t kPriorityWarning = 3;  // 警告/升级
static constexpr uint8_t kPriorityError   = 4;  // 错误/失败

// 弹窗布局
static constexpr uint8_t kDialogStyleDefault = 0;  // 标题 (+ 可选正文)
static constexpr uint8_t kDialogStyleSignal  = 1;  // 左文字 + 右固定波动音谱线

const int DIALOG_TITLE_MAX = sizeof(DialogState::title) - 1;
const int DIALOG_BODY_MAX  = sizeof(DialogState::body)  - 1;

// 弹窗默认自动关闭时长 (ms)
static constexpr int DIALOG_DEFAULT_TIMEOUT_MS = 10000;

// 在 MMAP bin (base, size) 中查找 EAF 动画并播放一次.
// panel: ST7735 panel handle (已经初始化).
// base, size: 嵌入的 MMAP bin (例如 emote_assets_bin / emote_assets_bin_size).
// asset_name: EAF 名称 (例如 "start.eaf").
// 阻塞调用 - 整个动画播完后返回 (最后一帧保留在屏幕上).
void play_splash(esp_lcd_panel_handle_t panel,
                 const uint8_t* bin_base, size_t bin_size,
                 const char* asset_name);

// 关闭弹窗 (active=false). 用于 DismissAlert 等场景.
// 仅在 priority 允许时关闭 (防止高优先级弹窗被静默状态误关).
void dismiss_dialog(DialogState* state);

// LoopConfig: 代码滚动/EAF loop 通用渲染配置
struct LoopConfig {
    esp_lcd_panel_handle_t panel = nullptr;
    uint16_t* out_buf = nullptr;
    int panel_w = 0;
    int panel_h = 0;
    DialogState* dialog_state_ptr = nullptr;  // 非 const, 可被外部更新/dismiss
};

// 启动代码无限滚动 (替代 EAF code_scroll.eaf)
// 逐字逐行打印代码, 满屏时自动向上滚动, 无限循环.
// 内部使用 FreeRTOS task, 函数立即返回.
void start_code_scroll_loop(const LoopConfig& cfg);

// 请求停止代码滚动任务 (仅置标志, 任务会在当前帧结束后退出).
void stop_code_scroll_loop();

// 跨 task 安全地更新 DialogState (临界区保护, splash task 与外部调用).
// title / body 会被截断到各自 MAX 长度.
// 3 参数版本: body 为空 (默认单行弹窗).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color);
// 5 参数版本: title + body 两行弹窗.
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, const char* body);
// 4 参数版本: 显式指定优先级 (优先级不够时拒绝写入).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority);
// 6 参数版本: priority + body (优先级保护 + 两行弹窗).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority, const char* body);
// 7 参数版本: 带超时 (ms, 0=无超时, 默认 DIALOG_DEFAULT_TIMEOUT_MS).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority, const char* body, int timeout_ms);
// 8 参数版本: 指定布局 style (默认 kDialogStyleDefault).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority, const char* body, int timeout_ms, uint8_t style);

}  // namespace moss_splash