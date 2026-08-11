#pragma once

#include <freertos/FreeRTOS.h>
#include <cstdint>

enum EyeMotorState { EYE_MOTOR_STATE_STOPPED, EYE_MOTOR_STATE_FORWARD, EYE_MOTOR_STATE_BACKWARD };

class EyeMotorDevice {
private:
    static constexpr uint8_t CH_AIN2 = 3;
    static constexpr uint8_t CH_AIN1 = 4;
    static constexpr uint8_t CH_PWM = 5;
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint8_t DEFAULT_SPEED_PERCENT = 100;

    EyeMotorState state_;
    uint32_t current_duty_;

public:
    EyeMotorDevice();
    ~EyeMotorDevice();

    // 禁用拷贝构造和赋值
    EyeMotorDevice(const EyeMotorDevice&) = delete;
    EyeMotorDevice& operator=(const EyeMotorDevice&) = delete;

    // 电机控制 (默认全速)
    bool StartForward();
    bool StartBackward();
    bool Stop();

    // 电机控制 (指定速度 0-100%)
    bool StartForward(uint8_t speed_percent);
    bool StartBackward(uint8_t speed_percent);
    bool SetSpeed(uint8_t speed_percent);

    // 状态查询
    EyeMotorState GetState() const { return state_; }
    bool IsRunning() const { return state_ != EYE_MOTOR_STATE_STOPPED; }
    uint32_t GetCurrentDuty() const { return current_duty_; }
    uint8_t GetCurrentSpeedPercent() const {
        return static_cast<uint8_t>((current_duty_ * 100) / MAX_DUTY);
    }

    // 获取单例实例
    static EyeMotorDevice& GetInstance();
};
