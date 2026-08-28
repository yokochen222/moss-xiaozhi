#include "lamp_panel.h"
#include <esp_log.h>
#include "../mcp/utils/pca9685_driver.h"

#define TAG "LampPanelDevice"

LampPanelDevice::LampPanelDevice()
    : panel_led_1_on_(false),
      panel_led_2_on_(false),
      bottom_led_on_(false),
      panel_brightness_(kDefaultBrightnessPercent) {}

LampPanelDevice::~LampPanelDevice() = default;

uint8_t LampPanelDevice::ClampBrightness(int percent) {
    if (percent < 1) {
        return 1;
    }
    if (percent > kPwmCeilingPercent) {
        return kPwmCeilingPercent;
    }
    return static_cast<uint8_t>(percent);
}

uint16_t LampPanelDevice::PercentToDuty(uint8_t percent) {
    return static_cast<uint16_t>((static_cast<uint32_t>(percent) * MAX_DUTY) / 100);
}

uint16_t LampPanelDevice::BottomDuty() { return PercentToDuty(kBottomFixedPwmPercent); }

uint16_t LampPanelDevice::PanelDuty() const { return PercentToDuty(panel_brightness_); }

bool LampPanelDevice::ApplyOutputs() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    pca.SetDuty(CH_PANEL1, panel_led_1_on_ ? PanelDuty() : 0);
    pca.SetDuty(CH_PANEL2, panel_led_2_on_ ? PanelDuty() : 0);
    pca.SetDuty(CH_BOTTOM, bottom_led_on_ ? BottomDuty() : 0);
    return true;
}

bool LampPanelDevice::TurnOffAll() {
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    bottom_led_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前舷信标与暗舷锚灯全部熄灭");
    return true;
}

bool LampPanelDevice::TurnOnPanelLeds() {
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前舷信标已点亮 brightness=%u%%", panel_brightness_);
    return true;
}

bool LampPanelDevice::TurnOffPanelLeds() {
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前舷信标已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnBottomLed() {
    bottom_led_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "暗舷锚灯已点亮 fixed=%u%%", kBottomFixedPwmPercent);
    return true;
}

bool LampPanelDevice::TurnOffBottomLed() {
    bottom_led_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "暗舷锚灯已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnAll() {
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    bottom_led_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前舷信标与暗舷锚灯全部点亮 panel=%u%% bottom=%u%%", panel_brightness_,
             kBottomFixedPwmPercent);
    return true;
}

bool LampPanelDevice::SetBrightness(int percent) {
    panel_brightness_ = ClampBrightness(percent);
    if ((panel_led_1_on_ || panel_led_2_on_) && !ApplyOutputs()) {
        return false;
    }
    ESP_LOGI(TAG, "Panel brightness set to %u%% (ceiling %u%%)", panel_brightness_,
             kPwmCeilingPercent);
    return true;
}

LampPanelDevice& LampPanelDevice::GetInstance() {
    static LampPanelDevice instance;
    return instance;
}
