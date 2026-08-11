#pragma once

#include <driver/gpio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"

class ShiftRegister74HC595;

class LampBarDevice {
private:
    static constexpr int LED_COUNT = 5;
    static constexpr gpio_num_t SER_PIN = MOSS_LAMP_74HC595_SER_PIN;
    static constexpr gpio_num_t RCK_PIN = MOSS_LAMP_74HC595_RCK_PIN;
    static constexpr gpio_num_t SCK_PIN = MOSS_LAMP_74HC595_SCK_PIN;

    bool power_;
    bool flowing_;
    TaskHandle_t flow_task_;
    ShiftRegister74HC595* shift_register_;

    void InitializeShiftRegister();
    static void FlowTask(void* arg);

public:
    LampBarDevice();
    ~LampBarDevice();

    LampBarDevice(const LampBarDevice&) = delete;
    LampBarDevice& operator=(const LampBarDevice&) = delete;

    bool StartFlow();
    bool StopFlow();
    bool IsFlowing() const { return flowing_; }
    bool IsPowered() const { return power_; }

    bool ResetDriver();
    bool ForceRestart();

    static LampBarDevice& GetInstance();
};
