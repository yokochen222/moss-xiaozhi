#pragma once

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "config.h"

class LampEyeDevice {
private:
    // Avoid TIMER_0/CH0 (backlight) and TIMER_1/CH1 (eye motor).
    // Camera (when enabled) historically used TIMER_2/CH2.
    static constexpr gpio_num_t GPIO_NUM = MOSS_LAMP_EYE_PIN;
    static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_3;
    static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_3;
    static constexpr ledc_timer_bit_t LEDC_DUTY_RES = LEDC_TIMER_13_BIT;
    static constexpr uint32_t LEDC_FREQUENCY = 5000;
    static constexpr uint32_t STOP_NOTIFICATION = 0x01;

    gpio_num_t gpio_num_;
    bool power_;
    bool breathing_;
    bool pause_;
    TaskHandle_t breathing_task_handle_;
    SemaphoreHandle_t pwm_mutex_;

    void InitializeGpio();
    void SetDuty(int duty);
    void WaitBreathingTaskExit(int max_ms = 2000);
    static void BreathingTask(void* arg);

public:
    LampEyeDevice();
    ~LampEyeDevice();

    LampEyeDevice(const LampEyeDevice&) = delete;
    LampEyeDevice& operator=(const LampEyeDevice&) = delete;

    bool TurnOn();
    bool TurnOff();
    bool StartBreathing();
    bool PauseBreathing();
    bool ResumeBreathing();
    bool StopBreathing();

    bool IsPowered() const { return power_; }
    bool IsBreathing() const { return breathing_; }
    bool IsPaused() const { return pause_; }

    static LampEyeDevice& GetInstance();
};
