#pragma once

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>

#include "config.h"

enum EyeMotorState { EYE_MOTOR_STATE_STOPPED, EYE_MOTOR_STATE_FORWARD, EYE_MOTOR_STATE_BACKWARD };

class EyeMotorDevice {
private:
    static constexpr gpio_num_t IN1_PIN = MOSS_EYE_MOTOR_IN1_PIN;
    static constexpr gpio_num_t IN2_PIN = MOSS_EYE_MOTOR_IN2_PIN;
    static constexpr gpio_num_t PWM_PIN = MOSS_EYE_MOTOR_PWM_PIN;

    // TIMER_0/CHANNEL_0 = backlight; TIMER_2/CHANNEL_2 = lamp eye.
    static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_1;
    static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_1;
    static constexpr ledc_timer_bit_t LEDC_DUTY_RES = LEDC_TIMER_8_BIT;
    static constexpr uint32_t LEDC_FREQUENCY = 1000;

    static constexpr uint32_t MAX_DUTY = (1 << LEDC_DUTY_RES) - 1;

    EyeMotorState state_;
    uint32_t current_duty_;

    void InitializeGpio();

public:
    static constexpr uint8_t DEFAULT_SPEED_PERCENT = 40;

    EyeMotorDevice();
    ~EyeMotorDevice();

    EyeMotorDevice(const EyeMotorDevice&) = delete;
    EyeMotorDevice& operator=(const EyeMotorDevice&) = delete;

    bool StartForward();
    bool StartBackward();
    bool Stop();

    bool StartForward(uint8_t speed_percent);
    bool StartBackward(uint8_t speed_percent);
    bool SetSpeed(uint8_t speed_percent);

    EyeMotorState GetState() const { return state_; }
    bool IsRunning() const { return state_ != EYE_MOTOR_STATE_STOPPED; }
    uint32_t GetCurrentDuty() const { return current_duty_; }
    uint8_t GetCurrentSpeedPercent() const {
        return static_cast<uint8_t>((current_duty_ * 100) / MAX_DUTY);
    }

    static EyeMotorDevice& GetInstance();
};
