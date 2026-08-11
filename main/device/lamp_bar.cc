#include "lamp_bar.h"
#include <esp_log.h>
#include "../mcp/utils/pca9685_driver.h"

#define TAG "LampBarDevice"

LampBarDevice::LampBarDevice() : power_(false), flowing_(false), flow_task_(nullptr) {}

LampBarDevice::~LampBarDevice() {
    if (flow_task_ != nullptr) {
        flowing_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        flow_task_ = nullptr;
    }
}

void LampBarDevice::ApplyPattern(uint8_t mask5) {
    auto& pca = Pca9685::GetInstance();
    for (int i = 0; i < LED_COUNT; ++i) {
        pca.SetDigital(CH_FLOW_BASE + i, (mask5 >> i) & 0x1);
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
        ESP_LOGI(TAG, "Flow effect started");
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
        // 流水灯效果1: 两端亮，中间暗
        instance->ApplyPattern(0b10001);  // LED0和LED4亮
        vTaskDelay(pdMS_TO_TICKS(110));

        // 流水灯效果2: 交替亮
        instance->ApplyPattern(0b01010);  // LED1和LED3亮
        vTaskDelay(pdMS_TO_TICKS(100));

        // 流水灯效果3: 中间亮
        instance->ApplyPattern(0b00110);  // LED2和LED3亮
        vTaskDelay(pdMS_TO_TICKS(90));

        // 流水灯效果4: 交叉亮
        instance->ApplyPattern(0b10110);  // LED0, LED2, LED3亮
        vTaskDelay(pdMS_TO_TICKS(100));

        // 流水灯效果5: 两端亮
        instance->ApplyPattern(0b10001);  // LED0和LED4亮
        vTaskDelay(pdMS_TO_TICKS(130));
    }

    instance->ApplyPattern(0);
    vTaskDelete(NULL);
}

LampBarDevice& LampBarDevice::GetInstance() {
    static LampBarDevice instance;
    return instance;
}
