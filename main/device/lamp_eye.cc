#include "lamp_eye.h"
#include <esp_log.h>
#include <climits>
#include "../mcp/utils/pca9685_driver.h"

#define TAG "LampEyeDevice"

LampEyeDevice::LampEyeDevice()
    : power_(false),
      breathing_(false),
      pause_(false),
      breathing_task_handle_(nullptr),
      pwm_mutex_(nullptr) {
    pwm_mutex_ = xSemaphoreCreateMutex();
}

LampEyeDevice::~LampEyeDevice() {
    // 确保停止呼吸模式
    if (breathing_ && breathing_task_handle_) {
        // 发送停止通知
        xTaskNotify(breathing_task_handle_, STOP_NOTIFICATION, eSetBits);

        breathing_ = false;
        pause_ = false;
        power_ = false;

        // 等待任务退出
        int wait_count = 0;
        while (breathing_task_handle_ && wait_count < 40) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_count++;
        }

        // 强制删除任务
        if (breathing_task_handle_) {
            vTaskDelete(breathing_task_handle_);
            breathing_task_handle_ = nullptr;
        }

        // 关闭灯光
        SetDuty(0);
    }

    // 删除互斥锁
    if (pwm_mutex_) {
        vSemaphoreDelete(pwm_mutex_);
        pwm_mutex_ = nullptr;
    }
}

void LampEyeDevice::SetDuty(int duty) {
    uint16_t d = static_cast<uint16_t>(duty);
    if (d > MAX_DUTY) {
        d = MAX_DUTY;
    }

    auto apply = [d]() { Pca9685::GetInstance().SetDuty(CH, d); };

    if (pwm_mutex_) {
        BaseType_t result = xSemaphoreTake(pwm_mutex_, pdMS_TO_TICKS(200));
        if (result == pdTRUE) {
            apply();
            xSemaphoreGive(pwm_mutex_);
        } else {
            ESP_LOGW(TAG, "获取PWM互斥锁超时，强制设置duty");
            apply();
        }
    } else {
        ESP_LOGW(TAG, "PWM互斥锁未初始化，直接设置duty");
        apply();
    }
}

bool LampEyeDevice::TurnOn() {
    if (!Pca9685::GetInstance().IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }
    power_ = true;
    SetDuty(MAX_DUTY);
    ESP_LOGI(TAG, "Eye light turned on");
    return true;
}

bool LampEyeDevice::TurnOff() {
    power_ = false;
    SetDuty(0);
    ESP_LOGI(TAG, "Eye light turned off");
    return true;
}

bool LampEyeDevice::StartBreathing() {
    if (!Pca9685::GetInstance().IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    if (!breathing_) {
        breathing_ = true;
        pause_ = false;
        BaseType_t result =
            xTaskCreate(BreathingTask, "BreathingTask", 2048, this, 5, &breathing_task_handle_);
        if (result != pdPASS) {
            ESP_LOGE(TAG, "Failed to create breathing task");
            breathing_ = false;
            pause_ = false;
            return false;
        }
        ESP_LOGI(TAG, "Breathing effect started");
        return true;
    }
    return true;  // 已经在运行
}

bool LampEyeDevice::PauseBreathing() {
    if (breathing_) {
        pause_ = true;
        ESP_LOGI(TAG, "Breathing effect paused");
        return true;
    }
    return false;  // 没有在呼吸模式
}

bool LampEyeDevice::ResumeBreathing() {
    if (breathing_) {
        pause_ = false;
        ESP_LOGI(TAG, "Breathing effect resumed");
        return true;
    }
    return false;  // 没有在呼吸模式
}

bool LampEyeDevice::StopBreathing() {
    if (breathing_ && breathing_task_handle_) {
        ESP_LOGI(TAG, "Stopping breathing mode");

        // 发送停止通知给任务
        xTaskNotify(breathing_task_handle_, STOP_NOTIFICATION, eSetBits);

        // 设置状态标志
        breathing_ = false;
        pause_ = false;
        power_ = false;

        // 等待任务自然退出（最多等待2秒）
        int wait_count = 0;
        while (breathing_task_handle_ && wait_count < 40) {
            vTaskDelay(pdMS_TO_TICKS(50));
            wait_count++;
        }

        // 如果任务仍然存在，强制删除
        if (breathing_task_handle_) {
            ESP_LOGW(TAG, "Force deleting breathing task");
            vTaskDelete(breathing_task_handle_);
            breathing_task_handle_ = nullptr;
        }

        // 确保灯光关闭
        SetDuty(0);
        ESP_LOGI(TAG, "Breathing mode stopped");
        return true;
    }
    return true;  // 已经停止
}

void LampEyeDevice::BreathingTask(void* arg) {
    LampEyeDevice* instance = static_cast<LampEyeDevice*>(arg);
    int direction = 1;
    int duty = 0;
    uint32_t notification_value = 0;

    ESP_LOGI(TAG, "Breathing task started");

    while (true) {
        // 等待通知或超时
        BaseType_t result =
            xTaskNotifyWait(0x00, ULONG_MAX, &notification_value, pdMS_TO_TICKS(50));

        // 检查停止通知
        if (result == pdTRUE && (notification_value & STOP_NOTIFICATION)) {
            ESP_LOGI(TAG, "Received stop notification, exiting breathing task");
            break;
        }

        // 检查暂停状态
        if (instance->pause_) {
            continue;
        }

        // 更新duty
        // Step 50 keeps cycle time close to the old 13-bit LEDC feel at MAX_DUTY=4095.
        duty += direction * 50;
        if (duty >= MAX_DUTY) {
            duty = MAX_DUTY;
            direction = -1;
        } else if (duty <= 0) {
            duty = 0;
            direction = 1;
        }

        instance->SetDuty(duty);
    }

    // 确保灯光关闭
    instance->SetDuty(0);
    ESP_LOGI(TAG, "Breathing task ended");

    // 清除任务句柄
    instance->breathing_task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

LampEyeDevice& LampEyeDevice::GetInstance() {
    static LampEyeDevice instance;
    return instance;
}
