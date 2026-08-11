#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>

class LampBarDevice {
private:
    static constexpr int LED_COUNT = 5;
    static constexpr uint8_t CH_FLOW_BASE = 8;  // PCA9685 ch 8..12

    bool power_;
    bool flowing_;
    TaskHandle_t flow_task_;

    void ApplyPattern(uint8_t mask5);  // bits0..4 → PCA ch 8..12
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
