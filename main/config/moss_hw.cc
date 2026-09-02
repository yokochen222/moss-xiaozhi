#include "moss_hw.h"

#include "config/device_config.h"
#include "device/eye_motor.h"
#include "device/lamp_bar.h"
#include "device/lamp_eye.h"
#include "device/lamp_panel.h"

namespace {

std::string JsonString(cJSON* obj, const char* key) {
    if (!obj)
        return "";
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    return "";
}

int JsonInt(cJSON* obj, const char* key, int fallback) {
    if (!obj)
        return fallback;
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item))
        return item->valueint;
    return fallback;
}

}  // namespace

HwApplyResult MossHwApply(cJSON* payload) {
    const std::string device = JsonString(payload, "device");
    const std::string action = JsonString(payload, "action");
    int speed = JsonInt(payload, "speed", DeviceConfig::DefaultMotorSpeedPercent());
    if (speed < 1)
        speed = 1;
    if (speed > 100)
        speed = 100;

    auto eye_on = []() {
        auto& eye = LampEyeDevice::GetInstance();
        return eye.StartBreathing() || eye.TurnOn();
    };
    auto eye_off = []() {
        auto& eye = LampEyeDevice::GetInstance();
        eye.StopBreathing();
        return eye.TurnOff();
    };

    HwApplyResult result;
    if (device == "eye") {
        result.ok = action == "off" ? eye_off() : eye_on();
    } else if (device == "bar") {
        auto& bar = LampBarDevice::GetInstance();
        result.ok = action == "off" ? bar.StopFlow() : bar.StartFlow();
    } else if (device == "panel") {
        auto& panel = LampPanelDevice::GetInstance();
        result.ok = action == "off" ? panel.TurnOffPanelLeds() : panel.TurnOnPanelLeds();
    } else if (device == "bottom") {
        auto& panel = LampPanelDevice::GetInstance();
        result.ok = action == "off" ? panel.TurnOffBottomLed() : panel.TurnOnBottomLed();
    } else if (device == "motor") {
        auto& motor = EyeMotorDevice::GetInstance();
        if (action == "forward" || action == "on" || action == "start") {
            result.ok = motor.StartOscillate(static_cast<uint8_t>(speed));
        } else if (action == "backward") {
            result.ok = motor.StartBackward(static_cast<uint8_t>(speed));
        } else {
            result.ok = motor.Stop();
        }
    } else if (device == "all") {
        auto& bar = LampBarDevice::GetInstance();
        auto& panel = LampPanelDevice::GetInstance();
        auto& motor = EyeMotorDevice::GetInstance();
        if (action == "off") {
            eye_off();
            bar.StopFlow();
            panel.TurnOffAll();
            motor.Stop();
            result.ok = true;
        } else {
            const bool eye_ok = eye_on();
            const bool bar_ok = bar.StartFlow();
            const bool panel_ok = panel.TurnOnAll();
            const bool motor_ok = motor.StartOscillate(static_cast<uint8_t>(speed));
            result.ok = eye_ok && bar_ok && panel_ok && motor_ok;
            if (!result.ok)
                result.message = "partial failure";
        }
    } else {
        result.message = "unknown device";
    }
    return result;
}

cJSON* MossHwStateJson(bool ok, const std::string& message) {
    auto& eye = LampEyeDevice::GetInstance();
    auto& bar = LampBarDevice::GetInstance();
    auto& panel = LampPanelDevice::GetInstance();
    auto& motor = EyeMotorDevice::GetInstance();
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "ok", ok);
    cJSON_AddStringToObject(payload, "message", message.c_str());
    cJSON_AddBoolToObject(payload, "eye", eye.IsPowered() || eye.IsBreathing());
    cJSON_AddBoolToObject(payload, "bar", bar.IsFlowing());
    cJSON_AddBoolToObject(payload, "panel", panel.IsPanelLed1On() || panel.IsPanelLed2On());
    cJSON_AddBoolToObject(payload, "bottom", panel.IsBottomLedOn());
    cJSON_AddBoolToObject(payload, "motor", motor.IsRunning());
    const char* dir = "stop";
    if (motor.GetState() == EYE_MOTOR_STATE_FORWARD) {
        dir = "forward";
    } else if (motor.GetState() == EYE_MOTOR_STATE_BACKWARD) {
        dir = "backward";
    }
    cJSON_AddStringToObject(payload, "motor_dir", dir);
    cJSON_AddNumberToObject(payload, "motor_speed", motor.GetCurrentSpeedPercent());
    return payload;
}
