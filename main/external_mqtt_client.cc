#include "external_mqtt_client.h"
#include "api/api.h"
#include "api/methods/ir/ir_data_manager.h"
#include "application.h"
#include "board.h"
#include "config/device_config.h"
#include "config/moss_config_service.h"
#include "device/eye_motor.h"
#include "device/infrared.h"
#include "device/ir_catalog.h"
#include "device/lamp_bar.h"
#include "device/lamp_eye.h"
#include "device/lamp_panel.h"

#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
#include "device/face_tracker.h"
#include "device/moss_camera_stream.h"
#include "device/stepper_gimbal.h"
#endif

#include <esp_log.h>
#include <esp_heap_caps.h>
#include <cstdint>
#include <cstring>

#define TAG "ExternalMqtt"

namespace {
constexpr int kMaxFails = 3;
constexpr uint64_t kReconnectUs = 5ULL * 1000ULL * 1000ULL;
constexpr uint64_t kWakeDeferUs = 20ULL * 1000ULL;

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
}  // namespace

ExternalMqttClient::ExternalMqttClient() {
    esp_timer_create_args_t args = {
        .callback = ReconnectTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ext_mqtt_re",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, &reconnect_timer_);

    esp_timer_create_args_t wake_args = {
        .callback = WakeDeferTimerCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ext_mqtt_wake",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&wake_args, &wake_defer_timer_);
}

ExternalMqttClient::~ExternalMqttClient() {
    Stop();
    if (wake_defer_timer_) {
        esp_timer_stop(wake_defer_timer_);
        esp_timer_delete(wake_defer_timer_);
        wake_defer_timer_ = nullptr;
    }
    if (reconnect_timer_) {
        esp_timer_stop(reconnect_timer_);
        esp_timer_delete(reconnect_timer_);
        reconnect_timer_ = nullptr;
    }
}

bool ExternalMqttClient::IsConnected() const { return mqtt_ && mqtt_->IsConnected(); }

bool ExternalMqttClient::Start() {
    config_ = ExtMqttSettings::Load();
    if (!config_.bound || config_.broker.empty()) {
        ESP_LOGI(TAG, "Not bound, skip MQTT connect");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (mqtt_ && mqtt_->IsConnected()) {
            return true;
        }
        mqtt_.reset();
        auto network = Board::GetInstance().GetNetwork();
        mqtt_ = network->CreateMqtt(1);
        if (!mqtt_) {
            ESP_LOGE(TAG, "Failed to create MQTT client");
            return false;
        }
        mqtt_->SetKeepAlive(240);
        mqtt_->OnConnected([this]() {
            ESP_LOGI(TAG, "External MQTT connected, subscribe %s", config_.subscribe_topic.c_str());
            mqtt_->Subscribe(config_.subscribe_topic, 1);
            MossConfigService::GetInstance().OnMqttConnected();
        });
        mqtt_->OnDisconnected([this]() {
            ESP_LOGI(TAG, "External MQTT disconnected");
            MossConfigService::GetInstance().OnMqttDisconnected();
            if (running_) {
                ScheduleReconnect();
            }
        });
        mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
            HandleMessage(topic, payload);
        });
        running_ = true;
    }

    ESP_LOGI(TAG, "Connecting to %s:%d id=%s", config_.broker.c_str(), config_.port,
             config_.client_id.c_str());
    if (!mqtt_->Connect(config_.broker, config_.port, config_.client_id, config_.username,
                        config_.password)) {
        ESP_LOGE(TAG, "Connect failed, code=%d", mqtt_->GetLastError());
        {
            std::lock_guard<std::mutex> lock(mutex_);
            mqtt_.reset();
            running_ = false;
        }
        fail_count_++;
        if (fail_count_ >= kMaxFails) {
            ESP_LOGW(TAG, "MQTT failed %d times, enter bind mode", fail_count_);
            Application::GetInstance().Schedule(
                []() { MossConfigService::GetInstance().EnterBindMode(); });
        } else {
            running_ = true;
            ScheduleReconnect();
        }
        return false;
    }
    fail_count_ = 0;
    return true;
}

void ExternalMqttClient::Stop() {
    running_ = false;
    if (reconnect_timer_) {
        esp_timer_stop(reconnect_timer_);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
}

void ExternalMqttClient::Reload() {
    Stop();
    Start();
}

void ExternalMqttClient::ScheduleReconnect() {
    if (!running_ || !reconnect_timer_) {
        return;
    }
    esp_timer_stop(reconnect_timer_);
    esp_timer_start_once(reconnect_timer_, kReconnectUs);
}

void ExternalMqttClient::ReconnectTimerCallback(void* arg) {
    auto* self = static_cast<ExternalMqttClient*>(arg);
    Application::GetInstance().Schedule([self]() {
        if (!self->running_) {
            return;
        }
        if (self->fail_count_ >= kMaxFails) {
            MossConfigService::GetInstance().EnterBindMode();
            return;
        }
        ESP_LOGI(TAG, "MQTT reconnect attempt %d", self->fail_count_ + 1);
        self->Start();
    });
}

void ExternalMqttClient::DeferChatWake(const std::string& request_id) {
    pending_wake_id_ = request_id;
    if (!wake_defer_timer_) {
        HandleDeferredChatWake();
        return;
    }
    esp_timer_stop(wake_defer_timer_);
    esp_timer_start_once(wake_defer_timer_, kWakeDeferUs);
}

void ExternalMqttClient::WakeDeferTimerCallback(void* arg) {
    auto* self = static_cast<ExternalMqttClient*>(arg);
    Application::GetInstance().Schedule([self]() { self->HandleDeferredChatWake(); });
}

void ExternalMqttClient::HandleDeferredChatWake() {
    const std::string id = pending_wake_id_;
    pending_wake_id_.clear();

    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    bool ackOk = true;
    std::string ackMsg = "waking";
    bool invoke = false;

    if (state == kDeviceStateConnecting) {
        ackMsg = "connecting";
    } else if (state == kDeviceStateIdle || state == kDeviceStateListening ||
               state == kDeviceStateSpeaking) {
        ackMsg = state == kDeviceStateIdle ? "waking" : "toggling";
        invoke = true;
    } else {
        ackOk = false;
        ackMsg = "busy";
    }

    ESP_LOGI(TAG, "chat.wake state=%d ack=%d msg=%s", static_cast<int>(state), ackOk, ackMsg.c_str());
    PublishAck("chat.ack", id, ackOk, ackMsg);
    if (invoke) {
        app.RequestChatWake("MOSS");
    }
}

bool ExternalMqttClient::PublishUp(const std::string& type, const std::string& request_id,
                                   cJSON* payload) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!mqtt_ || !mqtt_->IsConnected() || config_.publish_topic.empty()) {
        return false;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "id", request_id.c_str());
    cJSON_AddStringToObject(root, "type", type.c_str());
    if (payload) {
        cJSON_AddItemToObject(root, "payload", cJSON_Duplicate(payload, 1));
    } else {
        cJSON_AddItemToObject(root, "payload", cJSON_CreateObject());
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return false;
    }
    bool ok = mqtt_->Publish(config_.publish_topic, printed, 1);
    cJSON_free(printed);
    return ok;
}

void ExternalMqttClient::PublishAck(const std::string& type, const std::string& request_id, bool ok,
                                    const std::string& message) {
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddBoolToObject(payload, "ok", ok);
    cJSON_AddStringToObject(payload, "message", message.c_str());
    if (!PublishUp(type, request_id, payload)) {
        ESP_LOGW(TAG, "PublishAck failed type=%s ok=%d free=%u min_free=%u",
                 type.c_str(), ok, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    }
    cJSON_Delete(payload);
}

void ExternalMqttClient::HandleMessage(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "MQTT in topic=%s len=%d", topic.c_str(), (int)payload.size());
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        ESP_LOGE(TAG, "Invalid JSON");
        return;
    }

    cJSON* event = cJSON_GetObjectItem(root, "event");
    if (cJSON_IsString(event) && strcmp(event->valuestring, "codesuccess") == 0) {
        cJSON* preview = cJSON_GetObjectItem(root, "promptPreview");
        if (cJSON_IsString(preview) && preview->valuestring) {
            std::string text(preview->valuestring);
            Application::GetInstance().Schedule(
                [text]() { Application::GetInstance().HandleExternalTextMessage(text); });
        }
        cJSON_Delete(root);
        return;
    }

    if (!config_.cmd_topic.empty() && !topic.empty() && topic != config_.cmd_topic) {
        cJSON_Delete(root);
        return;
    }

    HandleTypedMessage(root);
    cJSON_Delete(root);
}

void ExternalMqttClient::HandleTypedMessage(cJSON* root) {
    std::string type = JsonString(root, "type");
    std::string id = JsonString(root, "id");
    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    if (type.empty()) {
        return;
    }
    ESP_LOGI(TAG, "cmd type=%s", type.c_str());

    if (type == "chat.wake") {
        DeferChatWake(id);
        return;
    }
    if (type == "task.completed") {
        std::string prompt = JsonString(payload, "prompt");
        if (prompt.empty()) {
            std::string title = JsonString(payload, "title");
            std::string summary = JsonString(payload, "summary");
            if (title.empty()) {
                title = "后台任务";
            }
            prompt = "请明确告知用户：MOSS 后台任务「" + title + "」已经完成。" + summary;
        }
        Application::GetInstance().Schedule([prompt]() {
            Application::GetInstance().HandleExternalTextMessage(prompt);
        });
        PublishAck("task.ack", id, true, "queued");
        return;
    }
    if (type == "hw.control") {
        cJSON* payload_copy = payload ? cJSON_Duplicate(payload, 1) : nullptr;
        Application::GetInstance().Schedule([this, id, payload_copy]() {
            HandleHwControl(id, payload_copy);
            if (payload_copy) {
                cJSON_Delete(payload_copy);
            }
        });
        return;
    }
#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
    if (type == "gimbal.control") {
        // Inline: a queued follow then stop on the main loop runs back-to-back and the motor never starts.
        HandleGimbalControl(id, payload);
        return;
    }
    if (type == "face_track.control") {
        auto& tracker = FaceTracker::GetInstance();
        const std::string action = JsonString(payload, "action");
        bool ok = false;
        std::string message = "ok";
        if (action == "start") {
            MossCameraStream::GetInstance().Disarm();
            ok = tracker.Start();
            message = ok ? "started" : "start failed";
        } else if (action == "stop") {
            ok = tracker.Stop();
            message = ok ? "stopped" : "stop failed";
        } else {
            message = "unknown action";
        }
        PublishAck("face_track.state", id, ok, message);
        return;
    }
#endif
    if (type == "hw.status") {
        PublishHwState(id, true, "ok");
        return;
    }
    if (type == "device.config.get") {
        HandleDeviceConfigGet(id);
        return;
    }
    if (type == "device.config.set") {
        HandleDeviceConfigSet(id, payload);
        return;
    }
    if (type == "bind.hello") {
        PublishAck("bind.ack", id, true, "ok");
        Application::GetInstance().Schedule(
            []() { MossConfigService::GetInstance().OnBindHello(); });
        return;
    }
    if (type == "bind.clear") {
        Application::GetInstance().Schedule(
            []() { MossConfigService::GetInstance().OnBindClear(); });
        PublishAck("bind.ack", id, true, "cleared");
        return;
    }
    if (type == "ir.learn") {
        HandleIrLearn(id);
        return;
    }
    if (type == "ir.test") {
        HandleIrTest(id, payload);
        return;
    }
    if (type == "ir.devices.get") {
        HandleDevicesGet(id);
        return;
    }
    if (type == "ir.device.put") {
        HandleDevicePut(id, payload);
        return;
    }
    if (type == "ir.device.delete") {
        HandleDeviceDelete(id, payload);
        return;
    }
    if (type == "ir.command.put") {
        HandleCommandPut(id, payload);
        return;
    }
    if (type == "ir.command.delete") {
        HandleCommandDelete(id, payload);
        return;
    }
    if (type == "ir.export.get") {
        HandleExportGet(id);
        return;
    }
    if (type == "ir.import") {
        HandleImport(id, payload);
        return;
    }
}

void ExternalMqttClient::HandleIrLearn(const std::string& request_id) {
    ApiServer::GetInstance().ClearIrReceivedData();
    bool ok = InfraredDevice::GetInstance().StartLearn();
    PublishAck("ir.ack", request_id, ok, ok ? "learning" : "learn failed");
}

void ExternalMqttClient::HandleIrTest(const std::string& request_id, cJSON* payload) {
    std::string code = payload ? JsonString(payload, "code") : "";
    const std::string device_id = payload ? JsonString(payload, "device_id") : "";
    const std::string command_id = payload ? JsonString(payload, "id") : "";
    if (code.empty() && (!device_id.empty() || !command_id.empty())) {
        code = IrCatalog::GetInstance().FindCode(device_id, command_id);
        ESP_LOGI(TAG, "ir.test lookup device=%s cmd=%s found=%d", device_id.c_str(),
                 command_id.c_str(), !code.empty());
    }
    if (code.empty()) {
        ESP_LOGW(TAG, "ir.test missing code device=%s cmd=%s", device_id.c_str(),
                 command_id.c_str());
        PublishAck("ir.error", request_id, false, "missing code");
        return;
    }
    ESP_LOGI(TAG, "ir.test send code_len=%u", (unsigned)code.size());
    bool ok = InfraredDevice::GetInstance().SendIrCommand(IrCatalog::UartPayload(code));
    PublishAck("ir.ack", request_id, ok, ok ? "sent" : "send failed");
}

void ExternalMqttClient::HandleDevicesGet(const std::string& request_id) {
    std::string json = IrCatalog::GetInstance().MetadataJson();
    cJSON* parsed = cJSON_Parse(json.c_str());
    PublishUp("ir.devices", request_id, parsed);
    cJSON_Delete(parsed);
}

void ExternalMqttClient::HandleDevicePut(const std::string& request_id, cJSON* payload) {
    if (!payload) {
        PublishAck("ir.error", request_id, false, "missing payload");
        return;
    }
    IrAppliance appliance;
    appliance.id = JsonString(payload, "id");
    appliance.name = JsonString(payload, "name");
    appliance.type = JsonString(payload, "type");
    if (appliance.type.empty()) {
        appliance.type = "custom";
    }
    auto status = IrCatalog::GetInstance().UpsertAppliance(appliance, true);
    if (status == IrCatalogStatus::kOk) {
        PublishAck("ir.ack", request_id, true, "saved");
    } else {
        PublishAck("ir.error", request_id, false, IrCatalog::StatusMessage(status));
    }
}

void ExternalMqttClient::HandleDeviceDelete(const std::string& request_id, cJSON* payload) {
    std::string id = payload ? JsonString(payload, "id") : "";
    auto status = IrCatalog::GetInstance().DeleteAppliance(id);
    if (status == IrCatalogStatus::kOk) {
        PublishAck("ir.ack", request_id, true, "deleted");
    } else {
        PublishAck("ir.error", request_id, false, IrCatalog::StatusMessage(status));
    }
}

void ExternalMqttClient::HandleCommandPut(const std::string& request_id, cJSON* payload) {
    if (!payload) {
        PublishAck("ir.error", request_id, false, "missing payload");
        return;
    }
    std::string appliance_id = JsonString(payload, "device_id");
    IrCommand command;
    command.id = JsonString(payload, "id");
    command.name = JsonString(payload, "name");
    command.code = JsonString(payload, "code");
    auto status = IrCatalog::GetInstance().UpsertCommand(appliance_id, command);
    if (status == IrCatalogStatus::kOk) {
        PublishAck("ir.ack", request_id, true, "saved");
    } else {
        PublishAck("ir.error", request_id, false, IrCatalog::StatusMessage(status));
    }
}

void ExternalMqttClient::HandleCommandDelete(const std::string& request_id, cJSON* payload) {
    std::string appliance_id = payload ? JsonString(payload, "device_id") : "";
    std::string id = payload ? JsonString(payload, "id") : "";
    auto status = IrCatalog::GetInstance().DeleteCommand(appliance_id, id);
    if (status == IrCatalogStatus::kOk) {
        PublishAck("ir.ack", request_id, true, "deleted");
    } else {
        PublishAck("ir.error", request_id, false, IrCatalog::StatusMessage(status));
    }
}

void ExternalMqttClient::HandleExportGet(const std::string& request_id) {
    std::string json = IrCatalog::GetInstance().ExportJson();
    cJSON* parsed = cJSON_Parse(json.c_str());
    PublishUp("ir.export", request_id, parsed);
    cJSON_Delete(parsed);
}

void ExternalMqttClient::HandleImport(const std::string& request_id, cJSON* payload) {
    if (!payload) {
        PublishAck("ir.error", request_id, false, "missing payload");
        return;
    }
    char* printed = cJSON_PrintUnformatted(payload);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    bool replace = true;
    cJSON* mode = cJSON_GetObjectItem(payload, "mode");
    if (cJSON_IsString(mode) && mode->valuestring && strcmp(mode->valuestring, "merge") == 0) {
        replace = false;
    }
    auto status = IrCatalog::GetInstance().ImportCatalog(json, replace);
    if (status == IrCatalogStatus::kOk) {
        PublishAck("ir.ack", request_id, true, "imported");
    } else {
        const char* message = status == IrCatalogStatus::kInvalid
                                  ? IrCatalog::GetInstance().LastErrorMessage()
                                  : IrCatalog::StatusMessage(status);
        PublishAck("ir.error", request_id, false, message);
    }
}

void ExternalMqttClient::PublishHwState(const std::string& request_id, bool ok,
                                        const std::string& message) {
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
    PublishUp("hw.state", request_id, payload);
    cJSON_Delete(payload);
}

#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
void ExternalMqttClient::HandleGimbalControl(const std::string& request_id, cJSON* payload) {
    auto& gimbal = StepperGimbalDevice::GetInstance();
    const std::string action = JsonString(payload, "action");
    bool ok = false;
    std::string message = "ok";
    if (action == "stop") {
        ok = gimbal.Idle();
        if (!ok) {
            message = "idle failed";
        }
    } else if (action == "follow") {
        if (FaceTracker::GetInstance().IsRunning()) {
            ok = false;
            message = "face track owns gimbal";
        } else {
            const int h = JsonInt(payload, "h_dir", 0);
            const int v = JsonInt(payload, "v_dir", 0);
            ok = gimbal.SetFollowRates(h > 0 ? 1 : (h < 0 ? -1 : 0),
                                       v > 0 ? 1 : (v < 0 ? -1 : 0), 4, StepMode::Half);
            if (!ok) {
                message = "follow task failed";
            }
        }
    } else if (action == "move") {
        if (FaceTracker::GetInstance().IsRunning()) {
            ok = false;
            message = "face track owns gimbal";
        } else {
            const std::string direction = JsonString(payload, "direction");
            GimbalDir dir = GimbalDir::Up;
            bool have_dir = true;
            if (direction == "up") {
                dir = GimbalDir::Up;
            } else if (direction == "down") {
                dir = GimbalDir::Down;
            } else if (direction == "left") {
                dir = GimbalDir::Left;
            } else if (direction == "right") {
                dir = GimbalDir::Right;
            } else {
                have_dir = false;
            }
            if (!have_dir) {
                ok = false;
                message = "invalid direction";
            } else {
                int degrees = JsonInt(payload, "degrees", 14);
                if (degrees < 4) {
                    degrees = 4;
                }
                if (degrees > 45) {
                    degrees = 45;
                }
                const int steps = (degrees * 4096 + 180) / 360;
                ok = gimbal.Move(dir, static_cast<uint16_t>(steps), StepMode::Half, 4);
                if (!ok) {
                    message = "move failed";
                }
            }
        }
    } else {
        message = "unknown action";
    }
    PublishAck("gimbal.state", request_id, ok, message);
}
#endif

void ExternalMqttClient::HandleHwControl(const std::string& request_id, cJSON* payload) {
    const std::string device = payload ? JsonString(payload, "device") : "";
    const std::string action = payload ? JsonString(payload, "action") : "";
    int speed = payload ? JsonInt(payload, "speed", DeviceConfig::DefaultMotorSpeedPercent()) : DeviceConfig::DefaultMotorSpeedPercent();
    if (speed < 1) speed = 1;
    if (speed > 100) speed = 100;

    auto eye_on = []() {
        auto& eye = LampEyeDevice::GetInstance();
        return eye.StartBreathing() || eye.TurnOn();
    };
    auto eye_off = []() {
        auto& eye = LampEyeDevice::GetInstance();
        eye.StopBreathing();
        return eye.TurnOff();
    };

    bool ok = false;
    std::string message = "ok";
    if (device == "eye") {
        ok = action == "off" ? eye_off() : eye_on();
    } else if (device == "bar") {
        auto& bar = LampBarDevice::GetInstance();
        ok = action == "off" ? bar.StopFlow() : bar.StartFlow();
    } else if (device == "panel") {
        auto& panel = LampPanelDevice::GetInstance();
        ok = action == "off" ? panel.TurnOffPanelLeds() : panel.TurnOnPanelLeds();
    } else if (device == "bottom") {
        auto& panel = LampPanelDevice::GetInstance();
        ok = action == "off" ? panel.TurnOffBottomLed() : panel.TurnOnBottomLed();
    } else if (device == "motor") {
        auto& motor = EyeMotorDevice::GetInstance();
        if (action == "forward") {
            ok = motor.StartForward(static_cast<uint8_t>(speed));
        } else if (action == "backward") {
            ok = motor.StartBackward(static_cast<uint8_t>(speed));
        } else {
            ok = motor.Stop();
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
            ok = true;
        } else {
            const bool eye_ok = eye_on();
            const bool bar_ok = bar.StartFlow();
            const bool panel_ok = panel.TurnOnAll();
            const bool motor_ok = motor.StartForward(static_cast<uint8_t>(speed));
            ok = eye_ok && bar_ok && panel_ok && motor_ok;
            if (!ok) {
                message = "partial failure";
            }
        }
    } else {
        message = "unknown device";
    }
    PublishHwState(request_id, ok, message);
}

void ExternalMqttClient::HandleDeviceConfigGet(const std::string& request_id) {
    Application::GetInstance().Schedule([this, request_id]() {
        cJSON* payload = DeviceConfig::BuildJson();
        cJSON_AddBoolToObject(payload, "ok", true);
        if (!PublishUp("device.config", request_id, payload)) {
            ESP_LOGW(TAG, "device.config publish failed free=%u min_free=%u",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
        }
        cJSON_Delete(payload);
    });
}

void ExternalMqttClient::HandleDeviceConfigSet(const std::string& request_id, cJSON* payload) {
    cJSON* payload_copy = payload ? cJSON_Duplicate(payload, 1) : nullptr;
    Application::GetInstance().Schedule([this, request_id, payload_copy]() {
        std::string error;
        bool ok = DeviceConfig::Apply(payload_copy, &error);
        cJSON* resp = DeviceConfig::BuildJson();
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (!ok) {
            cJSON_AddStringToObject(resp, "message", error.c_str());
        }
        PublishUp("device.config", request_id, resp);
        cJSON_Delete(resp);
        if (payload_copy) {
            cJSON_Delete(payload_copy);
        }
    });
}
