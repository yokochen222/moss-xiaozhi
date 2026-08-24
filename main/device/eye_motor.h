#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

enum EyeMotorState { EYE_MOTOR_STATE_STOPPED, EYE_MOTOR_STATE_FORWARD, EYE_MOTOR_STATE_BACKWARD };

class EyeMotorDevice {
private:
    static constexpr uint8_t CH_AIN2 = 3;
    static constexpr uint8_t CH_AIN1 = 4;
    static constexpr uint8_t CH_PWM = 5;
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint8_t DEFAULT_SPEED_PERCENT = 100;
    static constexpr int OSC_PHASE_MS = 8000;

    EyeMotorState state_;
    uint32_t current_duty_;
    volatile bool oscillating_;
    uint8_t osc_speed_;
    TaskHandle_t osc_task_;

    void CancelOscillate();
    bool DriveForward(uint8_t speed_percent);
    bool DriveBackward(uint8_t speed_percent);
    static void OscillateTask(void* arg);

public:
    EyeMotorDevice();
    ~EyeMotorDevice();

    EyeMotorDevice(const EyeMotorDevice&) = delete;
    EyeMotorDevice& operator=(const EyeMotorDevice&) = delete;

    bool StartForward();
    bool StartBackward();
    bool StartOscillate();
    bool Stop();

    bool StartForward(uint8_t speed_percent);
    bool StartBackward(uint8_t speed_percent);
    bool StartOscillate(uint8_t speed_percent);
    bool SetSpeed(uint8_t speed_percent);

    EyeMotorState GetState() const { return state_; }
    bool IsRunning() const { return state_ != EYE_MOTOR_STATE_STOPPED || oscillating_; }
    bool IsOscillating() const { return oscillating_; }
    uint32_t GetCurrentDuty() const { return current_duty_; }
    uint8_t GetCurrentSpeedPercent() const {
        return static_cast<uint8_t>((current_duty_ * 100) / MAX_DUTY);
    }

    static EyeMotorDevice& GetInstance();
};
