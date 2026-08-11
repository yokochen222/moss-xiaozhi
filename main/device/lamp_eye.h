#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <cstdint>

class LampEyeDevice {
private:
    static constexpr uint8_t CH = 6;
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint32_t STOP_NOTIFICATION = 0x01;

    bool power_;
    bool breathing_;
    bool pause_;
    TaskHandle_t breathing_task_handle_;
    SemaphoreHandle_t pwm_mutex_;

    void SetDuty(int duty);
    static void BreathingTask(void* arg);

public:
    LampEyeDevice();
    ~LampEyeDevice();

    // 禁用拷贝构造和赋值
    LampEyeDevice(const LampEyeDevice&) = delete;
    LampEyeDevice& operator=(const LampEyeDevice&) = delete;

    // 灯光控制
    bool TurnOn();
    bool TurnOff();
    bool StartBreathing();
    bool PauseBreathing();
    bool ResumeBreathing();
    bool StopBreathing();

    // 状态查询
    bool IsPowered() const { return power_; }
    bool IsBreathing() const { return breathing_; }
    bool IsPaused() const { return pause_; }

    // 获取单例实例
    static LampEyeDevice& GetInstance();
};
