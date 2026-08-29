#pragma once

#include "external_mqtt_client.h"

#include <cJSON.h>
#include <memory>
#include <string>

class MossConfigService {
public:
    static MossConfigService& GetInstance();

    void OnNetworkConnected();
    void EnterBindMode();
    void LeaveBindMode();
    void OnMqttConnected();
    void OnMqttDisconnected();
    void AnnounceOnline();
    void OnBindHello();
    void OnBindClear();
    void OnConfigSavedStartMqtt();
    bool IsBindMode() const { return bind_mode_; }
    bool PublishUp(const std::string& type, const std::string& request_id, cJSON* payload);
    ExternalMqttClient& Mqtt() { return mqtt_client_; }

    std::string Hostname() const;
    std::string InstanceName() const;

private:
    MossConfigService();
    ~MossConfigService();

    void StartLanServices();
    void StartMdns();
    void StopMdns();
    static void HelloTimeoutCallback(void* arg);

    ExternalMqttClient mqtt_client_;
    bool bind_mode_ = false;
    bool mdns_started_ = false;
    bool awaiting_hello_ = false;
    void* hello_timer_ = nullptr;
    std::string hostname_;
};
