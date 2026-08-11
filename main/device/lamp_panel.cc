#include "lamp_panel.h"
#include <esp_log.h>
#include "../mcp/utils/pca9685_driver.h"

#define TAG "LampPanelDevice"

LampPanelDevice::LampPanelDevice()
    : panel_led_1_on_(false), panel_led_2_on_(false), bottom_led_on_(false) {}

LampPanelDevice::~LampPanelDevice() = default;

bool LampPanelDevice::ApplyOutputs() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    pca.SetDigital(CH_PANEL1, panel_led_1_on_);
    pca.SetDigital(CH_PANEL2, panel_led_2_on_);
    pca.SetDigital(CH_BOTTOM, bottom_led_on_);
    return true;
}

bool LampPanelDevice::TurnOffAll() {
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    bottom_led_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "面板灯与底灯全部熄灭");
    return true;
}

bool LampPanelDevice::TurnOnPanelLeds() {
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前面板灯已点亮");
    return true;
}

bool LampPanelDevice::TurnOffPanelLeds() {
    panel_led_1_on_ = false;
    panel_led_2_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "前面板灯已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnBottomLed() {
    bottom_led_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "底灯已点亮");
    return true;
}

bool LampPanelDevice::TurnOffBottomLed() {
    bottom_led_on_ = false;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "底灯已熄灭");
    return true;
}

bool LampPanelDevice::TurnOnAll() {
    panel_led_1_on_ = true;
    panel_led_2_on_ = true;
    bottom_led_on_ = true;
    if (!ApplyOutputs())
        return false;
    ESP_LOGI(TAG, "面板灯与底灯全部点亮");
    return true;
}

LampPanelDevice& LampPanelDevice::GetInstance() {
    static LampPanelDevice instance;
    return instance;
}
