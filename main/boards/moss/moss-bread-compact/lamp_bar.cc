#include "device/lamp_bar.h"

#include <esp_log.h>

#define TAG "LampBarDevice"

gpio_num_t LampBarDevice::Pin(int index) {
    static const gpio_num_t pins[LED_COUNT] = {
        MOSS_LAMP_BAR_PIN0, MOSS_LAMP_BAR_PIN1, MOSS_LAMP_BAR_PIN2, MOSS_LAMP_BAR_PIN3,
        MOSS_LAMP_BAR_PIN4,
    };
    return pins[index];
}

LampBarDevice::LampBarDevice()
    : power_(false), flowing_(false), ready_(false), flow_task_(nullptr) {}

bool LampBarDevice::EnsureReady() {
    if (ready_) {
        return true;
    }
    InitializeGpio();
    ready_ = true;
    return true;
}

void LampBarDevice::Initialize() {
    EnsureReady();
}

LampBarDevice::~LampBarDevice() {
    if (flow_task_ != nullptr) {
        flowing_ = false;
        WaitFlowTaskExit(2000);
    }
}

void LampBarDevice::InitializeGpio() {
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_config_t config = {
            .pin_bit_mask = (1ULL << Pin(i)),
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        ESP_ERROR_CHECK(gpio_config(&config));
        gpio_set_level(Pin(i), 0);
    }
}

void LampBarDevice::ClearAll() {
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_set_level(Pin(i), 0);
    }
}

void LampBarDevice::ApplyPattern(uint8_t mask5) {
    for (int i = 0; i < LED_COUNT; i++) {
        gpio_set_level(Pin(i), (mask5 >> i) & 0x01);
    }
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
    if (!EnsureReady()) {
        return false;
    }
    if (flowing_ && flow_task_ != nullptr) {
        return true;
    }
    if (flow_task_ != nullptr) {
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
    ESP_LOGI(TAG, "GPIO flow effect started");
    return true;
}

bool LampBarDevice::StopFlow() {
    if (!EnsureReady()) {
        return false;
    }
    if (!flowing_ && flow_task_ == nullptr) {
        return true;
    }
    flowing_ = false;
    power_ = false;
    WaitFlowTaskExit(2000);
    ClearAll();
    ESP_LOGI(TAG, "GPIO flow effect stopped");
    return true;
}

bool LampBarDevice::ResetDriver() {
    if (!EnsureReady()) {
        return false;
    }
    ClearAll();
    return true;
}

bool LampBarDevice::ForceRestart() {
    if (!EnsureReady()) {
        return false;
    }
    flowing_ = false;
    power_ = false;
    WaitFlowTaskExit(2000);
    ClearAll();
    vTaskDelay(pdMS_TO_TICKS(500));
    return true;
}

void LampBarDevice::FlowTask(void* arg) {
    auto* instance = static_cast<LampBarDevice*>(arg);
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
    instance->ClearAll();
    instance->flow_task_ = nullptr;
    vTaskDelete(nullptr);
}

LampBarDevice& LampBarDevice::GetInstance() {
    static LampBarDevice instance;
    return instance;
}
