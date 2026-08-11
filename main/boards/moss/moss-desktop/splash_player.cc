#include "splash_player.h"
#include "moss_splash.h"
#include "config.h"
#include "eaf_iface.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdlib>
#include <algorithm>

#include <lvgl.h>

static const char* TAG = "SplashPlayer";

LV_FONT_DECLARE(font_puhui_basic_14_1);
static const lv_font_t* s_font = &font_puhui_basic_14_1;

namespace moss_splash {

// ============ 代码无限滚动渲染器 (替代 emote-code EAF) ============

// kAsciiCharW 必须定义在最前 (dialog section 也用到)
static constexpr int kAsciiCharW = 6;

// 前向声明 (被下面函数引用但定义在后)
static uint32_t utf8_next(const char* s, int* io);
static void draw_ascii_char(uint16_t* dst, int dst_w, int dst_h,
                            int x_left, int y_top,
                            unsigned char c, uint16_t color);
static void draw_cjk_glyph(uint16_t* dst, int dst_w, int dst_h,
                           int x_left, int y_baseline,
                           const lv_font_glyph_dsc_t* g_dsc, uint16_t color);

// dialog 渲染函数前向声明
static bool should_show_dialog(uint8_t priority);
static void calc_dialog_box(int dst_w, int dst_h, int* out_x, int* out_y, int* out_w, int* out_h);
static void draw_dialog_box(uint16_t* dst, int dst_w, int dst_h,
                            int box_x, int box_y, int box_w, int box_h,
                            uint16_t color, const char* title, const char* body);
static void copy_dialog_snapshot(const struct DialogState* src, struct DialogState* dst);

// 配色 (RGB565)
static constexpr uint16_t kCodeColor = 0xFFFF;  // 白色

// 渲染器内部配置
namespace code_scroll_cfg {
    // 逐字打印速度: 每 TICK_INTERVAL_MS ms 打印 CHARS_PER_TICK 个字符
    static constexpr uint32_t TICK_INTERVAL_MS   = 15;   // 35ms per tick
    static constexpr uint32_t CHARS_PER_TICK      = 1;    // 1 char per tick = 20 chars/sec
    // 一帧渲染间隔 (FPS 控制, 不要太高以节省 CPU)
    static constexpr uint32_t RENDER_INTERVAL_MS  = 50;   // render every 50ms = 20 FPS

    // 行高 (像素)
    static constexpr int ASCII_LINE_H  = 8;   // 7px char + 1px spacing
    static constexpr int CJK_LINE_H    = 15;  // 14px char + 1px spacing
    static constexpr int LINE_SPACING  = 1;   // 行间距

    // 有效代码区宽度 (右侧留边距)
    static constexpr int CODE_MARGIN_R = 2;

    // 滚动阈值: 显示行数接近屏幕高度时向上滚
    // 当新行 baseline 超过 (panel_h - 2*line_h) 时, scroll_offset++

    // 自动换行宽度 (字符数)
    static constexpr int WRAP_WIDTH    = 20;  // 20 chars per line
}

// 渲染一行代码 (纯色, 无语法高亮)
// code_x: 代码区左边界 (像素)
// y_baseline: 文字 baseline (像素)
// char_count: 只渲染前 char_count 个字符
static void draw_code_line(uint16_t* dst, int dst_w, int dst_h,
                         int code_x, int y_baseline,
                         const char* line, int char_count) {
    if (line == nullptr || char_count <= 0) return;
    int i = 0;
    int x = code_x;
    int drawn = 0;
    while (line[i] != '\0' && drawn < char_count) {
        uint32_t cp = utf8_next(line, &i);
        if (cp == 0) break;
        if (x >= dst_w) break;

        if (cp < 128) {
            draw_ascii_char(dst, dst_w, dst_h, x, y_baseline - 6, (unsigned char)cp, kCodeColor);
            x += kAsciiCharW;
        } else {
            lv_font_glyph_dsc_t g_dsc;
            if (lv_font_get_glyph_dsc(s_font, &g_dsc, cp, 0)) {
                draw_cjk_glyph(dst, dst_w, dst_h, x, y_baseline, &g_dsc, kCodeColor);
                int adv = (int)g_dsc.adv_w;
                if (adv <= 0) adv = 14;
                x += adv;
            }
        }
        drawn++;
    }
}

struct CodeLine {
    char text[64];
    int  text_len;
    int  rendered;       // 已渲染字符数
};

class CodeScrollRenderer {
public:
    struct InitResult {
        bool ok;
        const char* error;
    };

    static InitResult validate_config(const LoopConfig& cfg) {
        if (cfg.panel == nullptr || cfg.out_buf == nullptr) {
            return {false, "panel or out_buf is null"};
        }
        if (cfg.panel_w <= 0 || cfg.panel_h <= 0) {
            return {false, "invalid panel dimensions"};
        }
        return {true, nullptr};
    }

    static void start_loop_task(const LoopConfig& cfg) {
        if (s_task != nullptr) {
            ESP_LOGW(TAG, "CodeScrollRenderer task already running");
            return;
        }
        s_stop_requested = false;

        LoopCtx* ctx = new LoopCtx{};
        ctx->cfg = cfg;
        ctx->bin_base = nullptr;
        ctx->bin_size = 0;
        ctx->asset_name = nullptr;
        ctx->out_buf2 = nullptr;
        ctx->cur_buf_idx = 0;

        BaseType_t ok = xTaskCreate(
            loop_task, "code_scroll",
            8192, ctx, 5, &s_task);
        if (ok != pdPASS) {
            ESP_LOGE(TAG, "Failed to create code_scroll task");
            delete ctx;
            s_task = nullptr;
        }
    }

    static void stop_loop() {
        s_stop_requested = true;
    }

private:
    // loop 任务上下文
    struct LoopCtx {
        LoopConfig cfg;
        const uint8_t* bin_base;
        size_t bin_size;
        const char* asset_name;
        uint16_t* out_buf2;
        uint16_t cur_buf_idx;
    };

    static volatile bool s_stop_requested;
    static TaskHandle_t s_task;

    // 预设代码内容 (C 代码片段)
    static constexpr const char* CODE_CONTENT =
        "// MOSS Code Scroller\n"
        "// Infinite Code Display\n"
        "#include <esp_system.h>\n"
        "#define MAX_BUFFER 1024\n"
        "void init_display(void) {\n"
        "    esp_lcd_panel_init();\n"
        "    set_backlight(true);\n"
        "    clear_screen(BLACK);\n"
        "}\n"
        "void render_frame(void) {\n"
        "    uint32_t ts = get_tick();\n"
        "    draw_pixels(buf, ts);\n"
        "    flip_buffer();\n"
        "    enter_light_sleep();\n"
        "}\n"
        "bool check_wake_reason(void) {\n"
        "    auto evt = get_last_event();\n"
        "    return evt == WAKE_GPIO;\n"
        "}\n"
        "int main_loop(void) {\n"
        "    while (!shutdown) {\n"
        "        if (can_sleep()) {\n"
        "            deep_sleep(5000);\n"
        "        }\n"
        "        process_events();\n"
        "        update_display();\n"
        "        delay_ms(PERIOD_MS);\n"
        "    }\n"
        "    return STATE_OK;\n"
        "}\n"
        "// END OF SOURCE\n"
        "// MOSS Code Scroller\n"
        "// Infinite Code Display\n"
        "#include <driver/spi.h>\n"
        "static constexpr int FPS = 20;\n"
        "void IRAM_ATTR isr_handler(void* arg) {\n"
        "    BaseType_t woken = pdFALSE;\n"
        "    xQueueSendFromISR(q, &data, &woken);\n"
        "    if (woken) portYIELD_FROM_ISR();\n"
        "}\n"
        "void spi_transmit(const uint8_t* tx) {\n"
        "    spi_transaction_t t = {};\n"
        "    t.length = BITS_PER_WORD;\n"
        "    t.tx_buffer = tx;\n"
        "    spi_trans(SPI_HOST, &t, portMAX_DELAY);\n"
        "}\n"
        "void draw_bitmap(int x, int y, int w, int h, const uint16_t* pixels) {\n"
        "    set_window(x, y, w, h);\n"
        "    for (int row = 0; row < h; row++) {\n"
        "        write_data(pixels + row * w, w * 2);\n"
        "    }\n"
        "}\n"
        "bool audio_available(void) {\n"
        "    return uxQueueMessagesWaiting(audio_q) > 0;\n"
        "}\n"
        "void play_tone(int freq_hz) {\n"
        "    dac_output_voltage(DAC_CHANNEL_1, freq_hz & 0xFF);\n"
        "}\n"
        "// END OF SOURCE\n";

    static constexpr int MAX_LINES = 48;
    static constexpr int CODE_LINE_H = code_scroll_cfg::ASCII_LINE_H;

    static void loop_task(void* arg) {
        LoopCtx* ctx = static_cast<LoopCtx*>(arg);
        LoopConfig cfg = ctx->cfg;
        delete ctx;

        const int pw = cfg.panel_w;
        const int ph = cfg.panel_h;
        const int buf_size = (size_t)pw * ph * sizeof(uint16_t);

        // 分配双缓冲
        uint16_t* buf0 = cfg.out_buf;
        uint16_t* buf1 = (uint16_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf1) buf1 = (uint16_t*)malloc(buf_size);
        uint16_t* disp_buf = buf0;
        uint16_t* render_buf = buf1;
        int cur_buf = 1;  // 下一帧渲染到 buf1, 显示 buf0

        // 代码行解析
        CodeLine lines[MAX_LINES];
        int num_lines = 0;
        parse_code(CODE_CONTENT, lines, &num_lines);

        ESP_LOGI(TAG, "CodeScroll: %d lines parsed, panel=%dx%d",
                 num_lines, pw, ph);

        // 滚动状态
        int scroll_offset = 0;       // 向上滚动的像素数 (无限增长)
        int total_line_idx = 0;      // 无限增长的绝对行索引
        int cursor_char_idx = 0;     // 当前行内已渲染字符

        // 字符定时器
        int64_t last_char_tick_us = esp_timer_get_time();
        int64_t last_render_tick_us = last_char_tick_us;

        // dialog snapshot
        DialogState dialog_snap{};

        while (!s_stop_requested) {
            int64_t now_us = esp_timer_get_time();

            // === 1. 逐字推进 (受 CHARS_PER_TICK 和 TICK_INTERVAL_MS 控制) ===
            int64_t elapsed_char_us = now_us - last_char_tick_us;
            if (elapsed_char_us >= code_scroll_cfg::TICK_INTERVAL_MS * 1000) {
                last_char_tick_us = now_us - (elapsed_char_us % (code_scroll_cfg::TICK_INTERVAL_MS * 1000));

                int chars_to_render = code_scroll_cfg::CHARS_PER_TICK;
                for (int c = 0; c < chars_to_render; c++) {
                    int line_in_buf = total_line_idx % num_lines;
                    CodeLine& line = lines[line_in_buf];

                    if (cursor_char_idx < line.text_len) {
                        cursor_char_idx++;
                    } else {
                        total_line_idx++;
                        cursor_char_idx = 0;
                    }
                }

                // === 2. 逐像素滚动: 打印位置超出屏幕底部时滚动 ===
                int print_y = (total_line_idx + 1) * CODE_LINE_H - scroll_offset;
                if (print_y >= ph) {
                    scroll_offset++;
                }

                // === 3. 渲染帧 ===
                int64_t elapsed_render_us = now_us - last_render_tick_us;
                if (elapsed_render_us >= code_scroll_cfg::RENDER_INTERVAL_MS * 1000) {
                    last_render_tick_us = now_us - (elapsed_render_us % (code_scroll_cfg::RENDER_INTERVAL_MS * 1000));

                    // 切换 buffer
                    int display_idx = cur_buf;
                    cur_buf = (cur_buf == 0) ? 1 : 0;
                    disp_buf = (display_idx == 0) ? buf0 : buf1;
                    render_buf = (cur_buf == 0) ? buf0 : buf1;

                    // 清屏
                    std::memset(render_buf, 0, buf_size);

                    // 渲染代码行 (无限滚动，无缝衔接)
                    int first_vis_line = scroll_offset / CODE_LINE_H;
                    int y_offset_in_line = scroll_offset % CODE_LINE_H;
                    int code_x = 0;

                    // 显示范围: [first_vis_line, total_line_idx] 落在屏幕内的行
                    for (int abs_li = first_vis_line; abs_li <= total_line_idx; abs_li++) {
                        int row_in_screen = (abs_li - first_vis_line) * CODE_LINE_H - y_offset_in_line;
                        int baseline = row_in_screen + CODE_LINE_H - 1;

                        if (baseline < -CODE_LINE_H || baseline >= ph) continue;

                        int line_in_buf = abs_li % num_lines;
                        CodeLine& cl = lines[line_in_buf];

                    // 每行根据与当前行的距离独立计算逐字进度
                    int rendered_here;
                    if (abs_li < total_line_idx) {
                        // 已完成的行: 显示全部
                        rendered_here = cl.text_len;
                    } else {
                        // 当前行或未来行: 逐字递减
                        int line_dist = total_line_idx - abs_li; // 0=当前, -1=下一行未开始
                        rendered_here = cursor_char_idx + line_dist * code_scroll_cfg::CHARS_PER_TICK;
                        if (rendered_here > cl.text_len) rendered_here = cl.text_len;
                        if (rendered_here < 0) rendered_here = 0;
                    }

                        if (rendered_here > 0) {
                            draw_code_line(render_buf, pw, ph,
                                          code_x, baseline,
                                          cl.text, rendered_here);
                        }
                    }

                    // === 4. 画对话框遮罩 (底部区域) ===
                    if (cfg.dialog_state_ptr != nullptr) {
                        copy_dialog_snapshot(cfg.dialog_state_ptr, &dialog_snap);
                        if (dialog_snap.active && dialog_snap.title[0] != '\0'
                            && should_show_dialog(dialog_snap.priority)) {
                            if (dialog_snap.show_until_tick > 0) {
                                if (now_us >= dialog_snap.show_until_tick) {
                                    dialog_snap.active = false;
                                }
                            }
                            if (dialog_snap.active) {
                                int bx, by, bw, bh;
                                calc_dialog_box(pw, ph, &bx, &by, &bw, &bh);
                                draw_dialog_box(render_buf, pw, ph,
                                               bx, by, bw, bh,
                                               dialog_snap.color,
                                               dialog_snap.title,
                                               dialog_snap.body[0] ? dialog_snap.body : nullptr);
                            }
                        }
                    }

                    // === 5. 发送到 panel ===
                    esp_lcd_panel_draw_bitmap(cfg.panel, 0, 0, pw, ph, disp_buf);
                }

                // === 6. 低功耗: 等待下一个 tick ===
                vTaskDelay(pdMS_TO_TICKS(code_scroll_cfg::TICK_INTERVAL_MS));
            } else {
                // 还没到 tick, 短暂 delay
                vTaskDelay(pdMS_TO_TICKS(10));
            }
        }

        // cleanup
        if (buf1) free(buf1);
        s_task = nullptr;
        vTaskDelete(nullptr);
    }

    // 解析代码文本为行数组
    static void parse_code(const char* src, CodeLine* out_lines, int* out_count) {
        int count = 0;
        const char* p = src;
        const char* line_start = p;

        while (*p && count < MAX_LINES) {
            if (*p == '\n') {
                int len = p - line_start;
                if (len > (int)sizeof(out_lines[count].text) - 1) {
                    len = sizeof(out_lines[count].text) - 1;
                }
                std::memcpy(out_lines[count].text, line_start, (size_t)len);
                out_lines[count].text[len] = '\0';
                out_lines[count].text_len = len;
                out_lines[count].rendered = 0;
                count++;
                line_start = p + 1;
            }
            p++;
        }
        // 最后一行 (无换行符结尾)
        if (*line_start && count < MAX_LINES) {
            int len = 0;
            while (line_start[len]) len++;
            if (len > 0) {
                std::memcpy(out_lines[count].text, line_start, (size_t)len);
                out_lines[count].text[len] = '\0';
                out_lines[count].text_len = len;
                out_lines[count].rendered = 0;
                count++;
            }
        }
        *out_count = count;
    }
};

volatile bool CodeScrollRenderer::s_stop_requested = false;
TaskHandle_t CodeScrollRenderer::s_task = nullptr;

// 启动代码滚动 (导出为 API)
void start_code_scroll_loop(const LoopConfig& cfg) {
    auto result = CodeScrollRenderer::validate_config(cfg);
    if (!result.ok) {
        ESP_LOGE(TAG, "CodeScrollRenderer config error: %s", result.error);
        return;
    }
    CodeScrollRenderer::start_loop_task(cfg);
}

void stop_code_scroll_loop() {
    CodeScrollRenderer::stop_loop();
}

// ============ 科技终端弹窗渲染 (含 ASCII + CJK 文字) ============

// 前向声明
static int draw_text(uint16_t* dst, int dst_w, int dst_h,
                     int x_left, int y_baseline,
                     const char* s, uint16_t color);

static void draw_dialog_box(uint16_t* dst, int dst_w, int dst_h,
                            int box_x, int box_y, int box_w, int box_h,
                            uint16_t color,
                            const char* title, const char* body) {
    (void)title;
    (void)body;

    static constexpr uint16_t kBg     = 0x0000;
    static constexpr uint16_t kFrame  = 0xFFFF;
    static constexpr uint16_t kInner  = 0x632C;
    static constexpr uint16_t kCorner = 0xFFFF;

    // 不透明黑色背景
    for (int row = 0; row < box_h; row++) {
        int dy = box_y + row;
        if (dy < 0 || dy >= dst_h) continue;
        int x0 = box_x < 0 ? 0 : box_x;
        int x1 = box_x + box_w;
        if (x1 > dst_w) x1 = dst_w;
        if (x1 <= x0) continue;
        std::memset(dst + dy * dst_w + x0, 0, (size_t)(x1 - x0) * sizeof(uint16_t));
    }
    (void)kBg;

    auto plot = [&](int x, int y, uint16_t c) {
        if (x < 0 || x >= dst_w || y < 0 || y >= dst_h) return;
        // RGB565 LE -> panel 大端: swap bytes
        dst[y * dst_w + x] = (uint16_t)((c >> 8) | (c << 8));
    };

    auto hline_real = [&](int x0, int x1, int y, uint16_t c) {
        if (y < 0 || y >= dst_h) return;
        if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
        if (x0 < 0) x0 = 0;
        if (x1 >= dst_w) x1 = dst_w - 1;
        for (int x = x0; x <= x1; x++) plot(x, y, c);
    };

    auto vline_real = [&](int x, int y0, int y1, uint16_t c) {
        if (x < 0 || x >= dst_w) return;
        if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
        if (y0 < 0) y0 = 0;
        if (y1 >= dst_h) y1 = dst_h - 1;
        for (int y = y0; y <= y1; y++) plot(x, y, c);
    };

    auto corner = [&](int x, int y, int which) {
        switch (which) {
            case 0: // 左上 ┌
                hline_real(x, x + 2, y,     kCorner);
                vline_real(x, y, y + 2, kCorner);
                break;
            case 1: // 右上 ┐
                hline_real(x - 2, x, y,     kCorner);
                vline_real(x, y, y + 2, kCorner);
                break;
            case 2: // 左下 └
                hline_real(x, x + 2, y,     kCorner);
                vline_real(x, y - 2, y, kCorner);
                break;
            case 3: // 右下 ┘
                hline_real(x - 2, x, y,     kCorner);
                vline_real(x, y - 2, y, kCorner);
                break;
        }
    };

    // 外框 + 内框
    hline_real(box_x,             box_x + box_w - 1, box_y,             kFrame);
    hline_real(box_x,             box_x + box_w - 1, box_y + box_h - 1, kFrame);
    vline_real(box_x,             box_y, box_y + box_h - 1, kFrame);
    vline_real(box_x + box_w - 1, box_y, box_y + box_h - 1, kFrame);

    hline_real(box_x + 1, box_x + box_w - 2, box_y + 1,         kInner);
    hline_real(box_x + 1, box_x + box_w - 2, box_y + box_h - 2, kInner);
    vline_real(box_x + 1,         box_y + 1, box_y + box_h - 2, kInner);
    vline_real(box_x + box_w - 2, box_y + 1, box_y + box_h - 2, kInner);

    // 四角 3x3 角括号
    corner(box_x + 1,         box_y + 1,         0);
    corner(box_x + box_w - 2, box_y + 1,         1);
    corner(box_x + 1,         box_y + box_h - 2, 2);
    corner(box_x + box_w - 2, box_y + box_h - 2, 3);

    // 右上角 3x3 状态指示灯 (颜色由调用方 color 传入, 表示 info/warn/error)
    int dot_x = box_x + box_w - 6;
    int dot_y = box_y + 4;
    for (int dy = 0; dy < 3; dy++) {
        for (int dx = 0; dx < 3; dx++) {
            plot(dot_x + dx, dot_y + dy, color);
        }
    }

    // 标题
    if (title != nullptr && title[0] != '\0') {
        // ASCII 5x7 字符顶部 = baseline - 6, 留 4px 上边距 → baseline = box_y + 11
        int title_baseline = box_y + 11;
        draw_text(dst, dst_w, dst_h, box_x + 4, title_baseline, title, kFrame);
    }

    // 正文
    if (body != nullptr && body[0] != '\0') {
        // 7px 字高 + 4px 下边距 → baseline = box_y + box_h - 5
        int body_baseline = box_y + box_h - 5;
        draw_text(dst, dst_w, dst_h, box_x + 4, body_baseline, body, kInner);
    }
}

// 5x7 ASCII 点阵字体 (公开领域 "5x7 dot matrix font").
// 顺序 ASCII 32..127, 每字符 5 字节 (1 字节/列, 低 7 bit = 列内像素,
// bit0 = 顶部, bit6 = 底部). 非 ASCII (含 UTF-8 多字节) 走 font_puhui 渲染.
static const uint8_t kAscii5x7[96][5] = {
    {0x00,0x00,0x00,0x00,0x00},
    {0x00,0x00,0x5F,0x00,0x00},
    {0x00,0x07,0x00,0x07,0x00},
    {0x14,0x7F,0x14,0x7F,0x14},
    {0x24,0x2A,0x7F,0x2A,0x12},
    {0x23,0x13,0x08,0x64,0x62},
    {0x36,0x49,0x55,0x22,0x50},
    {0x00,0x05,0x03,0x00,0x00},
    {0x00,0x1C,0x22,0x41,0x00},
    {0x00,0x41,0x22,0x1C,0x00},
    {0x08,0x2A,0x1C,0x2A,0x08},
    {0x08,0x08,0x3E,0x08,0x08},
    {0x00,0x50,0x30,0x00,0x00},
    {0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x10,0x08,0x04,0x02},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x56,0x36,0x00,0x00},
    {0x00,0x08,0x14,0x22,0x41},
    {0x14,0x14,0x14,0x14,0x14},
    {0x41,0x22,0x14,0x08,0x00},
    {0x02,0x01,0x51,0x09,0x06},
    {0x32,0x49,0x79,0x41,0x3E},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7E,0x09,0x09,0x01,0x02},
    {0x3E,0x41,0x41,0x51,0x32},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x04,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x00,0x00,0x7F,0x41,0x41},
    {0x02,0x04,0x08,0x10,0x20},
    {0x41,0x41,0x7F,0x00,0x00},
    {0x04,0x02,0x01,0x02,0x04},
    {0x40,0x40,0x40,0x40,0x40},
    {0x00,0x01,0x02,0x04,0x00},
    {0x20,0x54,0x54,0x54,0x78},
    {0x7F,0x48,0x44,0x44,0x38},
    {0x38,0x44,0x44,0x44,0x20},
    {0x38,0x44,0x44,0x48,0x7F},
    {0x38,0x54,0x54,0x54,0x18},
    {0x08,0x7E,0x09,0x01,0x02},
    {0x08,0x14,0x54,0x54,0x3C},
    {0x7F,0x08,0x04,0x04,0x78},
    {0x00,0x44,0x7D,0x40,0x00},
    {0x20,0x40,0x44,0x3D,0x00},
    {0x00,0x7F,0x10,0x28,0x44},
    {0x00,0x41,0x7F,0x40,0x00},
    {0x7C,0x04,0x18,0x04,0x78},
    {0x7C,0x08,0x04,0x04,0x78},
    {0x38,0x44,0x44,0x44,0x38},
    {0x7C,0x14,0x14,0x14,0x08},
    {0x08,0x14,0x14,0x18,0x7C},
    {0x7C,0x08,0x04,0x04,0x08},
    {0x48,0x54,0x54,0x54,0x20},
    {0x04,0x3F,0x44,0x40,0x20},
    {0x3C,0x40,0x40,0x20,0x7C},
    {0x1C,0x20,0x40,0x20,0x1C},
    {0x3C,0x40,0x30,0x40,0x3C},
    {0x44,0x28,0x10,0x28,0x44},
    {0x0C,0x50,0x50,0x50,0x3C},
    {0x44,0x64,0x54,0x4C,0x44},
    {0x00,0x08,0x36,0x41,0x00},
    {0x00,0x00,0x7F,0x00,0x00},
    {0x00,0x41,0x36,0x08,0x00},
    {0x08,0x04,0x08,0x10,0x08},
    {0x00,0x00,0x00,0x00,0x00},
};

// kAsciiCharW 已在上方定义
static constexpr int kAsciiCharH = 7;

// UTF-8 解码: 输入字节索引 i, 返回 unicode codepoint 并推进 i.
// 出错或字符串结束返回 0.
static uint32_t utf8_next(const char* s, int* io) {
    unsigned char c = (unsigned char)s[*io];
    if (c == 0) return 0;
    if (c < 0x80) {
        (*io) += 1;
        return c;
    }
    uint32_t cp = 0;
    int extra = 0;
    if      ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
    else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
    else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
    else {
        // 非法 leading byte: 跳 1 字节
        (*io) += 1;
        return 0xFFFD;
    }
    (*io) += 1;
    for (int k = 0; k < extra; k++) {
        unsigned char cc = (unsigned char)s[*io];
        if ((cc & 0xC0) != 0x80) return 0xFFFD;
        cp = (cp << 6) | (cc & 0x3F);
        (*io) += 1;
    }
    return cp;
}

// 直接读 lv_font_t 的 dsc 表查找 glyph bitmap, 绕开 lv_font_get_glyph_bitmap
// (后者在 box_w != stride*8 的情况下会丢 req_raw_bitmap=0).
static const uint8_t* get_packed_bitmap(const lv_font_glyph_dsc_t* g_dsc) {
    if (g_dsc == nullptr) return nullptr;
    const lv_font_t* font = g_dsc->resolved_font;
    if (font == nullptr) return nullptr;
    const lv_font_fmt_txt_dsc_t* fdsc = (const lv_font_fmt_txt_dsc_t*)font->dsc;
    if (fdsc == nullptr) return nullptr;
    uint32_t gid = g_dsc->gid.index;
    if (gid == 0) return nullptr;  // lvgl convention
    const lv_font_fmt_txt_glyph_dsc_t* g = &fdsc->glyph_dsc[gid];
    return &fdsc->glyph_bitmap[g->bitmap_index];
}

// 画单个 CJK glyph (来自 font_puhui_basic_14_1 plain 1-bpp).
// storage layout: bit-packed contiguous, 无 row byte 对齐.
//   bit_offset = row * box_w + col
//   byte       = bit_offset / 8
//   bit        = 7 - (bit_offset % 8)     (MSB-first within byte)
static void draw_cjk_glyph(uint16_t* dst, int dst_w, int dst_h,
                           int x_left, int y_baseline,
                           const lv_font_glyph_dsc_t* g_dsc, uint16_t color) {
    if (g_dsc == nullptr || g_dsc->box_w <= 0 || g_dsc->box_h <= 0) return;

    const uint8_t* packed = get_packed_bitmap(g_dsc);
    if (packed == nullptr) return;

    uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    int y_top = y_baseline + g_dsc->ofs_y;
    int x_off = x_left + g_dsc->ofs_x;
    int total_bits = (int)g_dsc->box_w * (int)g_dsc->box_h;

    for (int row = 0; row < (int)g_dsc->box_h; row++) {
        int dy = y_top + row;
        if (dy < 0 || dy >= dst_h) continue;
        for (int col = 0; col < (int)g_dsc->box_w; col++) {
            int dx = x_off + col;
            if (dx < 0 || dx >= dst_w) continue;
            int bit_offset = row * (int)g_dsc->box_w + col;
            if (bit_offset >= total_bits) continue;
            // bit-packed: byte = bit_offset / 8, bit = 7 - (bit_offset % 8)
            uint8_t bit = (packed[bit_offset >> 3] >> (7 - (bit_offset & 7))) & 1;
            if (bit == 0) continue;
            dst[dy * dst_w + dx] = be;
        }
    }
}

// 画单个 ASCII 字符 (含 column-spacing).
static void draw_ascii_char(uint16_t* dst, int dst_w, int dst_h,
                            int x_left, int y_top,
                            unsigned char c, uint16_t color) {
    if (c < 32 || c > 127) return;
    const uint8_t* glyph = kAscii5x7[c - 32];
    uint16_t be = (uint16_t)((color >> 8) | (color << 8));
    for (int col = 0; col < 5; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 7; row++) {
            if ((bits & (1u << row)) == 0) continue;
            int px = x_left + col;
            int py = y_top + row;
            if (px < 0 || px >= dst_w || py < 0 || py >= dst_h) continue;
            dst[py * dst_w + px] = be;
        }
    }
}

// 画一行 UTF-8 文字 (ASCII + 中文):
//   - ASCII 0..127           -> 5x7 内置表 (7px 高, fast path)
//   - CJK >=128 (utf-8 中日韩) -> font_puhui_basic_14_1 (MSB-first packed bit)
// 每个字符在它自己的 baseline 上画. 5x7 字符 baseline = y_top + 6,
// CJK 字符 baseline = y_baseline.
//
// x_left: 最左字符的 x 坐标.
// y_baseline: 文字公共 baseline (CJK 用). 5x7 字符顶部 = y_baseline - 6.
// 返回字符串实际占用的 x 宽度.
static int draw_text(uint16_t* dst, int dst_w, int dst_h,
                     int x_left, int y_baseline,
                     const char* s, uint16_t color) {
    if (s == nullptr) return 0;
    int x = x_left;
    int i = 0;
    while (s[i] != '\0') {
        uint32_t cp = utf8_next(s, &i);
        if (cp == 0) break;
        if (cp < 128) {
            // ASCII: 用 5x7 表, 字符顶部 = y_baseline - 6
            int y_top = y_baseline - 6;
            draw_ascii_char(dst, dst_w, dst_h, x, y_top, (unsigned char)cp, color);
            x += kAsciiCharW;
        } else {
            // CJK / 非 ASCII: 通过 lvgl font 查 packed bitmap
            lv_font_glyph_dsc_t g_dsc;
            if (lv_font_get_glyph_dsc(s_font, &g_dsc, cp, 0)) {
                draw_cjk_glyph(dst, dst_w, dst_h, x, y_baseline, &g_dsc, color);
                // dsc.adv_w 是像素 (lvgl 已从 fixed-point >>8)
                int adv = (int)g_dsc.adv_w;
                if (adv <= 0) adv = 14;  // fallback
                x += adv;
            } else {
                // 字形缺失: 用 '?' 占位
                int y_top = y_baseline - 6;
                draw_ascii_char(dst, dst_w, dst_h, x, y_top, '?', color);
                x += kAsciiCharW;
                if (cp == 0xFFFD) {
                    // skip remaining continuation bytes (already consumed)
                }
            }
        }
    }
    return x - x_left;
}

// 根据 priority 确定是否弹窗 (kPriorityState=1 以下的静默状态不弹)
static bool should_show_dialog(uint8_t priority) {
    return priority >= kPriorityInfo;
}

// 计算弹窗位置和尺寸 (居中于屏幕)
static void calc_dialog_box(int dst_w, int dst_h,
                           int* out_x, int* out_y, int* out_w, int* out_h) {
    static constexpr int kDialogBoxMarginX = 16;
    static constexpr int kDialogBoxH = 36;
    int w = dst_w - 2 * kDialogBoxMarginX;
    int h = kDialogBoxH;
    int x = kDialogBoxMarginX;
    int y = (dst_h - h) / 2;
    *out_x = x;
    *out_y = y;
    *out_w = w;
    *out_h = h;
}

// 同步保护 DialogState 跨 task 复制 (写者调 Set 改 state, 读者 splash task 每帧 copy).
static portMUX_TYPE s_dialog_spinlock = portMUX_INITIALIZER_UNLOCKED;

// 把 EAF 帧 src (frame_w x frame_h) 居中绘制到 dst (dst_w x dst_h),
// 不做任何缩放 / 旋转 / 镜像. 屏幕方向完全由 panel 的 swap_xy / mirror (MADCTL) 控制.
//
// src 比 dst 小或相等时居中绘制, 周围填 0 (黑色);
// src 比 dst 大时按左上角对齐并裁剪.
static void build_centered(
    const uint16_t* src,
    int frame_w, int frame_h,
    uint16_t* dst,
    int dst_w, int dst_h) {
    std::memset(dst, 0, (size_t)dst_w * dst_h * sizeof(uint16_t));

    int x_off = (dst_w - frame_w) / 2;
    int y_off = (dst_h - frame_h) / 2;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;

    const int copy_w = (frame_w < (dst_w - x_off)) ? frame_w : (dst_w - x_off);
    const int copy_h = (frame_h < (dst_h - y_off)) ? frame_h : (dst_h - y_off);

    for (int y = 0; y < copy_h; y++) {
        const uint16_t* src_row = src + y * frame_w;
        uint16_t* dst_row = dst + (y + y_off) * dst_w + x_off;
        std::memcpy(dst_row, src_row, (size_t)copy_w * sizeof(uint16_t));
    }
}

static void copy_dialog_snapshot(const DialogState* src, DialogState* dst) {
    if (src == nullptr) {
        dst->active = false;
        dst->priority = 0;
        dst->title[0] = '\0';
        dst->body[0]  = '\0';
        dst->color = 0xFFFF;
        dst->show_until_tick = 0;
        return;
    }
    portENTER_CRITICAL(&s_dialog_spinlock);
    dst->active = src->active;
    dst->priority = src->priority;
    dst->color  = src->color;
    dst->show_until_tick = src->show_until_tick;
    std::memcpy(dst->title, src->title, sizeof(dst->title));
    std::memcpy(dst->body,  src->body,  sizeof(dst->body));
    portEXIT_CRITICAL(&s_dialog_spinlock);
}

// 跨 task 安全写入 (持有临界区 - 在两个可能的并发 writer 之间互斥).
// 3 参数版本: body 为空 (默认单行弹窗).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color) {
    set_dialog_state(state, active, title, color, nullptr);
}

// 4 参数版本: 显式指定优先级
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority) {
    set_dialog_state(state, active, title, color, priority, nullptr, DIALOG_DEFAULT_TIMEOUT_MS);
}

// 5 参数版本: title + body 两行弹窗.
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, const char* body) {
    set_dialog_state(state, active, title, color, moss_splash::kPriorityInfo, body, DIALOG_DEFAULT_TIMEOUT_MS);
}

// 6 参数版本: priority + body (优先级保护 + 两行弹窗, 默认 10s 超时).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority, const char* body) {
    set_dialog_state(state, active, title, color, priority, body, DIALOG_DEFAULT_TIMEOUT_MS);
}

// 7 参数版本: priority + body + timeout (ms, 0=无超时, 默认 DIALOG_DEFAULT_TIMEOUT_MS).
void set_dialog_state(DialogState* state, bool active, const char* title, uint16_t color, uint8_t priority, const char* body, int timeout_ms) {
    if (state == nullptr) return;
    portENTER_CRITICAL(&s_dialog_spinlock);
    if (state->active && priority < state->priority) {
        portEXIT_CRITICAL(&s_dialog_spinlock);
        return;
    }
    state->active = active;
    state->color  = color;
    state->priority = priority;
    state->show_until_tick = 0;
    if (active && timeout_ms > 0) {
        state->show_until_tick = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    }
    state->title[0] = '\0';
    if (title != nullptr) {
        size_t i = 0;
        for (; i < sizeof(state->title) - 1 && title[i] != '\0'; i++) {
            state->title[i] = title[i];
        }
        state->title[i] = '\0';
    }
    state->body[0] = '\0';
    if (body != nullptr) {
        size_t i = 0;
        for (; i < sizeof(state->body) - 1 && body[i] != '\0'; i++) {
            state->body[i] = body[i];
        }
        state->body[i] = '\0';
    }
    portEXIT_CRITICAL(&s_dialog_spinlock);
}

void play_splash(esp_lcd_panel_handle_t panel,
                 const uint8_t* bin_base, size_t bin_size,
                 const char* asset_name) {
    if (panel == nullptr) {
        ESP_LOGE(TAG, "Invalid panel");
        return;
    }

    AssetInfo asset;
    if (!find_asset_in_bin(bin_base, bin_size, asset_name, asset)) {
        ESP_LOGE(TAG, "Asset '%s' not found in MMAP bin (%zu bytes)",
                 asset_name, bin_size);
        return;
    }
    ESP_LOGI(TAG, "Found asset '%s', size=%u bytes", asset_name, asset.size);

    eaf_format_handle_t parser = nullptr;
    esp_err_t ret = eaf_init(asset.data, asset.size, &parser);
    if (ret != ESP_OK || parser == nullptr) {
        ESP_LOGE(TAG, "eaf_init failed: %s", esp_err_to_name(ret));
        return;
    }

    int total_frames = eaf_get_total_frames(parser);
    ESP_LOGI(TAG, "EAF: %d frames", total_frames);
    if (total_frames <= 0) {
        eaf_deinit(parser);
        return;
    }

    eaf_header_t first_hdr;
    if (eaf_get_frame_info(parser, 0, &first_hdr) != EAF_FORMAT_VALID) {
        ESP_LOGE(TAG, "Failed to read first frame header");
        eaf_deinit(parser);
        return;
    }
    const int frame_w = first_hdr.width;
    const int frame_h = first_hdr.height;
    const int frame_buf_size = frame_w * frame_h * sizeof(uint16_t) + 1024;

    // JPEG hardware path needs 16-byte aligned output buffers on ESP32-S3.
    uint16_t* frame_buf = (uint16_t*)heap_caps_aligned_alloc(
        16, frame_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!frame_buf) {
        frame_buf = (uint16_t*)heap_caps_aligned_alloc(
            16, frame_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!frame_buf) {
        ESP_LOGE(TAG, "Failed to allocate aligned frame buffer");
        eaf_deinit(parser);
        eaf_free_header(&first_hdr);
        return;
    }

    const int panel_w = DISPLAY_WIDTH;
    const int panel_h = DISPLAY_HEIGHT;
    const int out_buf_size = panel_w * panel_h * sizeof(uint16_t);
    uint16_t* out_buf = (uint16_t*)heap_caps_aligned_alloc(
        16, out_buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!out_buf) {
        out_buf = (uint16_t*)heap_caps_aligned_alloc(
            16, out_buf_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!out_buf) {
        ESP_LOGE(TAG, "Failed to allocate aligned output buffer");
        heap_caps_free(frame_buf);
        eaf_deinit(parser);
        eaf_free_header(&first_hdr);
        return;
    }

    const int fps = 30;
    const int frame_delay_ms = 1000 / fps;

    ESP_LOGI(TAG, "Playing %d frames at %d fps (%d ms) on %dx%d panel [NO-TEXT MODE]",
             total_frames, fps, total_frames * frame_delay_ms, panel_w, panel_h);

    int64_t t_start = esp_timer_get_time();
    for (int i = 0; i < total_frames; i++) {
        std::memset(frame_buf, 0, frame_buf_size);
        esp_err_t dr = eaf_frame_decode(parser, i, (uint8_t*)frame_buf, frame_buf_size, true);
        if (dr != ESP_OK) {
            ESP_LOGW(TAG, "Failed to decode frame %d", i);
            continue;
        }
        build_centered(frame_buf, frame_w, frame_h, out_buf, panel_w, panel_h);
        esp_lcd_panel_draw_bitmap(panel, 0, 0, panel_w, panel_h, out_buf);

        int64_t t_now = esp_timer_get_time();
        int64_t total_elapsed_ms = (t_now - t_start) / 1000;
        int64_t expected_ms = (int64_t)(i + 1) * frame_delay_ms;
        if (total_elapsed_ms < expected_ms) {
            vTaskDelay(pdMS_TO_TICKS(expected_ms - total_elapsed_ms));
        }
    }
    printf("SplashPlayer: asset '%s' done\n", asset_name);

    heap_caps_free(out_buf);
    heap_caps_free(frame_buf);
    eaf_free_header(&first_hdr);
    eaf_deinit(parser);
}

void dismiss_dialog(DialogState* state) {
    if (state == nullptr) return;
    portENTER_CRITICAL(&s_dialog_spinlock);
    state->active = false;
    state->title[0] = '\0';
    state->body[0] = '\0';
    state->priority = 0;
    state->show_until_tick = 0;
    portEXIT_CRITICAL(&s_dialog_spinlock);
}

}  // namespace moss_splash
