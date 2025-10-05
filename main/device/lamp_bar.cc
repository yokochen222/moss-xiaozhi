#include "lamp_bar.h"
#include "../mcp/utils/74hc595_driver.h"
#include <esp_log.h>

#define TAG "LampBarDevice"

LampBarDevice::LampBarDevice() : power_(false), flowing_(false), flow_task_(nullptr), shift_register_(nullptr) {
    InitializeShiftRegister();
}

LampBarDevice::~LampBarDevice() {
    // 停止所有任务
    if (flow_task_ != nullptr) {
        flowing_ = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        flow_task_ = nullptr;
    }
    
    if (shift_register_) {
        delete shift_register_;
    }
}

void LampBarDevice::InitializeShiftRegister() {
    // 创建74HC595驱动实例
    shift_register_ = new ShiftRegister74HC595(SER_PIN, RCK_PIN, SCK_PIN);
    shift_register_->Initialize();
    
    // 初始化时关闭所有LED
    shift_register_->ClearAll();
}

bool LampBarDevice::StartFlow() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
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
        return true;
    }
    return true; // 已经在运行
}

bool LampBarDevice::StopFlow() {
    if (!shift_register_) {
        ESP_LOGE(TAG, "Shift register not initialized");
        return false;
    }
    
    if (flowing_) {
        flowing_ = false;
        power_ = false;
        
        // 等待任务结束
        if (flow_task_ != nullptr) {
            vTaskDelay(pdMS_TO_TICKS(100)); // 给任务一点时间结束
            flow_task_ = nullptr;
        }
        
        // 关闭所有LED
        shift_register_->ClearAll();
        ESP_LOGI(TAG, "Flow effect stopped");
        return true;
    }
    return true; // 已经停止
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
    
    // 强制停止当前任务
    if (flowing_) {
        flowing_ = false;
        power_ = false;
        
        if (flow_task_ != nullptr) {
            flow_task_ = nullptr;
        }
    }
    
    // 重置驱动
    shift_register_->Reset();
    
    // 关闭所有LED
    shift_register_->ClearAll();
    
    // 等待一下
    vTaskDelay(pdMS_TO_TICKS(500));
    
    ESP_LOGI(TAG, "Force restart completed");
    return true;
}

void LampBarDevice::FlowTask(void* arg) {
    LampBarDevice* instance = static_cast<LampBarDevice*>(arg);
    
    while(instance->flowing_) {
        // 流水灯效果1: 两端亮，中间暗
        uint8_t pattern1 = 0b10001;  // LED0和LED4亮
        instance->shift_register_->SetOutputs(pattern1);
        vTaskDelay(pdMS_TO_TICKS(110));
        
        // 流水灯效果2: 交替亮
        uint8_t pattern2 = 0b01010;  // LED1和LED3亮
        instance->shift_register_->SetOutputs(pattern2);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 流水灯效果3: 中间亮
        uint8_t pattern3 = 0b00110;  // LED2和LED3亮
        instance->shift_register_->SetOutputs(pattern3);
        vTaskDelay(pdMS_TO_TICKS(90));
        
        // 流水灯效果4: 交叉亮
        uint8_t pattern4 = 0b10110;  // LED0, LED2, LED3亮
        instance->shift_register_->SetOutputs(pattern4);
        vTaskDelay(pdMS_TO_TICKS(100));
        
        // 流水灯效果5: 两端亮
        uint8_t pattern5 = 0b10001;  // LED0和LED4亮
        instance->shift_register_->SetOutputs(pattern5);
        vTaskDelay(pdMS_TO_TICKS(130));
    }
    
    // 停止时关闭所有LED
    instance->shift_register_->ClearAll();
    vTaskDelete(NULL);
}

LampBarDevice& LampBarDevice::GetInstance() {
    static LampBarDevice instance;
    return instance;
}
