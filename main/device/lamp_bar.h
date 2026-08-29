#pragma once

#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_MOSS_OV2640

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

class LampBarDevice {
private:
    static constexpr int LED_COUNT = 5;
    static constexpr uint8_t CH_FLOW_BASE = 8;
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint8_t kPwmCeilingPercent = 40;
    static constexpr uint8_t kDefaultBrightnessPercent = 20;

    bool power_;
    bool flowing_;
    uint8_t brightness_;
    TaskHandle_t flow_task_;

    static uint8_t ClampBrightness(int percent);
    uint16_t OnDuty() const;
    void ApplyPattern(uint8_t mask5);
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

    bool SetBrightness(int percent);
    uint8_t GetBrightness() const { return brightness_; }

    bool ResetDriver();
    bool ForceRestart();

    static LampBarDevice& GetInstance();
};

#else

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
    void WaitFlowTaskExit(int max_ms = 2000);
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

#endif
