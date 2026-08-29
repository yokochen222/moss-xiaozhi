#include "eye_motor.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "pca9685_driver.h"

#define TAG "EyeMotorDevice"

EyeMotorDevice::EyeMotorDevice()
    : state_(EYE_MOTOR_STATE_STOPPED),
      current_duty_(0),
      oscillating_(false),
      osc_speed_(DEFAULT_SPEED_PERCENT),
      osc_task_(nullptr) {}

EyeMotorDevice::~EyeMotorDevice() {
    Stop();
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool EyeMotorDevice::StartForward() { return StartForward(DEFAULT_SPEED_PERCENT); }

bool EyeMotorDevice::StartBackward() { return StartBackward(DEFAULT_SPEED_PERCENT); }

bool EyeMotorDevice::StartOscillate() { return StartOscillate(DEFAULT_SPEED_PERCENT); }

void EyeMotorDevice::CancelOscillate() {
    if (!oscillating_ && osc_task_ == nullptr) {
        return;
    }
    oscillating_ = false;
    if (osc_task_ != nullptr && xTaskGetCurrentTaskHandle() != osc_task_) {
        for (int i = 0; i < 20 && osc_task_ != nullptr; ++i) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        osc_task_ = nullptr;
    }
}

bool EyeMotorDevice::DriveForward(uint8_t speed_percent) {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    uint32_t duty = (static_cast<uint32_t>(speed_percent) * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    if (state_ == EYE_MOTOR_STATE_FORWARD && current_duty_ == duty) {
        return true;
    }

    if (state_ == EYE_MOTOR_STATE_BACKWARD) {
        pca.SetDigital(CH_AIN1, false);
        pca.SetDigital(CH_AIN2, false);
        pca.SetDuty(CH_PWM, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    pca.SetDigital(CH_AIN1, true);
    pca.SetDigital(CH_AIN2, false);
    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_FORWARD;
    ESP_LOGI(TAG, "电机正转启动, 速度: %d%%, duty=%lu", speed_percent,
             static_cast<unsigned long>(duty));
    return true;
}

bool EyeMotorDevice::DriveBackward(uint8_t speed_percent) {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    uint32_t duty = (static_cast<uint32_t>(speed_percent) * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    if (state_ == EYE_MOTOR_STATE_BACKWARD && current_duty_ == duty) {
        return true;
    }

    if (state_ == EYE_MOTOR_STATE_FORWARD) {
        pca.SetDigital(CH_AIN1, false);
        pca.SetDigital(CH_AIN2, false);
        pca.SetDuty(CH_PWM, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    pca.SetDigital(CH_AIN1, false);
    pca.SetDigital(CH_AIN2, true);
    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_BACKWARD;
    ESP_LOGI(TAG, "电机反转启动, 速度: %d%%, duty=%lu", speed_percent,
             static_cast<unsigned long>(duty));
    return true;
}

bool EyeMotorDevice::StartForward(uint8_t speed_percent) {
    CancelOscillate();
    return DriveForward(speed_percent);
}

bool EyeMotorDevice::StartBackward(uint8_t speed_percent) {
    CancelOscillate();
    return DriveBackward(speed_percent);
}

bool EyeMotorDevice::StartOscillate(uint8_t speed_percent) {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }
    if (speed_percent < 1) {
        speed_percent = 1;
    }
    if (speed_percent > 100) {
        speed_percent = 100;
    }
    if (oscillating_ && osc_task_ != nullptr && osc_speed_ == speed_percent) {
        return true;
    }

    CancelOscillate();
    oscillating_ = true;
    osc_speed_ = speed_percent;
    BaseType_t ok = xTaskCreate(OscillateTask, "eye_osc", 2048, this, 2, &osc_task_);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "Failed to create oscillate task");
        oscillating_ = false;
        osc_task_ = nullptr;
        return false;
    }
    ESP_LOGI(TAG, "电机预设启动: 正转/反转各 %dms, 速度: %d%%", OSC_PHASE_MS, speed_percent);
    return true;
}

void EyeMotorDevice::OscillateTask(void* arg) {
    auto* self = static_cast<EyeMotorDevice*>(arg);
    const int slices = OSC_PHASE_MS / 100;
    while (self->oscillating_) {
        if (!self->DriveForward(self->osc_speed_)) {
            self->oscillating_ = false;
            break;
        }
        for (int i = 0; i < slices && self->oscillating_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        if (!self->oscillating_) {
            break;
        }
        if (!self->DriveBackward(self->osc_speed_)) {
            self->oscillating_ = false;
            break;
        }
        for (int i = 0; i < slices && self->oscillating_; ++i) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    self->osc_task_ = nullptr;
    vTaskDelete(nullptr);
}

bool EyeMotorDevice::SetSpeed(uint8_t speed_percent) {
    auto& pca = Pca9685::GetInstance();
    if (!pca.IsReady()) {
        ESP_LOGE(TAG, "PCA9685 not ready");
        return false;
    }

    uint32_t duty = (static_cast<uint32_t>(speed_percent) * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    if (state_ == EYE_MOTOR_STATE_STOPPED && !oscillating_) {
        ESP_LOGW(TAG, "电机已停止，无法设置速度");
        return false;
    }

    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));
    current_duty_ = duty;
    osc_speed_ = speed_percent;

    ESP_LOGI(TAG, "电机速度调整: %d%%", speed_percent);
    return true;
}

bool EyeMotorDevice::Stop() {
    CancelOscillate();

    if (state_ == EYE_MOTOR_STATE_STOPPED) {
        return true;
    }

    auto& pca = Pca9685::GetInstance();
    if (pca.IsReady()) {
        pca.SetDigital(CH_AIN1, false);
        pca.SetDigital(CH_AIN2, false);
        pca.SetDuty(CH_PWM, 0);
    }

    current_duty_ = 0;
    state_ = EYE_MOTOR_STATE_STOPPED;
    ESP_LOGI(TAG, "电机已停止");
    return true;
}

EyeMotorDevice& EyeMotorDevice::GetInstance() {
    static EyeMotorDevice instance;
    return instance;
}
