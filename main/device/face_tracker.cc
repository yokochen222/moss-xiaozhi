#include "face_tracker.h"

#include "device/stepper_gimbal.h"
#include "human_face_detect.hpp"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cmath>

#define TAG "FaceTracker"

FaceTracker& FaceTracker::GetInstance() {
    static FaceTracker instance;
    return instance;
}

void FaceTracker::SetCamera(FaceTrackCamera* camera) {
    std::lock_guard<std::mutex> lock(mutex_);
    camera_ = camera;
}

void FaceTracker::SetStatusSink(StatusSink sink) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_sink_ = std::move(sink);
}

void FaceTracker::SetUiHooks(UiHook on_start, UiHook on_stop) {
    std::lock_guard<std::mutex> lock(mutex_);
    on_start_ui_ = std::move(on_start);
    on_stop_ui_ = std::move(on_stop);
}

bool FaceTracker::PauseForExternalCameraUse() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            resume_after_photo_ = false;
            return false;
        }
        paused_ = true;
        resume_after_photo_ = true;
        status_.paused = true;
        filt_err_x_ = 0.f;
        filt_err_y_ = 0.f;
        last_h_dir_ = 0;
        last_v_dir_ = 0;
        last_follow_delay_ms_ = 0;
    }
    StepperGimbalDevice::GetInstance().SetFollowRates(0, 0, kMaxStepDelayMs);
    for (int i = 0; i < 40; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!frame_in_flight_) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_ && camera_->IsTrackingAcquired()) {
            camera_->ReleaseTracking();
        }
    }
    ESP_LOGI(TAG, "Paused for external camera use");
    return true;
}

void FaceTracker::ResumeAfterExternalCameraUse() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!resume_after_photo_ || !running_) {
        resume_after_photo_ = false;
        return;
    }
    resume_after_photo_ = false;
    if (camera_ && !camera_->AcquireTracking()) {
        ESP_LOGE(TAG, "Failed to re-acquire camera after photo; stopping tracker");
        stop_requested_ = true;
        paused_ = false;
        status_.paused = false;
        return;
    }
    paused_ = false;
    status_.paused = false;
    ESP_LOGI(TAG, "Resumed after external camera use");
}

bool FaceTracker::EnsureDetector() {
    if (detector_) {
        return true;
    }
    const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    ESP_LOGI(TAG, "Creating HumanFaceDetect (MSR+MNP) free_psram=%u free_int=%u",
             (unsigned)free_psram, (unsigned)free_int);
    auto* det = new (std::nothrow) HumanFaceDetect(HumanFaceDetect::MSRMNP_S8_V1, true);
    if (!det) {
        ESP_LOGE(TAG, "Failed to allocate HumanFaceDetect");
        return false;
    }
    // Looser than default 0.5: distant / partial faces on OV2640 were often missed.
    det->set_score_thr(0.25f, 0);
    det->set_score_thr(0.25f, 1);
    detector_ = det;
    return true;
}

void FaceTracker::ReleaseDetector() {
    if (!detector_) {
        return;
    }
    delete static_cast<HumanFaceDetect*>(detector_);
    detector_ = nullptr;
}

bool FaceTracker::EnsurePreviewBuffer(int w, int h) {
    if (preview_buf_ && preview_w_ == w && preview_h_ == h) {
        return true;
    }
    ReleasePreviewBuffer();
    const size_t bytes = static_cast<size_t>(w) * static_cast<size_t>(h) * sizeof(uint16_t);
    preview_buf_ = static_cast<uint16_t*>(
        heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!preview_buf_) {
        preview_buf_ = static_cast<uint16_t*>(malloc(bytes));
    }
    if (!preview_buf_) {
        ESP_LOGE(TAG, "Failed to alloc preview %dx%d", w, h);
        return false;
    }
    preview_w_ = w;
    preview_h_ = h;
    return true;
}

void FaceTracker::ReleasePreviewBuffer() {
    if (preview_buf_) {
        heap_caps_free(preview_buf_);
        preview_buf_ = nullptr;
    }
    preview_w_ = 0;
    preview_h_ = 0;
}

void FaceTracker::FillPreviewFromFrame(const camera_fb_t* fb) {
    if (!fb || !fb->buf || fb->width <= 0 || fb->height <= 0) {
        return;
    }
    if (!EnsurePreviewBuffer(kPreviewW, kPreviewH)) {
        return;
    }
    const auto* src = reinterpret_cast<const uint16_t*>(fb->buf);
    for (int y = 0; y < preview_h_; ++y) {
        const int sy = y * fb->height / preview_h_;
        for (int x = 0; x < preview_w_; ++x) {
            const int sx = x * fb->width / preview_w_;
            preview_buf_[y * preview_w_ + x] = src[sy * fb->width + sx];
        }
    }
}

void FaceTracker::ApplyControl(int err_x, int err_y, int frame_w) {
    // Normalize to QVGA-equivalent pixels so thresholds stay stable across resolutions.
    const float scale =
        static_cast<float>(kRefFrameW) / static_cast<float>(std::max(frame_w, 1));
    const float nerr_x = static_cast<float>(err_x) * scale;
    const float nerr_y = static_cast<float>(err_y) * scale;

    filt_err_x_ = kFiltAlpha * nerr_x + (1.f - kFiltAlpha) * filt_err_x_;
    filt_err_y_ = kFiltAlpha * nerr_y + (1.f - kFiltAlpha) * filt_err_y_;

    int8_t h_dir = 0;
    int8_t v_dir = 0;
    if (std::fabs(filt_err_x_) >= static_cast<float>(kDeadzonePx)) {
        h_dir = (filt_err_x_ > 0.f) ? 1 : -1;
    }
    if (std::fabs(filt_err_y_) >= static_cast<float>(kDeadzonePx)) {
        // Face below center → tilt down (−V).
        v_dir = (filt_err_y_ > 0.f) ? -1 : 1;
    }

    const float mag = std::max(std::fabs(filt_err_x_), std::fabs(filt_err_y_));
    uint16_t delay_ms = kMaxStepDelayMs;
    if (h_dir != 0 || v_dir != 0) {
        float t = (mag - static_cast<float>(kDeadzonePx)) /
                  (kErrForMaxSpeed - static_cast<float>(kDeadzonePx));
        t = std::clamp(t, 0.f, 1.f);
        // Mild ease (t^1.5): faster mid-range than pure t^2, still brakes near center.
        t = t * std::sqrt(t);
        delay_ms = static_cast<uint16_t>(std::lround(
            static_cast<float>(kMaxStepDelayMs) -
            t * static_cast<float>(kMaxStepDelayMs - kMinStepDelayMs)));
        delay_ms = std::clamp(delay_ms, kMinStepDelayMs, kMaxStepDelayMs);
    }

    const bool changed =
        (h_dir != last_h_dir_) || (v_dir != last_v_dir_) || (delay_ms != last_follow_delay_ms_);
    last_h_dir_ = h_dir;
    last_v_dir_ = v_dir;
    last_follow_delay_ms_ = delay_ms;

    auto& gimbal = StepperGimbalDevice::GetInstance();
    if (!changed) {
        // Keep follow task alive if it was stopped externally.
        if (h_dir != 0 || v_dir != 0) {
            gimbal.SetFollowRates(h_dir, v_dir, delay_ms, StepMode::Half);
        }
        return;
    }

    if (h_dir == 0 && v_dir == 0) {
        ESP_LOGI(TAG, "Follow idle filt=(%.0f,%.0f)", filt_err_x_, filt_err_y_);
        gimbal.SetFollowRates(0, 0, kMaxStepDelayMs);
        return;
    }

    ESP_LOGI(TAG, "Follow h=%d v=%d delay=%ums filt=(%.0f,%.0f) err=(%d,%d)", (int)h_dir,
             (int)v_dir, (unsigned)delay_ms, filt_err_x_, filt_err_y_, err_x, err_y);
    gimbal.SetFollowRates(h_dir, v_dir, delay_ms, StepMode::Half);
}

void FaceTracker::ApplyIdleFollow() {
    filt_err_x_ *= (1.f - kFiltAlpha);
    filt_err_y_ *= (1.f - kFiltAlpha);
    if (std::fabs(filt_err_x_) < 1.f) {
        filt_err_x_ = 0.f;
    }
    if (std::fabs(filt_err_y_) < 1.f) {
        filt_err_y_ = 0.f;
    }
    if (last_h_dir_ == 0 && last_v_dir_ == 0) {
        return;
    }
    last_h_dir_ = 0;
    last_v_dir_ = 0;
    last_follow_delay_ms_ = 0;
    StepperGimbalDevice::GetInstance().SetFollowRates(0, 0, kMaxStepDelayMs);
}

void FaceTracker::TaskEntry(void* arg) { static_cast<FaceTracker*>(arg)->TaskLoop(); }

void FaceTracker::TaskLoop() {
    ESP_LOGI(TAG, "Track task started on core %d", xPortGetCoreID());
    uint32_t frame_i = 0;
    int64_t last_log_us = 0;

    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stop_requested_) {
                break;
            }
        }
        if (paused_ || !camera_) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        FaceTrackCamera* cam = nullptr;
        HumanFaceDetect* det = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stop_requested_ && !paused_ && camera_ && camera_->IsTrackingAcquired() &&
                detector_) {
                cam = camera_;
                det = static_cast<HumanFaceDetect*>(detector_);
            }
        }
        if (!cam || !det) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        const int64_t t0 = esp_timer_get_time();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_in_flight_ = true;
        }
        camera_fb_t* fb = cam->GrabTrackingFrame();
        if (!fb || !fb->buf || fb->len < 2) {
            if (fb) {
                cam->ReturnTrackingFrame(fb);
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                frame_in_flight_ = false;
            }
            vTaskDelay(pdMS_TO_TICKS(kLoopPeriodMs));
            continue;
        }

        // esp_camera RGB565 on S3 is typically byte-order BE in the buffer.
        dl::image::img_t img{};
        img.data = fb->buf;
        img.width = fb->width;
        img.height = fb->height;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;

        auto& results = det->run(img);
        const int64_t t1 = esp_timer_get_time();

        bool has_face = false;
        int err_x = 0;
        int err_y = 0;
        int face_w = 0;
        int face_h = 0;
        int box_x1 = 0;
        int box_y1 = 0;
        int box_x2 = 0;
        int box_y2 = 0;
        int best_area = -1;
        const int faces = static_cast<int>(results.size());
        for (auto& r : results) {
            if (r.box.size() < 4) {
                continue;
            }
            const int area = r.box_area();
            if (area > best_area) {
                best_area = area;
                box_x1 = r.box[0];
                box_y1 = r.box[1];
                box_x2 = r.box[2];
                box_y2 = r.box[3];
                const int cx = (box_x1 + box_x2) / 2;
                const int cy = (box_y1 + box_y2) / 2;
                face_w = box_x2 - box_x1;
                face_h = box_y2 - box_y1;
                err_x = cx - static_cast<int>(fb->width) / 2;
                err_y = cy - static_cast<int>(fb->height) / 2;
                has_face = true;
            }
        }

        const int frame_w = fb->width;
        const int frame_h = fb->height;
        FillPreviewFromFrame(fb);
        cam->ReturnTrackingFrame(fb);
        fb = nullptr;

        FaceTrackerStatus ui_status{};
        StatusSink sink;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            frame_in_flight_ = false;
            status_.detect_ms = static_cast<uint32_t>((t1 - t0) / 1000);
            status_.has_face = has_face;
            status_.err_x = err_x;
            status_.err_y = err_y;
            status_.face_w = face_w;
            status_.face_h = face_h;
            status_.box_x1 = box_x1;
            status_.box_y1 = box_y1;
            status_.box_x2 = box_x2;
            status_.box_y2 = box_y2;
            status_.frame_w = frame_w;
            status_.frame_h = frame_h;
            status_.faces = static_cast<uint32_t>(faces);
            if (has_face) {
                status_.lost_frames = 0;
            } else {
                status_.lost_frames++;
            }
            status_.gimbal_moving = StepperGimbalDevice::GetInstance().IsMoving();
            status_.preview_rgb565 = preview_buf_;
            status_.preview_w = preview_w_;
            status_.preview_h = preview_h_;
            ui_status = status_;
            ui_status.running = running_;
            ui_status.paused = paused_;
            sink = status_sink_;
        }
        if (sink) {
            sink(ui_status);
        }

        ++frame_i;
        if ((t1 - last_log_us) > 1500000) {
            last_log_us = t1;
            ESP_LOGI(TAG,
                     "frame=%u faces=%d has=%d box=%dx%d err=(%d,%d) detect=%ums fb=%dx%d",
                     (unsigned)frame_i, faces, (int)has_face, face_w, face_h, err_x, err_y,
                     (unsigned)((t1 - t0) / 1000), frame_w, frame_h);
        }

        if (has_face) {
            ApplyControl(err_x, err_y, frame_w);
        } else {
            ApplyIdleFollow();
        }

        const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
        const int delay_ms = kLoopPeriodMs - static_cast<int>(elapsed_ms);
        if (delay_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (camera_ && camera_->IsTrackingAcquired()) {
            camera_->ReleaseTracking();
        }
        ReleaseDetector();
        ReleasePreviewBuffer();
        running_ = false;
        paused_ = false;
        frame_in_flight_ = false;
        filt_err_x_ = 0.f;
        filt_err_y_ = 0.f;
        last_h_dir_ = 0;
        last_v_dir_ = 0;
        last_follow_delay_ms_ = 0;
        status_.running = false;
        status_.paused = false;
        status_.has_face = false;
        task_ = nullptr;
    }
    StepperGimbalDevice::GetInstance().Stop();
    ESP_LOGI(TAG, "Track task exit");
    vTaskDelete(nullptr);
}

bool FaceTracker::Start() {
    UiHook start_ui;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return true;
        }
        if (!camera_) {
            ESP_LOGE(TAG, "No FaceTrackCamera registered");
            return false;
        }
        if (!EnsureDetector()) {
            return false;
        }
        if (!camera_->AcquireTracking()) {
            ReleaseDetector();
            ESP_LOGE(TAG, "AcquireTracking failed");
            return false;
        }

        stop_requested_ = false;
        paused_ = false;
        resume_after_photo_ = false;
        filt_err_x_ = 0.f;
        filt_err_y_ = 0.f;
        last_h_dir_ = 0;
        last_v_dir_ = 0;
        last_follow_delay_ms_ = 0;
        status_ = FaceTrackerStatus{};
        status_.running = true;
        running_ = true;
        start_ui = on_start_ui_;

        BaseType_t ok = xTaskCreatePinnedToCore(TaskEntry, "face_track", 8192, this, 1, &task_, 1);
        if (ok != pdPASS) {
            camera_->ReleaseTracking();
            ReleaseDetector();
            running_ = false;
            status_.running = false;
            task_ = nullptr;
            ESP_LOGE(TAG, "Failed to create track task");
            return false;
        }
    }
    if (start_ui) {
        start_ui();
    }
    ESP_LOGI(TAG, "Face tracking started");
    return true;
}

bool FaceTracker::Stop() {
    UiHook stop_ui;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) {
            return true;
        }
        stop_requested_ = true;
        paused_ = false;
        resume_after_photo_ = false;
        stop_ui = on_stop_ui_;
    }
    StepperGimbalDevice::GetInstance().Stop();

    for (int i = 0; i < 100 && IsRunning(); ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (IsRunning()) {
        ESP_LOGW(TAG, "Track task did not exit in time");
        return false;
    }
    if (stop_ui) {
        stop_ui();
    }
    ESP_LOGI(TAG, "Face tracking stopped");
    return true;
}

bool FaceTracker::IsRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

FaceTrackerStatus FaceTracker::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    FaceTrackerStatus s = status_;
    s.running = running_;
    s.paused = paused_;
    s.gimbal_moving = StepperGimbalDevice::GetInstance().IsMoving();
    return s;
}

std::string FaceTracker::GetStatusString() const {
    auto s = GetStatus();
    std::string out = "{";
    out += "\"running\":";
    out += s.running ? "true" : "false";
    out += ",\"paused\":";
    out += s.paused ? "true" : "false";
    out += ",\"has_face\":";
    out += s.has_face ? "true" : "false";
    out += ",\"faces\":" + std::to_string(s.faces);
    out += ",\"err_x\":" + std::to_string(s.err_x);
    out += ",\"err_y\":" + std::to_string(s.err_y);
    out += ",\"face_w\":" + std::to_string(s.face_w);
    out += ",\"face_h\":" + std::to_string(s.face_h);
    out += ",\"box\":[" + std::to_string(s.box_x1) + "," + std::to_string(s.box_y1) + "," +
           std::to_string(s.box_x2) + "," + std::to_string(s.box_y2) + "]";
    out += ",\"gimbal_moving\":";
    out += s.gimbal_moving ? "true" : "false";
    out += ",\"detect_ms\":" + std::to_string(s.detect_ms);
    out += ",\"lost_frames\":" + std::to_string(s.lost_frames);
    out += "}";
    return out;
}
