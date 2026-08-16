#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "esp_camera.h"

// Board-owned camera bridge for face tracking (RGB565). Implemented by moss-desktop
// OnDemandEsp32Camera. FaceTracker never owns the sensor.
class FaceTrackCamera {
public:
    virtual ~FaceTrackCamera() = default;

    // Switch sensor to RGB565 (board chooses size, e.g. HVGA) in PSRAM and keep streaming.
    virtual bool AcquireTracking() = 0;
    // Deinit DVP; return to idle (JPEG on-demand path remains available).
    virtual void ReleaseTracking() = 0;
    virtual bool IsTrackingAcquired() const = 0;

    virtual camera_fb_t* GrabTrackingFrame() = 0;
    virtual void ReturnTrackingFrame(camera_fb_t* fb) = 0;
};

struct FaceTrackerStatus {
    bool running = false;
    bool paused = false;
    bool has_face = false;
    int err_x = 0;
    int err_y = 0;
    int face_w = 0;
    int face_h = 0;
    int box_x1 = 0;
    int box_y1 = 0;
    int box_x2 = 0;
    int box_y2 = 0;
    int frame_w = 320;
    int frame_h = 240;
    bool gimbal_moving = false;
    uint32_t detect_ms = 0;
    uint32_t lost_frames = 0;
    uint32_t faces = 0;
    // Optional RGB565 preview for 160x80 HUD (camera byte order, valid only during StatusSink).
    const uint16_t* preview_rgb565 = nullptr;
    int preview_w = 0;
    int preview_h = 0;
};

class FaceTracker {
public:
    using StatusSink = std::function<void(const FaceTrackerStatus&)>;
    using UiHook = std::function<void()>;

    static FaceTracker& GetInstance();

    FaceTracker(const FaceTracker&) = delete;
    FaceTracker& operator=(const FaceTracker&) = delete;

    void SetCamera(FaceTrackCamera* camera);
    void SetStatusSink(StatusSink sink);
    void SetUiHooks(UiHook on_start, UiHook on_stop);

    // Pause tracking loop + release RGB camera so JPEG take_photo can run.
    // Returns true if tracking was running (caller should Resume later).
    bool PauseForExternalCameraUse();
    void ResumeAfterExternalCameraUse();

    bool Start();
    bool Stop();
    bool IsRunning() const;
    FaceTrackerStatus GetStatus() const;
    std::string GetStatusString() const;

private:
    FaceTracker() = default;

    static void TaskEntry(void* arg);
    void TaskLoop();
    bool EnsureDetector();
    void ReleaseDetector();
    void ApplyControl(int err_x, int err_y, int frame_w);
    void ApplyIdleFollow();
    bool EnsurePreviewBuffer(int w, int h);
    void ReleasePreviewBuffer();
    void FillPreviewFromFrame(const camera_fb_t* fb);

    mutable std::mutex mutex_;
    FaceTrackCamera* camera_ = nullptr;
    void* detector_ = nullptr;  // HumanFaceDetect*
    TaskHandle_t task_ = nullptr;
    bool stop_requested_ = false;
    bool running_ = false;
    bool paused_ = false;
    bool resume_after_photo_ = false;
    bool frame_in_flight_ = false;
    FaceTrackerStatus status_{};
    StatusSink status_sink_;
    UiHook on_start_ui_;
    UiHook on_stop_ui_;
    uint16_t* preview_buf_ = nullptr;
    int preview_w_ = 0;
    int preview_h_ = 0;

    // Smoothed error for continuous velocity follow (QVGA-normalized px).
    float filt_err_x_ = 0.f;
    float filt_err_y_ = 0.f;
    int8_t last_h_dir_ = 0;
    int8_t last_v_dir_ = 0;
    uint16_t last_follow_delay_ms_ = 0;

    // Control: continuous rate follow (not burst MoveAxes).
    // Detect latency ~0.6s → ease near center; keep far-field snappy.
    static constexpr int kRefFrameW = 320;
    static constexpr float kFiltAlpha = 0.30f;
    static constexpr int kDeadzonePx = 15;
    static constexpr float kErrForMaxSpeed = 60.f;
    static constexpr uint16_t kMinStepDelayMs = 2;
    static constexpr uint16_t kMaxStepDelayMs = 7;
    static constexpr int kLoopPeriodMs = 50;
    static constexpr int kPreviewW = 160;
    static constexpr int kPreviewH = 80;
};
