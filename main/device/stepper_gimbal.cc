#include "stepper_gimbal.h"
#include "stepper_phase.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cstdlib>
#include <mutex>

#define TAG "StepperGimbal"

void StepperGimbalDevice::DirDeltas(GimbalDir dir, int& h_delta, int& v_delta) {
    h_delta = 0;
    v_delta = 0;
    switch (dir) {
        case GimbalDir::Right:
            h_delta = 1;
            break;
        case GimbalDir::Left:
            h_delta = -1;
            break;
        case GimbalDir::Up:
            v_delta = 1;
            break;
        case GimbalDir::Down:
            v_delta = -1;
            break;
        case GimbalDir::UpRight:
            h_delta = 1;
            v_delta = 1;
            break;
        case GimbalDir::UpLeft:
            h_delta = -1;
            v_delta = 1;
            break;
        case GimbalDir::DownRight:
            h_delta = 1;
            v_delta = -1;
            break;
        case GimbalDir::DownLeft:
            h_delta = -1;
            v_delta = -1;
            break;
    }
}

StepperGimbalDevice::StepperGimbalDevice()
    : shift_register_(nullptr),
      task_handle_(nullptr),
      moving_(false),
      holding_(false),
      stop_requested_(false),
      mode_(StepMode::Half),
      delay_ms_(DEFAULT_DELAY_MS),
      h_steps_(0),
      v_steps_(0),
      h_idx_(0),
      v_idx_(0) {
    // Pure phase-table check only; no GPIO configuration here.
    stepper_phase::SelfTest();
}

StepperGimbalDevice::~StepperGimbalDevice() { Stop(); }

bool StepperGimbalDevice::EnsureInitialized() {
    if (shift_register_) {
        return true;
    }
    shift_register_ = std::make_unique<ShiftRegister74HC595>(SER_PIN, RCK_PIN, SCK_PIN);
    if (!shift_register_) {
        ESP_LOGE(TAG, "Failed to allocate shift register");
        return false;
    }
    shift_register_->Initialize();
    AllCoilsOffLocked();
    ESP_LOGI(TAG, "74HC595 initialized on first Move/Stop (SER=%d RCK=%d SCK=%d)",
             static_cast<int>(SER_PIN), static_cast<int>(RCK_PIN), static_cast<int>(SCK_PIN));
    return true;
}

void StepperGimbalDevice::AllCoilsOffLocked() {
    if (!shift_register_) {
        return;
    }
    // 写两次：强制删除步进任务时可能打断半截移位，二次全 0 保证锁存干净。
    shift_register_->SetOutputs(0);
    shift_register_->SetOutputs(0);
}

uint16_t StepperGimbalDevice::ClampDelay(uint16_t delay_ms) const {
    if (delay_ms == 0) {
        return DEFAULT_DELAY_MS;
    }
    if (delay_ms < MIN_DELAY_MS) {
        return MIN_DELAY_MS;
    }
    if (delay_ms > MAX_DELAY_MS) {
        return MAX_DELAY_MS;
    }
    return delay_ms;
}

bool StepperGimbalDevice::WaitMoveTaskExit(int max_wait_slices) {
    int wait_count = 0;
    while (task_handle_ != nullptr && wait_count < max_wait_slices) {
        vTaskDelay(pdMS_TO_TICKS(10));
        wait_count++;
    }
    if (task_handle_ != nullptr) {
        ESP_LOGW(TAG, "Force-deleting move task");
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

bool StepperGimbalDevice::StartMoveTask(int16_t h_steps, int16_t v_steps, StepMode mode,
                                        uint16_t delay_ms) {
    delay_ms = ClampDelay(delay_ms);

    // Stop any in-flight move / hold without holding the mutex across the wait.
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureInitialized()) {
            return false;
        }
        holding_ = false;
        if (moving_ || task_handle_ != nullptr) {
            stop_requested_ = true;
        }
    }

    WaitMoveTaskExit();

    std::lock_guard<std::mutex> lock(mutex_);
    // 上一任务被强杀或自然退出后，先关断全部相，避免保持电流发烫。
    AllCoilsOffLocked();
    moving_ = false;
    holding_ = false;
    stop_requested_ = false;

    mode_ = mode;
    delay_ms_ = delay_ms;
    h_steps_ = h_steps;
    v_steps_ = v_steps;
    moving_ = true;

    // 优先级低于 audio_communication(3)/audio_input(8)；钉在 core1，避开 core0 上的音频采集
    BaseType_t result =
        xTaskCreatePinnedToCore(MoveTask, "GimbalStep", 3072, this, 2, &task_handle_, 1);
    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create move task");
        moving_ = false;
        task_handle_ = nullptr;
        AllCoilsOffLocked();
        return false;
    }

    ESP_LOGI(TAG, "Move start: h=%d v=%d mode=%s delay=%ums (tick_hz=%u)",
             static_cast<int>(h_steps), static_cast<int>(v_steps),
             mode == StepMode::Full ? "full" : "half", static_cast<unsigned>(delay_ms),
             static_cast<unsigned>(configTICK_RATE_HZ));
    if (configTICK_RATE_HZ < 1000) {
        ESP_LOGW(TAG,
                 "FREERTOS_HZ=%u < 1000：本步用 esp_timer 精确定时（避免被抬到 ~%ums/tick）；"
                 "建议 scripts/build.py 应用 CONFIG_FREERTOS_HZ=1000 后改回纯 vTaskDelay",
                 static_cast<unsigned>(configTICK_RATE_HZ),
                 static_cast<unsigned>(1000 / configTICK_RATE_HZ));
    }
    return true;
}

bool StepperGimbalDevice::Move(GimbalDir dir, uint16_t steps, StepMode mode, uint16_t delay_ms) {
    if (steps < MIN_STEPS) {
        steps = MIN_STEPS;
    } else if (steps > MAX_STEPS) {
        steps = MAX_STEPS;
    }

    int h_delta = 0;
    int v_delta = 0;
    DirDeltas(dir, h_delta, v_delta);
    return MoveAxes(static_cast<int16_t>(h_delta * static_cast<int>(steps)),
                    static_cast<int16_t>(v_delta * static_cast<int>(steps)), mode, delay_ms);
}

bool StepperGimbalDevice::MoveAxes(int16_t h_steps, int16_t v_steps, StepMode mode,
                                   uint16_t delay_ms) {
    auto clamp_axis = [](int16_t s) -> int16_t {
        if (s > static_cast<int16_t>(MAX_STEPS)) {
            return static_cast<int16_t>(MAX_STEPS);
        }
        if (s < -static_cast<int16_t>(MAX_STEPS)) {
            return static_cast<int16_t>(-static_cast<int>(MAX_STEPS));
        }
        return s;
    };
    h_steps = clamp_axis(h_steps);
    v_steps = clamp_axis(v_steps);

    if (h_steps == 0 && v_steps == 0) {
        ESP_LOGW(TAG, "MoveAxes ignored: both axes zero — coils off");
        Stop();
        return false;
    }

    return StartMoveTask(h_steps, v_steps, mode, delay_ms);
}

bool StepperGimbalDevice::HoldPattern(uint8_t pattern, uint16_t hold_ms) {
    if (hold_ms < 100) {
        hold_ms = 100;
    } else if (hold_ms > 30000) {
        hold_ms = 30000;
    }

    Stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!EnsureInitialized()) {
            return false;
        }
        if (pattern == 0) {
            AllCoilsOffLocked();
            holding_ = false;
            ESP_LOGI(TAG, "HoldPattern 0x00 — coils already off");
            return true;
        }
        stop_requested_ = false;
        holding_ = true;
        shift_register_->SetOutputs(pattern);
        ESP_LOGI(TAG, "HoldPattern 0x%02X for %ums — probe now (Stop cancels; motor will heat)",
                 pattern, static_cast<unsigned>(hold_ms));
    }

    // 不持锁等待，便于 Stop() 立刻写 0 关断。
    uint32_t elapsed = 0;
    while (elapsed < hold_ms) {
        if (stop_requested_ || !holding_) {
            ESP_LOGI(TAG, "HoldPattern cancelled after %ums", static_cast<unsigned>(elapsed));
            break;
        }
        const uint32_t slice = (hold_ms - elapsed > 50) ? 50 : (hold_ms - elapsed);
        vTaskDelay(pdMS_TO_TICKS(slice));
        elapsed += slice;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        holding_ = false;
        AllCoilsOffLocked();
        ESP_LOGI(TAG, "HoldPattern done (coils off)");
    }
    return true;
}

bool StepperGimbalDevice::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // 即使从未 Move 过也初始化并清零，消除 595 上电随机输出导致的线圈常通。
        if (!EnsureInitialized()) {
            return false;
        }
        // 始终置位：打断 MoveTask 延时，也打断 HoldPattern 等待。
        stop_requested_ = true;
        // hold 期间立刻断电，不必等 HoldPattern 循环醒过来。
        if (holding_) {
            holding_ = false;
            AllCoilsOffLocked();
        }
        if (!moving_ && task_handle_ == nullptr) {
            AllCoilsOffLocked();
            stop_requested_ = false;
            ESP_LOGI(TAG, "Stopped (coils off)");
            return true;
        }
    }

    WaitMoveTaskExit();

    std::lock_guard<std::mutex> lock(mutex_);
    moving_ = false;
    holding_ = false;
    stop_requested_ = false;
    AllCoilsOffLocked();
    ESP_LOGI(TAG, "Stopped (coils off)");
    return true;
}

// HZ>=1000：纯 vTaskDelay（干净、IDLE 可喂狗）。
// HZ=100：vTaskDelay 最少约 10ms，改用 esp_timer 精确定时；不足 1 tick 的部分 busy-wait，
// 并在 MoveTask 中周期性 vTaskDelay(1) 让 CPU1 IDLE 喂狗。
static bool StepDelayMsInterruptible(uint16_t delay_ms, volatile bool* stop_requested) {
    if (configTICK_RATE_HZ >= 1000) {
        TickType_t remaining = pdMS_TO_TICKS(delay_ms);
        if (remaining < 1) {
            remaining = 1;
        }
        while (remaining > 0) {
            if (stop_requested && *stop_requested) {
                return true;
            }
            TickType_t slice = remaining > pdMS_TO_TICKS(1) ? pdMS_TO_TICKS(1) : remaining;
            if (slice < 1) {
                slice = 1;
            }
            vTaskDelay(slice);
            if (remaining > slice) {
                remaining = static_cast<TickType_t>(remaining - slice);
            } else {
                remaining = 0;
            }
        }
        return stop_requested && *stop_requested;
    }

    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(delay_ms) * 1000;
    const int64_t tick_us = 1000000LL / static_cast<int64_t>(configTICK_RATE_HZ);
    while (true) {
        if (stop_requested && *stop_requested) {
            return true;
        }
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            return false;
        }
        if (remaining >= tick_us) {
            vTaskDelay(1);
            continue;
        }
        while (esp_timer_get_time() < deadline) {
            if (stop_requested && *stop_requested) {
                return true;
            }
        }
        return false;
    }
}

void StepperGimbalDevice::MoveTask(void* arg) {
    auto* self = static_cast<StepperGimbalDevice*>(arg);

    const bool half = (self->mode_ == StepMode::Half);
    const uint16_t delay_ms = self->delay_ms_;

    int h_left = std::abs(static_cast<int>(self->h_steps_));
    int v_left = std::abs(static_cast<int>(self->v_steps_));
    const int h_dir = (self->h_steps_ > 0) ? 1 : ((self->h_steps_ < 0) ? -1 : 0);
    const int v_dir = (self->v_steps_ > 0) ? 1 : ((self->v_steps_ < 0) ? -1 : 0);

    ESP_LOGI(TAG, "MoveTask running: h=%d(%c) v=%d(%c) delay=%ums", h_left,
             h_dir > 0 ? '+' : (h_dir < 0 ? '-' : '0'), v_left,
             v_dir > 0 ? '+' : (v_dir < 0 ? '-' : '0'), static_cast<unsigned>(delay_ms));

    uint32_t step_i = 0;
    while (h_left > 0 || v_left > 0) {
        if (self->stop_requested_) {
            ESP_LOGI(TAG, "MoveTask stop requested at step %u (h_left=%d v_left=%d)",
                     static_cast<unsigned>(step_i), h_left, v_left);
            break;
        }

        const bool step_h = h_left > 0;
        const bool step_v = v_left > 0;

        if (step_h) {
            self->h_idx_ += h_dir;
            --h_left;
        }
        if (step_v) {
            self->v_idx_ += v_dir;
            --v_left;
        }

        uint8_t h_mask = 0;
        uint8_t v_mask = 0;
        if (step_h) {
            h_mask = half ? stepper_phase::HalfStepH(self->h_idx_)
                          : stepper_phase::FullStepH(self->h_idx_);
        }
        if (step_v) {
            v_mask = half ? stepper_phase::HalfStepV(self->v_idx_)
                          : stepper_phase::FullStepV(self->v_idx_);
        }

        const uint8_t out = static_cast<uint8_t>(h_mask | v_mask);
        {
            // 与 Stop/Hold 互斥：避免 stop 写 0 后又被本步覆盖成励磁。
            std::lock_guard<std::mutex> lock(self->mutex_);
            if (self->stop_requested_) {
                ESP_LOGI(TAG, "MoveTask stop before phase write at step %u",
                         static_cast<unsigned>(step_i));
                break;
            }
            self->shift_register_->SetOutputs(out);
        }
        if (step_i < 3 || (h_left == 0 && v_left == 0)) {
            ESP_LOGI(TAG, "step %u out=0x%02X (H=0x%02X V=0x%02X)", static_cast<unsigned>(step_i),
                     out, h_mask, v_mask);
        }

        const bool last_step = (h_left == 0 && v_left == 0);
        // 最后一步只保留最短励磁，随即关断，避免“已停转但仍保持 delay_ms”发烫。
        const uint16_t hold_ms = last_step ? 1 : delay_ms;
        if (StepDelayMsInterruptible(hold_ms, &self->stop_requested_)) {
            ESP_LOGI(TAG, "MoveTask stop during step delay at step %u",
                     static_cast<unsigned>(step_i));
            break;
        }
        ++step_i;
    }

    {
        std::lock_guard<std::mutex> lock(self->mutex_);
        self->AllCoilsOffLocked();
        self->moving_ = false;
        self->stop_requested_ = false;
        self->task_handle_ = nullptr;
    }
    ESP_LOGI(TAG, "MoveTask done (coils off)");
    vTaskDelete(nullptr);
}

StepperGimbalDevice& StepperGimbalDevice::GetInstance() {
    static StepperGimbalDevice instance;
    return instance;
}
