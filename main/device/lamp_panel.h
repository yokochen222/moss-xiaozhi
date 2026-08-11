#pragma once

#include <cstdint>

class LampPanelDevice {
private:
    // PCA9685 channels for panel / bottom LEDs
    static constexpr uint8_t CH_PANEL1 = 13;
    static constexpr uint8_t CH_PANEL2 = 14;
    static constexpr uint8_t CH_BOTTOM = 15;

    bool panel_led_1_on_;
    bool panel_led_2_on_;
    bool bottom_led_on_;

    bool ApplyOutputs();

public:
    LampPanelDevice();
    ~LampPanelDevice();

    // 禁用拷贝构造和赋值
    LampPanelDevice(const LampPanelDevice&) = delete;
    LampPanelDevice& operator=(const LampPanelDevice&) = delete;

    // 三盏灯全部熄灭
    bool TurnOffAll();

    // 仅亮两盏前面板灯（面板灯1 + 面板灯2）
    bool TurnOnPanelLeds();
    // 仅熄灭两盏前面板灯
    bool TurnOffPanelLeds();

    // 仅亮底灯
    bool TurnOnBottomLed();
    // 仅熄灭底灯
    bool TurnOffBottomLed();

    // 三盏灯全部点亮
    bool TurnOnAll();

    // 状态查询
    bool IsPanelLed1On() const { return panel_led_1_on_; }
    bool IsPanelLed2On() const { return panel_led_2_on_; }
    bool IsBottomLedOn() const { return bottom_led_on_; }
    bool IsAnyLedOn() const { return panel_led_1_on_ || panel_led_2_on_ || bottom_led_on_; }

    // 获取单例实例
    static LampPanelDevice& GetInstance();
};
