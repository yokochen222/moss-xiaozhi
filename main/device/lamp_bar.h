#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

class LampBarDevice {
private:
    static constexpr int LED_COUNT = 5;
    static constexpr uint8_t CH_FLOW_BASE = 8;  // PCA9685 ch 8..12
    static constexpr uint16_t MAX_DUTY = 4095;
    static constexpr uint8_t kPwmCeilingPercent = 40;
    static constexpr uint8_t kDefaultBrightnessPercent = 20;

    bool power_;
    bool flowing_;
    uint8_t brightness_;
    TaskHandle_t flow_task_;

    static uint8_t ClampBrightness(int percent);
    uint16_t OnDuty() const;
    void ApplyPattern(uint8_t mask5);  // bits0..4 → PCA ch 8..12
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
