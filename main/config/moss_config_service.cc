#include "moss_config_service.h"
#include "api/api.h"
#include "application.h"
#include "ext_mqtt_config.h"

#include <cJSON.h>

#include <esp_log.h>
#include <esp_netif.h>
#include <esp_timer.h>
#include <mdns.h>

#define TAG "MossConfig"

namespace {
constexpr int kHelloTimeoutUs = 30 * 1000 * 1000;
}

MossConfigService& MossConfigService::GetInstance() {
    static MossConfigService instance;
    return instance;
}

MossConfigService::MossConfigService() {
    hostname_ = "moss-" + ExtMqttSettings::MacSuffix();
    esp_timer_create_args_t args = {
        .callback = HelloTimeoutCallback,
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bind_hello",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&args, reinterpret_cast<esp_timer_handle_t*>(&hello_timer_));
}

MossConfigService::~MossConfigService() {
    if (hello_timer_) {
        esp_timer_stop(reinterpret_cast<esp_timer_handle_t>(hello_timer_));
        esp_timer_delete(reinterpret_cast<esp_timer_handle_t>(hello_timer_));
    }
    StopMdns();
}

std::string MossConfigService::Hostname() const { return hostname_; }

std::string MossConfigService::InstanceName() const {
    return "MOSS-" + ExtMqttSettings::MacSuffix();
}

void MossConfigService::OnNetworkConnected() {
    // Keep LAN HTTP up for onboard MJPEG even after MQTT bind.
    if (!ApiServer::GetInstance().IsRunning()) {
        ApiServer::GetInstance().Start(5500);
    }
    StartMdns();

    auto config = ExtMqttSettings::Load();
    if (!config.bound || config.broker.empty()) {
        ESP_LOGI(TAG, "Unbound: entering LAN bind mode");
        EnterBindMode();
        return;
    }
    ESP_LOGI(TAG, "Bound: starting external MQTT, HTTP stays on for camera");
    mqtt_client_.Start();
}

void MossConfigService::EnterBindMode() {
    if (!ApiServer::GetInstance().IsRunning()) {
        ApiServer::GetInstance().Start(5500);
    }
    StartMdns();
    bind_mode_ = true;
}

void MossConfigService::LeaveBindMode() {
    awaiting_hello_ = false;
    if (hello_timer_) {
        esp_timer_stop(reinterpret_cast<esp_timer_handle_t>(hello_timer_));
    }
    // Do not stop HTTP/mDNS: onboard camera stream needs :5500.
    bind_mode_ = false;
    ESP_LOGI(TAG, "Left bind mode, HTTP/mDNS kept for camera");
}

void MossConfigService::OnMqttConnected() {
    mqtt_client_.ResetFailCount();
    if (awaiting_hello_ && hello_timer_) {
        esp_timer_stop(reinterpret_cast<esp_timer_handle_t>(hello_timer_));
        esp_timer_start_once(reinterpret_cast<esp_timer_handle_t>(hello_timer_), kHelloTimeoutUs);
        ESP_LOGI(TAG, "MQTT up, waiting for bind.hello");
    }
}

void MossConfigService::OnMqttDisconnected() {}

void MossConfigService::OnBindHello() {
    ESP_LOGI(TAG, "bind.hello received");
    LeaveBindMode();
}

void MossConfigService::OnBindClear() {
    ExtMqttSettings::ClearBound();
    mqtt_client_.Stop();
    EnterBindMode();
}

void MossConfigService::OnConfigSavedStartMqtt() {
    awaiting_hello_ = true;
    EnterBindMode();
    // MQTT TLS connect is slow; do not block the /config/mqtt HTTP handler.
    Application::GetInstance().Schedule([this]() { mqtt_client_.Reload(); });
}

bool MossConfigService::PublishUp(const std::string& type, const std::string& request_id,
                                  cJSON* payload) {
    return mqtt_client_.PublishUp(type, request_id, payload);
}

void MossConfigService::StartMdns() {
    if (mdns_started_) {
        return;
    }
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(hostname_.c_str());
    mdns_instance_name_set(InstanceName().c_str());
    mdns_service_add(InstanceName().c_str(), "_moss-http", "_tcp", 5500, nullptr, 0);
    mdns_started_ = true;
    ESP_LOGI(TAG, "mDNS started hostname=%s.local instance=%s", hostname_.c_str(),
             InstanceName().c_str());
}

void MossConfigService::StopMdns() {
    if (!mdns_started_) {
        return;
    }
    mdns_service_remove("_moss-http", "_tcp");
    mdns_free();
    mdns_started_ = false;
}

void MossConfigService::HelloTimeoutCallback(void* arg) {
    auto* self = static_cast<MossConfigService*>(arg);
    if (self->awaiting_hello_) {
        ESP_LOGW(TAG, "No bind.hello in 30s, keeping HTTP bind mode");
    }
}
