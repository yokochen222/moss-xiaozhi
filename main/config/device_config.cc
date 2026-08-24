#include "device_config.h"
#include "product.h"

#include "application.h"
#include "board.h"
#include "device/eye_motor.h"
#include "device/lamp_bar.h"
#include "device/lamp_eye.h"
#include "device/lamp_panel.h"
#include "settings.h"

#include <algorithm>
#include <esp_app_desc.h>
#include <wifi_manager.h>

namespace {

constexpr int kDefaultMotorSpeed = 100;

int ClampPercent(int value, int fallback) {
    if (value < 0 || value > 100) {
        return fallback;
    }
    return value;
}

bool JsonHasNumber(cJSON* obj, const char* key) {
    return cJSON_IsNumber(cJSON_GetObjectItem(obj, key));
}

bool JsonHasBool(cJSON* obj, const char* key) {
    return cJSON_IsBool(cJSON_GetObjectItem(obj, key));
}

void AppendWakeWord(cJSON* root) {
    auto* wake = cJSON_CreateObject();
    cJSON_AddStringToObject(wake, "engine", "xiaozhi");
    cJSON_AddBoolToObject(wake, "custom_supported", false);
    cJSON_AddBoolToObject(wake, "runtime_configurable", false);
    cJSON_AddItemToObject(wake, "phrases", cJSON_CreateArray());
    cJSON_AddItemToObject(root, "wake_word", wake);
}

void AppendHardware(cJSON* root) {
    auto* hw = cJSON_CreateObject();
    auto& eye = LampEyeDevice::GetInstance();
    auto& bar = LampBarDevice::GetInstance();
    auto& panel = LampPanelDevice::GetInstance();
    auto& motor = EyeMotorDevice::GetInstance();

    cJSON_AddBoolToObject(hw, "eye", eye.IsPowered() || eye.IsBreathing());
    cJSON_AddBoolToObject(hw, "bar", bar.IsFlowing());
    cJSON_AddBoolToObject(hw, "panel", panel.IsPanelLed1On() || panel.IsPanelLed2On());
    cJSON_AddBoolToObject(hw, "bottom", panel.IsBottomLedOn());
    cJSON_AddBoolToObject(hw, "motor", motor.IsRunning());
    const char* dir = "stop";
    if (motor.GetState() == EYE_MOTOR_STATE_FORWARD) {
        dir = "forward";
    } else if (motor.GetState() == EYE_MOTOR_STATE_BACKWARD) {
        dir = "backward";
    }
    cJSON_AddStringToObject(hw, "motor_dir", dir);
    cJSON_AddNumberToObject(hw, "motor_speed", motor.GetCurrentSpeedPercent());
    cJSON_AddNumberToObject(hw, "default_motor_speed", DeviceConfig::DefaultMotorSpeedPercent());
    cJSON_AddItemToObject(root, "hardware", hw);
}

}  // namespace

namespace DeviceConfig {

int DefaultMotorSpeedPercent() {
    Settings settings("vendor");
    int speed = settings.GetInt("default_motor_speed", kDefaultMotorSpeed);
    return ClampPercent(speed, kDefaultMotorSpeed);
}

void SetDefaultMotorSpeedPercent(int speed) {
    speed = std::clamp(speed, 1, 100);
    Settings settings("vendor", true);
    settings.SetInt("default_motor_speed", speed);
}

cJSON* BuildJson() {
    auto& board = Board::GetInstance();
    cJSON* root = cJSON_CreateObject();
    MossProduct::AddIdentity(root);

    auto* audio = cJSON_CreateObject();
    if (auto* codec = board.GetAudioCodec()) {
        cJSON_AddNumberToObject(audio, "volume", codec->output_volume());
    }
    Settings vendor("vendor");
    cJSON_AddBoolToObject(audio, "press_to_talk", vendor.GetInt("press_to_talk", 0) != 0);
    cJSON_AddItemToObject(root, "audio", audio);

    auto* screen = cJSON_CreateObject();
    if (auto* backlight = board.GetBacklight()) {
        cJSON_AddNumberToObject(screen, "brightness", backlight->brightness());
    }
    cJSON_AddItemToObject(root, "screen", screen);

    AppendWakeWord(root);
    AppendHardware(root);

    auto* device = cJSON_CreateObject();
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(device, "version", app ? app->version : "");
    cJSON_AddStringToObject(device, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    cJSON_AddStringToObject(device, "ssid", WifiManager::GetInstance().GetSsid().c_str());
    cJSON_AddItemToObject(root, "device", device);

    return root;
}

bool Apply(cJSON* payload, std::string* error) {
    if (!payload) {
        if (error) {
            *error = "missing payload";
        }
        return false;
    }

    auto& board = Board::GetInstance();
    bool changed = false;

    cJSON* audio = cJSON_GetObjectItem(payload, "audio");
    if (cJSON_IsObject(audio)) {
        if (JsonHasNumber(audio, "volume")) {
            int volume = ClampPercent(cJSON_GetObjectItem(audio, "volume")->valueint, -1);
            if (volume < 0) {
                if (error) {
                    *error = "invalid volume";
                }
                return false;
            }
            if (auto* codec = board.GetAudioCodec()) {
                codec->SetOutputVolume(volume);
                changed = true;
            }
        }
        if (JsonHasBool(audio, "press_to_talk")) {
            bool enabled = cJSON_IsTrue(cJSON_GetObjectItem(audio, "press_to_talk"));
            Settings settings("vendor", true);
            settings.SetInt("press_to_talk", enabled ? 1 : 0);
            changed = true;
        }
    }

    cJSON* screen = cJSON_GetObjectItem(payload, "screen");
    if (cJSON_IsObject(screen) && JsonHasNumber(screen, "brightness")) {
        int brightness = ClampPercent(cJSON_GetObjectItem(screen, "brightness")->valueint, -1);
        if (brightness < 0) {
            if (error) {
                *error = "invalid brightness";
            }
            return false;
        }
        if (auto* backlight = board.GetBacklight()) {
            backlight->SetBrightness(static_cast<uint8_t>(brightness), true);
            changed = true;
        }
    }

    cJSON* hw = cJSON_GetObjectItem(payload, "hardware");
    if (cJSON_IsObject(hw) && JsonHasNumber(hw, "default_motor_speed")) {
        int speed = cJSON_GetObjectItem(hw, "default_motor_speed")->valueint;
        if (speed < 1 || speed > 100) {
            if (error) {
                *error = "invalid default_motor_speed";
            }
            return false;
        }
        SetDefaultMotorSpeedPercent(speed);
        changed = true;
    }

    if (!changed && error) {
        *error = "no supported fields";
    }
    return changed;
}

}  // namespace DeviceConfig
