#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include "../mcp/utils/74hc595_driver.h"
#include "config.h"

enum class GimbalDir { Up, Down, Left, Right, UpLeft, UpRight, DownLeft, DownRight };

enum class StepMode { Half, Full };

class StepperGimbalDevice {
public:
    StepperGimbalDevice();
    ~StepperGimbalDevice();

    StepperGimbalDevice(const StepperGimbalDevice&) = delete;
    StepperGimbalDevice& operator=(const StepperGimbalDevice&) = delete;

    // 24BYJ-48 / ULN2003：半步默认；约 2ms/步（需 CONFIG_FREERTOS_HZ>=1000，否则 vTaskDelay 粒度约 10ms）。
    // 步进间隔只用 vTaskDelay，避免 busy-wait 饿死 IDLE/AFE。
    // 半步约 4096 步/转（输出轴）。
    bool Move(GimbalDir dir, uint16_t steps, StepMode mode = StepMode::Half, uint16_t delay_ms = 2);
    // 双轴同时：+H=右 -H=左，+V=上 -V=下；两轴非零时同拍推进，短轴结束后长轴继续。
    bool MoveAxes(int16_t h_steps, int16_t v_steps, StepMode mode = StepMode::Half,
                  uint16_t delay_ms = 2);

    // Continuous velocity follow (face track / PTZ): dirs are -1/0/+1; delay_ms is step period.
    // Updates are live — no stop/restart between frames. Both dirs 0 → coils off, task stays warm.
    bool SetFollowRates(int8_t h_dir, int8_t v_dir, uint16_t delay_ms = 4,
                        StepMode mode = StepMode::Half);

    // Hold a raw 595 byte for hold_ms (for DMM probing). Coils off afterwards; Stop() cancels
    // early.
    bool HoldPattern(uint8_t pattern, uint16_t hold_ms = 2000);
    bool Stop();
    bool IsMoving() const { return moving_; }

    static StepperGimbalDevice& GetInstance();

private:
    static constexpr gpio_num_t SER_PIN = HC595_SER_PIN;
    static constexpr gpio_num_t RCK_PIN = HC595_RCK_PIN;
    static constexpr gpio_num_t SCK_PIN = HC595_SCK_PIN;
    static constexpr uint16_t MIN_STEPS = 1;
    static constexpr uint16_t MAX_STEPS = 4096;  // ~1 rev half-step on geared BYJ48
    static constexpr uint16_t MIN_DELAY_MS = 1;
    static constexpr uint16_t MAX_DELAY_MS = 50;
    static constexpr uint16_t DEFAULT_DELAY_MS = 2;

    // Created + Initialize()'d on first Move/Stop（板级构造也会尽早 Stop 清零）。
    std::unique_ptr<ShiftRegister74HC595> shift_register_;
    std::mutex mutex_;
    TaskHandle_t task_handle_;
    volatile bool moving_;
    volatile bool holding_;
    volatile bool stop_requested_;
    volatile bool follow_mode_;
    volatile int8_t follow_h_dir_;
    volatile int8_t follow_v_dir_;
    volatile uint16_t follow_delay_ms_;

    StepMode mode_;
    uint16_t delay_ms_;
    int16_t h_steps_;  // signed remaining target at start
    int16_t v_steps_;
    int h_idx_;
    int v_idx_;

    bool EnsureInitialized();
    // 关断全部线圈（595 写 0）；未初始化时为 no-op。调用方须已持有 mutex_（MoveTask
    // 收尾除外见实现）。
    void AllCoilsOffLocked();
    uint16_t ClampDelay(uint16_t delay_ms) const;
    bool StartMoveTask(int16_t h_steps, int16_t v_steps, StepMode mode, uint16_t delay_ms);
    bool EnsureFollowTask(StepMode mode);
    bool WaitMoveTaskExit(int max_wait_slices = 100);

    static void DirDeltas(GimbalDir dir, int& h_delta, int& v_delta);
    static void MoveTask(void* arg);
    static void FollowTask(void* arg);
};
