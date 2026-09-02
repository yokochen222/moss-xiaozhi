#include "eye_motor.h"
#include <esp_log.h>

#define TAG "EyeMotorDevice"

EyeMotorDevice::EyeMotorDevice()
    : state_(EYE_MOTOR_STATE_STOPPED),
      current_duty_(0),
      oscillating_(false),
      osc_speed_(DEFAULT_SPEED_PERCENT),
      osc_task_(nullptr) {
    InitializeGpio();
}

EyeMotorDevice::~EyeMotorDevice() {
    Stop();
    vTaskDelay(pdMS_TO_TICKS(50));
}

void EyeMotorDevice::InitializeGpio() {
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << IN1_PIN) | (1ULL << IN2_PIN);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io_conf);
    ESP_LOGI(TAG, "GPIO配置结果: %s, IN1=GPIO%d, IN2=GPIO%d", esp_err_to_name(err), IN1_PIN,
             IN2_PIN);

    // 初始化时确保方向引脚为低
    gpio_reset_pin(IN1_PIN);
    gpio_reset_pin(IN2_PIN);
    gpio_set_level(IN1_PIN, 0);
    gpio_set_level(IN2_PIN, 0);
    ESP_LOGI(TAG, "初始化GPIO电平 - IN1=%d, IN2=%d", gpio_get_level(IN1_PIN),
             gpio_get_level(IN2_PIN));

    // 配置LEDC定时器 (用于PWM速度控制)
    ledc_timer_config_t ledc_timer = {};
    ledc_timer.speed_mode = LEDC_MODE;
    ledc_timer.timer_num = LEDC_TIMER;
    ledc_timer.duty_resolution = LEDC_DUTY_RES;
    ledc_timer.freq_hz = LEDC_FREQUENCY;
    ledc_timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // 配置PWM引脚
    ledc_channel_config_t ledc_channel = {};
    ledc_channel.gpio_num = PWM_PIN;
    ledc_channel.speed_mode = LEDC_MODE;
    ledc_channel.channel = LEDC_CHANNEL;
    ledc_channel.timer_sel = LEDC_TIMER;
    ledc_channel.duty = 0;
    ledc_channel.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

    ESP_LOGI(TAG, "EyeMotor TB6612FNG驱动初始化完成, PWM=GPIO9, IN1=GPIO10, IN2=GPIO11");
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
    uint32_t duty = (speed_percent * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    ESP_LOGI(TAG, "StartForward 前 - 当前IN1=%d, IN2=%d, state=%d", gpio_get_level(IN1_PIN),
             gpio_get_level(IN2_PIN), state_);

    if (state_ == EYE_MOTOR_STATE_FORWARD && current_duty_ == duty) {
        return true;
    }

    // 如果正在反转，先停止
    if (state_ == EYE_MOTOR_STATE_BACKWARD) {
        gpio_set_level(IN1_PIN, 0);
        gpio_set_level(IN2_PIN, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // TB6612FNG: IN1=1, IN2=0, PWM=速度
    // 使用 gpio_config 彻底重新配置引脚
    gpio_config_t out_conf = {};
    out_conf.pin_bit_mask = (1ULL << IN1_PIN) | (1ULL << IN2_PIN);
    out_conf.mode = GPIO_MODE_OUTPUT;
    out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_conf);

    gpio_set_level(IN1_PIN, 1);
    gpio_set_level(IN2_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(1));  // 短暂延时确保稳定
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);

    // 调试：读取并打印实际 GPIO 电平
    ESP_LOGI(TAG, "GPIO实际状态 - IN1=%d, IN2=%d, PWM_DUTY=%d", gpio_get_level(IN1_PIN),
             gpio_get_level(IN2_PIN), duty);

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_FORWARD;
    ESP_LOGI(TAG, "电机正转启动, 速度: %d%%", speed_percent);
    return true;
}

bool EyeMotorDevice::DriveBackward(uint8_t speed_percent) {
    uint32_t duty = (speed_percent * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    if (state_ == EYE_MOTOR_STATE_BACKWARD && current_duty_ == duty) {
        return true;
    }

    // 如果正在正转，先停止
    if (state_ == EYE_MOTOR_STATE_FORWARD) {
        gpio_set_level(IN1_PIN, 0);
        gpio_set_level(IN2_PIN, 0);
        ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
        ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // TB6612FNG: IN1=0, IN2=1, PWM=速度
    // 使用 gpio_config 彻底重新配置引脚
    gpio_config_t out_conf = {};
    out_conf.pin_bit_mask = (1ULL << IN1_PIN) | (1ULL << IN2_PIN);
    out_conf.mode = GPIO_MODE_OUTPUT;
    out_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    out_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&out_conf);

    gpio_set_level(IN1_PIN, 0);
    gpio_set_level(IN2_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(1));  // 短暂延时确保稳定
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);

    // 调试：读取并打印实际 GPIO 电平
    ESP_LOGI(TAG, "GPIO实际状态 - IN1=%d, IN2=%d, PWM_DUTY=%d", gpio_get_level(IN1_PIN),
             gpio_get_level(IN2_PIN), duty);

    current_duty_ = duty;
    state_ = EYE_MOTOR_STATE_BACKWARD;
    ESP_LOGI(TAG, "电机反转启动, 速度: %d%%", speed_percent);
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
    uint32_t duty = (speed_percent * MAX_DUTY) / 100;
    if (duty > MAX_DUTY) {
        duty = MAX_DUTY;
    }

    if (state_ == EYE_MOTOR_STATE_STOPPED && !oscillating_) {
        ESP_LOGW(TAG, "电机已停止，无法设置速度");
        return false;
    }

    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
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

    // 停止PWM输出
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, 0);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);

    // 方向引脚拉低
    gpio_set_level(IN1_PIN, 0);
    gpio_set_level(IN2_PIN, 0);

    current_duty_ = 0;
    state_ = EYE_MOTOR_STATE_STOPPED;
    ESP_LOGI(TAG, "电机已停止");
    return true;
}

EyeMotorDevice& EyeMotorDevice::GetInstance() {
    static EyeMotorDevice instance;
    return instance;
}
