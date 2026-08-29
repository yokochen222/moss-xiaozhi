#include "camera_handlers.h"

#include "api/http_util.h"
#include "device/face_tracker.h"
#include "device/moss_camera_stream.h"
#include "device/stepper_gimbal.h"

#include <cJSON.h>
#include <cstring>
#include <optional>
#include <string>

namespace api_methods {
namespace camera {

esp_err_t HandleStream(httpd_req_t* req) {
    return MossCameraStream::GetInstance().HandleStream(req);
}

esp_err_t HandleSnapshot(httpd_req_t* req) {
    return MossCameraStream::GetInstance().HandleSnapshot(req);
}

esp_err_t HandleDisarm(httpd_req_t* req) {
    http_util::SetCors(req);
    MossCameraStream::GetInstance().Disarm();
    return http_util::SendJson(req, "{\"ok\":true}");
}

static int8_t ClampDir(int value) {
    if (value > 0) return 1;
    if (value < 0) return -1;
    return 0;
}

static std::optional<GimbalDir> ParseDir(const std::string& direction) {
    if (direction == "up") return GimbalDir::Up;
    if (direction == "down") return GimbalDir::Down;
    if (direction == "left") return GimbalDir::Left;
    if (direction == "right") return GimbalDir::Right;
    return std::nullopt;
}

esp_err_t HandleGimbal(httpd_req_t* req) {
    http_util::SetCors(req);
    const std::string body = http_util::ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    std::string action;
    std::string direction;
    int h_dir = 0;
    int v_dir = 0;
    int degrees = 12;
    if (root) {
        cJSON* a = cJSON_GetObjectItem(root, "action");
        if (cJSON_IsString(a) && a->valuestring) action = a->valuestring;
        cJSON* d = cJSON_GetObjectItem(root, "direction");
        if (cJSON_IsString(d) && d->valuestring) direction = d->valuestring;
        cJSON* h = cJSON_GetObjectItem(root, "h_dir");
        if (cJSON_IsNumber(h)) h_dir = h->valueint;
        cJSON* v = cJSON_GetObjectItem(root, "v_dir");
        if (cJSON_IsNumber(v)) v_dir = v->valueint;
        cJSON* deg = cJSON_GetObjectItem(root, "degrees");
        if (cJSON_IsNumber(deg)) degrees = deg->valueint;
        cJSON_Delete(root);
    }

    auto& gimbal = StepperGimbalDevice::GetInstance();
    bool ok = false;
    const char* message = "ok";

    if (action == "stop") {
        ok = gimbal.Idle();
    } else if (action == "follow") {
        if (FaceTracker::GetInstance().IsRunning()) {
            return http_util::SendError(req, "409 Conflict", "face track owns gimbal");
        }
        ok = gimbal.SetFollowRates(ClampDir(h_dir), ClampDir(v_dir), 4, StepMode::Half);
    } else if (action == "move") {
        if (FaceTracker::GetInstance().IsRunning()) {
            return http_util::SendError(req, "409 Conflict", "face track owns gimbal");
        }
        auto dir = ParseDir(direction);
        if (!dir) {
            return http_util::SendError(req, "400 Bad Request", "invalid direction");
        }
        if (degrees < 4) degrees = 4;
        if (degrees > 45) degrees = 45;
        const int steps = (degrees * 4096 + 180) / 360;
        ok = gimbal.Move(*dir, static_cast<uint16_t>(steps), StepMode::Half, 4);
    } else {
        return http_util::SendError(req, "400 Bad Request", "unknown action");
    }

    if (!ok) {
        message = "gimbal failed";
    }
    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "message", message);
    cJSON_AddBoolToObject(out, "moving", gimbal.IsMoving());
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    std::string json = printed ? printed : "{\"ok\":false}";
    if (printed) cJSON_free(printed);
    return http_util::SendJson(req, json);
}

esp_err_t HandleFaceTrack(httpd_req_t* req) {
    http_util::SetCors(req);
    auto& tracker = FaceTracker::GetInstance();

    if (req->method == HTTP_GET) {
        return http_util::SendJson(req, tracker.GetStatusString());
    }

    const std::string body = http_util::ReadBody(req);
    cJSON* root = cJSON_Parse(body.c_str());
    std::string action;
    if (root) {
        cJSON* a = cJSON_GetObjectItem(root, "action");
        if (cJSON_IsString(a) && a->valuestring) action = a->valuestring;
        cJSON_Delete(root);
    }

    bool ok = false;
    const char* message = "ok";
    if (action == "start") {
        MossCameraStream::GetInstance().Disarm();
        ok = tracker.Start();
        message = ok ? "started" : "start failed";
    } else if (action == "stop") {
        ok = tracker.Stop();
        message = ok ? "stopped" : "stop failed";
    } else {
        return http_util::SendError(req, "400 Bad Request", "unknown action");
    }

    cJSON* out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    cJSON_AddStringToObject(out, "message", message);
    cJSON_AddBoolToObject(out, "running", tracker.IsRunning());
    char* printed = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    std::string json = printed ? printed : "{\"ok\":false}";
    if (printed) cJSON_free(printed);
    return ok ? http_util::SendJson(req, json)
              : http_util::SendJson(req, json, "503 Service Unavailable");
}

}  // namespace camera
}  // namespace api_methods
