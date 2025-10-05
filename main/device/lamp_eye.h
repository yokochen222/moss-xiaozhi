#pragma once

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

class LampEyeDevice {
private:
#ifdef CONFIG_IDF_TARGET_ESP32
    static constexpr gpio_num_t GPIO_NUM = GPIO_NUM_15;
#else
    static constexpr gpio_num_t GPIO_NUM = GPIO_NUM_15;
#endif
    
    static constexpr ledc_timer_t LEDC_TIMER = LEDC_TIMER_0;
    static constexpr ledc_mode_t LEDC_MODE = LEDC_LOW_SPEED_MODE;
    static constexpr ledc_channel_t LEDC_CHANNEL = LEDC_CHANNEL_0;
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
