#include "moss_config_service.h"
#include "api/api.h"
#include "api/methods/ir/ir_data_manager.h"
#include "application.h"
#include "config/moss_chat_log.h"
#include "config/product.h"
#include "device/infrared.h"
#include "device/ir_catalog.h"
#include "ext_mqtt_config.h"
#include "system_info.h"

#include <esp_app_desc.h>
#include <esp_log.h>
#include <mdns.h>
#include <algorithm>
#include <wifi_manager.h>

#define TAG "MossConfig"

MossConfigService& MossConfigService::GetInstance() {
    static MossConfigService instance;
    return instance;
}

MossConfigService::MossConfigService() {
    hostname_ = "moss-" + ExtMqttSettings::MacSuffix();
}

MossConfigService::~MossConfigService() {
    StopMdns();
}

std::string MossConfigService::Hostname() const { return hostname_; }

std::string MossConfigService::InstanceName() const {
    return "MOSS-" + ExtMqttSettings::MacSuffix();
}

void MossConfigService::CacheIdentity() {
    cached_mac_ = SystemInfo::GetMacAddress();
    auto mqtt = ExtMqttSettings::Load();
    cached_client_id_ = ExtMqttSettings::DefaultClientId();
    cached_display_name_ = mqtt.display_name;
    const esp_app_desc_t* app = esp_app_get_description();
    cached_version_ = app ? app->version : "";
}

void MossConfigService::OnNetworkConnected() {
    Application::GetInstance().RegisterChatRelayCallback(
        [](const std::string& event, const std::string& role, const std::string& text,
           const std::string& state) {
            MossChatLog::GetInstance().OnRelay(event, role, text, state);
        });

    IrCatalog::GetInstance().Initialize();
    SeedIrCatalogFromBuiltin();
    InfraredDevice::GetInstance().Start();

    api_methods::ir::IrDataManager::GetInstance().SetOnReceived([](const std::string& data) {
        std::string code = IrCatalog::NormalizeCode(data);
        const size_t preview = std::min(code.size(), static_cast<size_t>(160));
        ESP_LOGI(TAG, "IR received (%u bytes): %.*s", (unsigned)code.size(), (int)preview,
                 code.c_str());
        if (code.empty() || code.rfind("xx", 0) == 0 || code.find(',') == std::string::npos ||
            (code.find("len=") == std::string::npos && code.find("#len") == std::string::npos)) {
            ESP_LOGW(TAG, "IR RX ignored (incomplete waveform, waiting for len=)");
            return;
        }
    });

    StartLanServices();
}

void MossConfigService::StartLanServices() {
    CacheIdentity();
    if (!ApiServer::GetInstance().IsRunning()) {
        ApiServer::GetInstance().Start(5500);
    }
    StartMdns();
}

void MossConfigService::EnterBindMode() {
    StartLanServices();
    bind_mode_ = true;
}

void MossConfigService::LeaveBindMode() {
    StartLanServices();
    bind_mode_ = false;
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
    mdns_txt_item_t txt[] = {
        {"path", "/health"},
        {"product", MossProduct::kId},
        {"board", BOARD_TYPE},
    };
    esp_err_t add_err =
        mdns_service_add(InstanceName().c_str(), "_moss-http", "_tcp", 5500, txt, 3);
    if (add_err != ESP_OK && add_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_service_add: %s", esp_err_to_name(add_err));
    }
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
