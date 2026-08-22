#include "lamp_bar.h"
#include "74hc595_driver.h"
#include <esp_log.h>

#define TAG "LampBarDevice"

LampBarDevice::LampBarDevice() : power_(false), flowing_(false), flow_task_(nullptr), shift_register_(nullptr) {
    InitializeShiftRegister();
}

LampBarDevice::~LampBarDevice() {
    if (flow_task_ != nullptr) {
        flowing_ = false;
        WaitFlowTaskExit(2000);
    }

    if (shift_register_) {
        delete shift_register_;
    }
}

void LampBarDevice::InitializeShiftRegister() {
    shift_register_ = new ShiftRegister74HC595(SER_PIN, RCK_PIN, SCK_PIN);
    shift_register_->Initialize();
    shift_register_->ClearAll();
}

void LampBarDevice::WaitFlowTaskExit(int max_ms) {
    const int step_ms = 50;
    int waited = 0;
    while (flow_task_ != nullptr && waited < max_ms) {
        vTaskDelay(pdMS_TO_TICKS(step_ms));
        waited += step_ms;
    }
    if (flow_task_ != nullptr) {
        ESP_LOGW(TAG, "Force deleting flow task");
        vTaskDelete(flow_task_);
        flow_task_ = nullptr;
    }
}

bool LampBarDevice::StartFlow() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return false;
    }

    if (flowing_ && flow_task_ != nullptr) {
        return true;
    }

    if (flow_task_ != nullptr) {
        ESP_LOGW(TAG, "Cleaning stale flow task before start");
        flowing_ = false;
        WaitFlowTaskExit(2000);
    }

    flowing_ = true;
    power_ = true;
    BaseType_t result = xTaskCreate(FlowTask, "FlowTask", 2560, this, 5, &flow_task_);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create flow task");
        flowing_ = false;
        power_ = false;
        flow_task_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "Flow effect started");
    return true;
}

bool LampBarDevice::StopFlow() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return false;
    }

    if (!flowing_ && flow_task_ == nullptr) {
        return true;
    }

    flowing_ = false;
    power_ = false;
    WaitFlowTaskExit(2000);
    shift_register_->ClearAll();
    ESP_LOGI(TAG, "Flow effect stopped");
    return true;
}

bool LampBarDevice::ResetDriver() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return false;
    }

    shift_register_->Reset();
    ESP_LOGI(TAG, "Driver reset");
    return true;
}

bool LampBarDevice::ForceRestart() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return false;
    }

    flowing_ = false;
    power_ = false;
    WaitFlowTaskExit(2000);
    shift_register_->Reset();
    shift_register_->ClearAll();
    vTaskDelay(pdMS_TO_TICKS(500));
    ESP_LOGI(TAG, "Force restart completed");
    return true;
}

void LampBarDevice::FlowTask(void* arg) {
    LampBarDevice* instance = static_cast<LampBarDevice*>(arg);

    while (instance->flowing_) {
        uint8_t pattern1 = 0b10001;
        instance->shift_register_->SetOutputs(pattern1);
        vTaskDelay(pdMS_TO_TICKS(110));

        uint8_t pattern2 = 0b01010;
        instance->shift_register_->SetOutputs(pattern2);
        vTaskDelay(pdMS_TO_TICKS(100));

        uint8_t pattern3 = 0b00110;
        instance->shift_register_->SetOutputs(pattern3);
        vTaskDelay(pdMS_TO_TICKS(90));

        uint8_t pattern4 = 0b10110;
        instance->shift_register_->SetOutputs(pattern4);
        vTaskDelay(pdMS_TO_TICKS(100));

        uint8_t pattern5 = 0b10001;
        instance->shift_register_->SetOutputs(pattern5);
        vTaskDelay(pdMS_TO_TICKS(130));
    }

    instance->shift_register_->ClearAll();
    instance->flow_task_ = nullptr;
    vTaskDelete(nullptr);
}

LampBarDevice& LampBarDevice::GetInstance() {
    static LampBarDevice instance;
    return instance;
}
