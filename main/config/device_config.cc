#include "device_config.h"

#include "application.h"
#include "audio/wake_word_config.h"
#include "board.h"
#include "device/eye_motor.h"
#include "device/lamp_bar.h"
#include "device/lamp_eye.h"
#include "device/lamp_panel.h"
#include "settings.h"

#include <algorithm>
#include <esp_app_desc.h>
#include <wifi_manager.h>

#ifdef HAVE_LVGL
#include "display/lvgl_display/lvgl_display.h"
#include "display/lvgl_display/lvgl_theme.h"
#endif

namespace {

int ClampPercent(int value, int fallback) {
    if (value < 0 || value > 100) {
        return fallback;
    }
    return value;
}

bool JsonHasNumber(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsNumber(item);
}

bool JsonHasBool(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsBool(item);
}

bool JsonHasString(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) && item->valuestring;
}

std::string JsonString(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

int JsonInt(cJSON* obj, const char* key, int fallback) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) {
        return item->valueint;
    }
    return fallback;
}

void AppendWakeWord(cJSON* root) {
    auto* wake = cJSON_CreateObject();
    auto& audio = Application::GetInstance().GetAudioService();
    const auto phrases = audio.GetWakeWordPhrases();
    const std::string engine = audio.GetWakeWordEngine();
    const bool custom = engine == "custom";

    cJSON_AddStringToObject(wake, "engine", engine.c_str());
    cJSON_AddBoolToObject(wake, "custom_supported", custom);
    cJSON_AddBoolToObject(wake, "runtime_configurable", custom);
    if (custom) {
        cJSON_AddStringToObject(wake, "command_format",
                                "lowercase pinyin syllables separated by spaces");
    }
    if (!phrases.empty()) {
        auto* list = cJSON_CreateArray();
        for (const auto& phrase : phrases) {
            cJSON_AddItemToArray(list, cJSON_CreateString(phrase.c_str()));
        }
        cJSON_AddItemToObject(wake, "phrases", list);
    } else {
        cJSON_AddItemToObject(wake, "phrases", cJSON_CreateArray());
    }

    std::vector<WakeWordCommandEntry> entries;
    int threshold = 0;
    if (audio.GetWakeWordConfig(&entries, &threshold)) {
        auto* commands = cJSON_CreateArray();
        for (const auto& entry : entries) {
            cJSON* item = cJSON_CreateObject();
            cJSON_AddStringToObject(item, "command", entry.command.c_str());
            cJSON_AddStringToObject(item, "display", entry.display.c_str());
            cJSON_AddItemToArray(commands, item);
        }
        cJSON_AddItemToObject(wake, "commands", commands);
        cJSON_AddNumberToObject(wake, "threshold", threshold);
    }

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
    int speed = settings.GetInt("default_motor_speed", EyeMotorDevice::DEFAULT_SPEED_PERCENT);
    return ClampPercent(speed, EyeMotorDevice::DEFAULT_SPEED_PERCENT);
}

void SetDefaultMotorSpeedPercent(int speed) {
    speed = std::clamp(speed, 1, 100);
    Settings settings("vendor", true);
    settings.SetInt("default_motor_speed", speed);
}

cJSON* BuildJson() {
    auto& board = Board::GetInstance();
    cJSON* root = cJSON_CreateObject();

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
#ifdef HAVE_LVGL
    if (auto* display = board.GetDisplay(); display && display->GetTheme()) {
        cJSON_AddStringToObject(screen, "theme", display->GetTheme()->name().c_str());
    }
#endif
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
            int volume = cJSON_GetObjectItem(audio, "volume")->valueint;
            volume = ClampPercent(volume, -1);
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
    if (cJSON_IsObject(screen)) {
        if (JsonHasNumber(screen, "brightness")) {
            int brightness = cJSON_GetObjectItem(screen, "brightness")->valueint;
            brightness = ClampPercent(brightness, -1);
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
#ifdef HAVE_LVGL
        if (JsonHasString(screen, "theme")) {
            const char* theme_name = cJSON_GetObjectItem(screen, "theme")->valuestring;
            if (auto* display = board.GetDisplay(); display && display->GetTheme()) {
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    changed = true;
                } else if (error) {
                    *error = "unknown theme";
                    return false;
                }
            }
        }
#endif
    }

    cJSON* wake = cJSON_GetObjectItem(payload, "wake_word");
    if (cJSON_IsObject(wake)) {
        std::vector<WakeWordCommandEntry> entries;
        cJSON* commands = cJSON_GetObjectItem(wake, "commands");
        const bool has_threshold = JsonHasNumber(wake, "threshold");
        bool has_command_fields = false;

        if (cJSON_IsArray(commands)) {
            const int count = cJSON_GetArraySize(commands);
            if (count > static_cast<int>(kMaxWakeWordCommands)) {
                if (error) {
                    *error = "too many wake_word commands (max 8)";
                }
                return false;
            }
            for (int i = 0; i < count; ++i) {
                cJSON* item = cJSON_GetArrayItem(commands, i);
                if (!cJSON_IsObject(item)) {
                    if (error) {
                        *error = "invalid wake_word command entry";
                    }
                    return false;
                }
                WakeWordCommandEntry entry;
                entry.command = JsonString(item, "command");
                entry.display = JsonString(item, "display");
                if (entry.command.empty() || entry.display.empty()) {
                    if (error) {
                        *error = "wake_word command and display are required";
                    }
                    return false;
                }
                entries.push_back(std::move(entry));
            }
            has_command_fields = !entries.empty();
        } else if (JsonHasString(wake, "command") && JsonHasString(wake, "display")) {
            entries.push_back({JsonString(wake, "command"), JsonString(wake, "display")});
            has_command_fields = true;
        }

        if (has_command_fields || has_threshold) {
            auto& audio = Application::GetInstance().GetAudioService();
            std::vector<WakeWordCommandEntry> current_entries;
            int current_threshold = 20;
            if (!audio.GetWakeWordConfig(&current_entries, &current_threshold)) {
                if (error) {
                    *error = "custom wake word not supported on this firmware";
                }
                return false;
            }
            if (entries.empty()) {
                entries = std::move(current_entries);
            }
            const int threshold =
                has_threshold ? JsonInt(wake, "threshold", current_threshold) : current_threshold;
            if (threshold < 1 || threshold > 99) {
                if (error) {
                    *error = "invalid wake_word threshold";
                }
                return false;
            }
            if (!ValidateWakeWordEntries(entries, error)) {
                return false;
            }
            if (!audio.ApplyWakeWordConfig(entries, threshold)) {
                if (error) {
                    *error = "failed to apply wake_word config";
                }
                return false;
            }
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
