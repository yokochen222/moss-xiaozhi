#include "lamp_panel.h"
#include "74hc595_driver.h"
#include <esp_log.h>

#define TAG "LampPanelDevice"

LampPanelDevice::LampPanelDevice()
    : panel_led_1_on_(false),
      panel_led_2_on_(false),
      bottom_led_on_(false),
      shift_register_(nullptr) {
    InitializeShiftRegister();
}

LampPanelDevice::~LampPanelDevice() {
    if (shift_register_) {
        delete shift_register_;
        shift_register_ = nullptr;
    }
}

void LampPanelDevice::InitializeShiftRegister() {
    shift_register_ = new ShiftRegister74HC595(SER_PIN, RCK_PIN, SCK_PIN);
    shift_register_->Initialize();

    // 初始化时三盏灯全部熄灭
    ApplyOutputs();
}

void LampPanelDevice::ApplyOutputs() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return;
    }

    // 写入面板/底灯位。SetPanelBit 是 74HC595 驱动专为高 3 位 (Q5/Q6/Q7)
    // 提供的通道 —— 它会把状态保存到共享 panel_state_，并在写入后立即
    // 把合并后的 8 位 (Q0-Q4 流水灯当前值 + Q5-Q7 面板/底灯当前值)
    // 锁存到硬件。从而：
    //   * 面板/底灯的写入 不会 冲掉流水灯当前在 Q0-Q4 上的状态
    //   * 流水灯的写入 不会 冲掉面板/底灯当前在 Q5-Q7 上的状态
    shift_register_->SetPanelBit(PANEL_LED_1_BIT, panel_led_1_on_);
    shift_register_->SetPanelBit(PANEL_LED_2_BIT, panel_led_2_on_);
    shift_register_->SetPanelBit(BOTTOM_LED_BIT,  bottom_led_on_);
}

bool LampPanelDevice::TurnOffAll() {
    if (!shift_register_) return false;
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    bottom_led_on_  = false;
    ApplyOutputs();
    ESP_LOGI(TAG, "面板灯与底灯全部熄灭");
    return true;
}

bool LampPanelDevice::TurnOnPanelLeds() {
    if (!shift_register_) return false;
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    ApplyOutputs();
    ESP_LOGI(TAG, "前面板灯已点亮");
    return true;
}

bool LampPanelDevice::TurnOffPanelLeds() {
    if (!shift_register_) return false;
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    ApplyOutputs();
    ESP_LOGI(TAG, "前面板灯已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnBottomLed() {
    if (!shift_register_) return false;
    bottom_led_on_ = true;
    ApplyOutputs();
    ESP_LOGI(TAG, "底灯已点亮");
    return true;
}

bool LampPanelDevice::TurnOffBottomLed() {
    if (!shift_register_) return false;
    bottom_led_on_ = false;
    ApplyOutputs();
    ESP_LOGI(TAG, "底灯已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnAll() {
    if (!shift_register_) return false;
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    bottom_led_on_  = true;
    ApplyOutputs();
    ESP_LOGI(TAG, "面板灯与底灯全部点亮");
    return true;
}

LampPanelDevice& LampPanelDevice::GetInstance() {
    static LampPanelDevice instance;
    return instance;
}
