#include "lamp_bar.h"
#include <esp_log.h>
#include "pca9685_driver.h"

#define TAG "LampBarDevice"

LampBarDevice::LampBarDevice()
    : power_(false), flowing_(false), brightness_(kDefaultBrightnessPercent), flow_task_(nullptr) {}

LampBarDevice::~LampBarDevice() {
    if (flow_task_ != nullptr) {
        flowing_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        flow_task_ = nullptr;
    }
}

uint8_t LampBarDevice::ClampBrightness(int percent) {
    if (percent < 1) {
        return 1;
    }
    if (percent > kPwmCeilingPercent) {
        return kPwmCeilingPercent;
    }
    return static_cast<uint8_t>(percent);
}

uint16_t LampBarDevice::OnDuty() const {
    return static_cast<uint16_t>((static_cast<uint32_t>(brightness_) * MAX_DUTY) / 100);
}

void LampBarDevice::ApplyPattern(uint8_t mask5) {
    auto& pca = Pca9685::GetInstance();
    const uint16_t on_duty = OnDuty();
    for (int i = 0; i < LED_COUNT; ++i) {
        pca.SetDuty(CH_FLOW_BASE + i, ((mask5 >> i) & 0x1) ? on_duty : 0);
    }
}

bool LampBarDevice::StartFlow() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    if (!flowing_) {
        flowing_ = true;
        power_ = true;
        BaseType_t result = xTaskCreate(FlowTask, "FlowTask", 2048, this, 5, &flow_task_);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create flow task");
            flowing_ = false;
            power_ = false;
            return false;
        }
        ESP_LOGI(TAG, "Flow effect started brightness=%u%%", brightness_);
    }
    return true;
}

bool LampBarDevice::StopFlow() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    if (flowing_) {
        flowing_ = false;
        power_ = false;

        if (flow_task_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100));
            flow_task_ = nullptr;
        }

        ApplyPattern(0);
        ESP_LOGI(TAG, "Flow effect stopped");
        return true;
    }
    return true;
}

bool LampBarDevice::SetBrightness(int percent) {
    brightness_ = ClampBrightness(percent);
    ESP_LOGI(TAG, "Flow brightness set to %u%% (ceiling %u%%)", brightness_, kPwmCeilingPercent);
    return true;
}

bool LampBarDevice::ResetDriver() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    ApplyPattern(0);
    ESP_LOGI(TAG, "Driver reset");
    return true;
}

bool LampBarDevice::ForceRestart() {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    if (flowing_) {
        flowing_ = false;
        power_ = false;

        if (flow_task_ != nullptr) {
            flow_task_ = nullptr;
        }
    }

    ApplyPattern(0);
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, "Force restart completed");
    return true;
}

void LampBarDevice::FlowTask(void* arg) {
    LampBarDevice* instance = static_cast<LampBarDevice*>(arg);

    while (instance->flowing_) {
        instance->ApplyPattern(0b10001);
        vTaskDelay(pdMS_TO_TICKS(110));

        instance->ApplyPattern(0b01010);
        vTaskDelay(pdMS_TO_TICKS(100));

        instance->ApplyPattern(0b00110);
        vTaskDelay(pdMS_TO_TICKS(90));

        instance->ApplyPattern(0b10110);
        vTaskDelay(pdMS_TO_TICKS(100));

        instance->ApplyPattern(0b10001);
        vTaskDelay(pdMS_TO_TICKS(130));
    }

    instance->ApplyPattern(0);
    vTaskDelete(NULL);
}

LampBarDevice& LampBarDevice::GetInstance() {
    static LampBarDevice instance;
    return instance;
}
