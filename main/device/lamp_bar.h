#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class ShiftRegister74HC595;

class LampBarDevice {
private:
    static constexpr int LED_COUNT = 5;
    static constexpr gpio_num_t SER_PIN = GPIO_NUM_3;   // 数据引脚
    static constexpr gpio_num_t RCK_PIN = GPIO_NUM_4;   // 锁存引脚
    static constexpr gpio_num_t SCK_PIN = GPIO_NUM_5;   // 时钟引脚
    
    bool power_;
    bool flowing_;
    TaskHandle_t flow_task_;
    ShiftRegister74HC595* shift_register_;
    
    void InitializeShiftRegister();
    static void FlowTask(void* arg);

public:
    LampBarDevice();
    ~LampBarDevice();
    
    // 禁用拷贝构造和赋值
    LampBarDevice(const LampBarDevice&) = delete;
    LampBarDevice& operator=(const LampBarDevice&) = delete;
    
    // 流水灯控制
    bool StartFlow();
    bool StopFlow();
    bool IsFlowing() const { return flowing_; }
    bool IsPowered() const { return power_; }
    
    // 硬件控制
    bool ResetDriver();
    bool ForceRestart();
    
    // 获取单例实例
    static LampBarDevice& GetInstance();
};
