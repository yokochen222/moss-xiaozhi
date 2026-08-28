#include "moss_camera_stream.h"

#include "api/http_util.h"
#include "application.h"
#include "device/face_tracker.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <mutex>
#include <vector>

#define TAG "MossCamStream"

namespace {
// Keep DVP running like a live stream so AEC/AWB stay locked between snapshots.
constexpr int kIdleUs = 20 * 1000 * 1000;
constexpr int kGrabPeriodMs = 180;
constexpr int kFirstWaitMs = 6000;
constexpr int kSkipAfterStart = 4;

std::mutex g_mu;
std::vector<uint8_t> g_last_jpeg;
TaskHandle_t g_task = nullptr;
SemaphoreHandle_t g_frame_sem = nullptr;
bool g_armed = false;
int64_t g_touch_us = 0;
int64_t g_jpeg_us = 0;
bool g_held = false;
int g_skip_after_start = 0;

bool VoiceBusy() {
    const auto state = Application::GetInstance().GetDeviceState();
    return state == kDeviceStateSpeaking || state == kDeviceStateListening ||
           state == kDeviceStateConnecting;
}

void ReleaseHeld(MossCameraStream* self) {
    if (!g_held || !self || !self->source()) {
        g_held = false;
        g_skip_after_start = 0;
        return;
    }
    g_held = false;
    g_skip_after_start = 0;
    self->source()->ReleaseLiveStream();
}

void PreviewTask(void* arg) {
    auto* self = static_cast<MossCameraStream*>(arg);
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
        while (true) {
            const int64_t now = esp_timer_get_time();
            bool armed = false;
            {
                std::lock_guard<std::mutex> lock(g_mu);
                armed = g_armed && (now - g_touch_us) < kIdleUs;
                if (!armed) {
                    g_armed = false;
                }
            }
            if (!armed) {
                ReleaseHeld(self);
                break;
            }
            if (FaceTracker::GetInstance().IsRunning() || VoiceBusy()) {
                ReleaseHeld(self);
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            if (!self->source()) {
                vTaskDelay(pdMS_TO_TICKS(300));
                continue;
            }
            if (!g_held) {
                if (!self->source()->AcquireLiveStream()) {
                    vTaskDelay(pdMS_TO_TICKS(400));
                    continue;
                }
                g_held = true;
                g_skip_after_start = kSkipAfterStart;
            }
            camera_fb_t* fb = self->source()->GrabJpeg();
            if (fb && fb->buf && fb->len > 0) {
                if (g_skip_after_start > 0) {
                    --g_skip_after_start;
                    self->source()->ReturnJpeg(fb);
                    continue;
                }
                {
                    std::lock_guard<std::mutex> lock(g_mu);
                    g_last_jpeg.assign(fb->buf, fb->buf + fb->len);
                    g_jpeg_us = esp_timer_get_time();
                }
                self->source()->ReturnJpeg(fb);
                if (g_frame_sem) {
                    xSemaphoreGive(g_frame_sem);
                }
            } else if (fb) {
                self->source()->ReturnJpeg(fb);
            }
            vTaskDelay(pdMS_TO_TICKS(kGrabPeriodMs));
        }
    }
}

void EnsureTask(MossCameraStream* self) {
    if (!g_frame_sem) {
        g_frame_sem = xSemaphoreCreateBinary();
    }
    if (g_task) {
        return;
    }
    if (xTaskCreate(PreviewTask, "cam_prev", 8192, self, 2, &g_task) != pdPASS) {
        g_task = nullptr;
        ESP_LOGE(TAG, "Preview task create failed");
    }
}

void Arm(MossCameraStream* self) {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_armed = true;
        g_touch_us = esp_timer_get_time();
    }
    EnsureTask(self);
    if (g_task) {
        xTaskNotifyGive(g_task);
    }
}

esp_err_t SendCachedOrEmpty(httpd_req_t* req) {
    std::vector<uint8_t> jpeg;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        jpeg = g_last_jpeg;
    }
    if (jpeg.empty()) {
        httpd_resp_set_status(req, "204 No Content");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, nullptr, 0);
    }
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, reinterpret_cast<const char*>(jpeg.data()), jpeg.size());
}

void WaitForFreshJpeg(int64_t request_us) {
    if (!g_frame_sem) {
        return;
    }
    const int64_t deadline_us = request_us + (int64_t)kFirstWaitMs * 1000;
    while (esp_timer_get_time() < deadline_us) {
        int64_t jpeg_us = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            jpeg_us = g_jpeg_us;
        }
        if (jpeg_us >= request_us) {
            return;
        }
        int remain_ms = (int)((deadline_us - esp_timer_get_time()) / 1000);
        if (remain_ms <= 0) {
            return;
        }
        if (remain_ms > 400) {
            remain_ms = 400;
        }
        xSemaphoreTake(g_frame_sem, pdMS_TO_TICKS(remain_ms));
    }
}
}  // namespace

MossCameraStream& MossCameraStream::GetInstance() {
    static MossCameraStream instance;
    return instance;
}

void MossCameraStream::SetSource(LiveJpegSource* source) { source_ = source; }

void MossCameraStream::Disarm() {
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_armed = false;
        g_touch_us = 0;
    }
    if (g_task) {
        xTaskNotifyGive(g_task);
    }
}

bool MossCameraStream::IsArmed() const {
    std::lock_guard<std::mutex> lock(g_mu);
    return g_armed || g_held;
}

esp_err_t MossCameraStream::HandleSnapshot(httpd_req_t* req) {
    http_util::SetCors(req);
    if (FaceTracker::GetInstance().IsRunning() || VoiceBusy()) {
        return SendCachedOrEmpty(req);
    }
    const int64_t request_us = esp_timer_get_time();
    Arm(this);
    WaitForFreshJpeg(request_us);
    return SendCachedOrEmpty(req);
}

esp_err_t MossCameraStream::HandleStream(httpd_req_t* req) { return HandleSnapshot(req); }
