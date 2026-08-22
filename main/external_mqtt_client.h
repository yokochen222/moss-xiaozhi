#ifndef EXTERNAL_MQTT_CLIENT_H
#define EXTERNAL_MQTT_CLIENT_H

#include "config/ext_mqtt_config.h"

#include <cJSON.h>
#include <esp_timer.h>
#include <functional>
#include <memory>
#include <mqtt.h>
#include <mutex>
#include <string>

class ExternalMqttClient {
public:
    ExternalMqttClient();
    ~ExternalMqttClient();

    bool Start();
    void Stop();
    void Reload();
    void ResetFailCount() { fail_count_ = 0; }
    int fail_count() const { return fail_count_; }
    bool IsConnected() const;
    bool PublishUp(const std::string& type, const std::string& request_id, cJSON* payload);

private:
    void HandleMessage(const std::string& topic, const std::string& payload);
    void HandleTypedMessage(cJSON* root);
    void HandleIrLearn(const std::string& request_id);
    void HandleIrTest(const std::string& request_id, cJSON* payload);
    void HandleDevicesGet(const std::string& request_id);
    void HandleDevicePut(const std::string& request_id, cJSON* payload);
    void HandleDeviceDelete(const std::string& request_id, cJSON* payload);
    void HandleCommandPut(const std::string& request_id, cJSON* payload);
    void HandleCommandDelete(const std::string& request_id, cJSON* payload);
    void HandleExportGet(const std::string& request_id);
    void HandleImport(const std::string& request_id, cJSON* payload);
    void HandleHwControl(const std::string& request_id, cJSON* payload);
    void HandleDeviceConfigGet(const std::string& request_id);
    void HandleDeviceConfigSet(const std::string& request_id, cJSON* payload);
    void PublishHwState(const std::string& request_id, bool ok, const std::string& message);
    void PublishAck(const std::string& type, const std::string& request_id, bool ok,
                    const std::string& message);
    void ScheduleReconnect();
    void DeferChatWake(const std::string& request_id);
    void HandleDeferredChatWake();
    static void ReconnectTimerCallback(void* arg);
    static void WakeDeferTimerCallback(void* arg);

    std::unique_ptr<Mqtt> mqtt_;
    ExtMqttConfig config_;
    bool running_ = false;
    int fail_count_ = 0;
    std::mutex mutex_;
    std::string pending_wake_id_;
    esp_timer_handle_t reconnect_timer_ = nullptr;
    esp_timer_handle_t wake_defer_timer_ = nullptr;
};

#endif
