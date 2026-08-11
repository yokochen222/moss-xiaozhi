#include "eye_motor.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "../mcp/utils/pca9685_driver.h"

#define TAG "EyeMotorDevice"

EyeMotorDevice::EyeMotorDevice() : state_(EYE_MOTOR_STATE_STOPPED), current_duty_(0) {}

EyeMotorDevice::~EyeMotorDevice() {
    Stop();
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool EyeMotorDevice::StartForward() { return StartForward(DEFAULT_SPEED_PERCENT); }

bool EyeMotorDevice::StartBackward() { return StartBackward(DEFAULT_SPEED_PERCENT); }

bool EyeMotorDevice::StartForward(uint8_t speed_percent) {
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

    // 如果正在反转，先停止
    if (state_ == EYE_MOTOR_STATE_BACKWARD) {
        pca.SetDigital(CH_AIN1, false);
        pca.SetDigital(CH_AIN2, false);
        pca.SetDuty(CH_PWM, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // TB6612FNG via PCA9685: AIN1=1, AIN2=0, PWMA=duty
    pca.SetDigital(CH_AIN1, true);
    pca.SetDigital(CH_AIN2, false);
    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_FORWARD;
    ESP_LOGI(TAG, "电机正转启动, 速度: %d%%, duty=%lu", speed_percent,
             static_cast<unsigned long>(duty));
    return true;
}

bool EyeMotorDevice::StartBackward(uint8_t speed_percent) {
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

    // 如果正在正转，先停止
    if (state_ == EYE_MOTOR_STATE_FORWARD) {
        pca.SetDigital(CH_AIN1, false);
        pca.SetDigital(CH_AIN2, false);
        pca.SetDuty(CH_PWM, 0);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // TB6612FNG via PCA9685: AIN1=0, AIN2=1, PWMA=duty
    pca.SetDigital(CH_AIN1, false);
    pca.SetDigital(CH_AIN2, true);
    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_BACKWARD;
    ESP_LOGI(TAG, "电机反转启动, 速度: %d%%, duty=%lu", speed_percent,
             static_cast<unsigned long>(duty));
    return true;
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

    if (state_ == EYE_MOTOR_STATE_STOPPED) {
        ESP_LOGW(TAG, "电机已停止，无法设置速度");
        return false;
    }

    pca.SetDuty(CH_PWM, static_cast<uint16_t>(duty));
    current_duty_ = duty;

    ESP_LOGI(TAG, "电机速度调整: %d%%", speed_percent);
    return true;
}

bool EyeMotorDevice::Stop() {
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
