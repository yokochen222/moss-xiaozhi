#include "api.h"
#include "http_util.h"
#include "methods/config/config_handlers.h"
#include "methods/ir/ir_handlers.h"
#include "methods/ir/ir_data_manager.h"

#include <esp_log.h>
#include <esp_http_server.h>

#define TAG "ApiServer"

ApiServer::ApiServer() : is_running_(false), port_(5500), server_(nullptr) {
}

ApiServer::~ApiServer() {
    Stop();
}

ApiServer& ApiServer::GetInstance() {
    static ApiServer instance;
    return instance;
}

bool ApiServer::Start(int port) {
    if (is_running_) {
        ESP_LOGW(TAG, "API server is already running");
        return true;
    }

    port_ = port;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.max_open_sockets = 7;
    config.max_uri_handlers = 24;
    config.max_resp_headers = 16;
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    config.uri_match_fn = httpd_uri_match_wildcard;

    esp_err_t ret = httpd_start((httpd_handle_t*)&server_, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }

    auto add = [this](const char* uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t*)) {
        httpd_uri_t item = {};
        item.uri = uri;
        item.method = method;
        item.handler = handler;
        item.user_ctx = this;
        httpd_register_uri_handler((httpd_handle_t)server_, &item);
    };

    add("/health", HTTP_GET, api_methods::config::HandleHealth);
    add("/config/mqtt", HTTP_GET, api_methods::config::HandleMqttGet);
    add("/config/mqtt", HTTP_PUT, api_methods::config::HandleMqttPut);
    add("/config/device", HTTP_GET, api_methods::config::HandleDeviceGet);
    add("/config/device", HTTP_PUT, api_methods::config::HandleDevicePut);
    add("/ir/send", HTTP_POST, api_methods::ir::HandleIrSend);
    add("/ir/read", HTTP_GET, api_methods::ir::HandleIrRead);
    add("/ir/learn", HTTP_POST, api_methods::ir::HandleIrLearn);
    add("/ir/test", HTTP_POST, api_methods::ir::HandleIrTest);
    add("/ir/devices", HTTP_GET, api_methods::ir::HandleIrDevicesGet);
    add("/ir/device", HTTP_PUT, api_methods::ir::HandleIrDevicePut);
    add("/ir/device/delete", HTTP_POST, api_methods::ir::HandleIrDeviceDelete);
    add("/ir/command", HTTP_PUT, api_methods::ir::HandleIrCommandPut);
    add("/ir/command/delete", HTTP_POST, api_methods::ir::HandleIrCommandDelete);
    add("/ir/export", HTTP_GET, api_methods::ir::HandleIrExport);
    add("/ir/import", HTTP_POST, api_methods::ir::HandleIrImport);
    add("/*", HTTP_OPTIONS, http_util::HandleOptions);

    is_running_ = true;
    ESP_LOGI(TAG, "API server started on port %d", port_);
    return true;
}

void ApiServer::Stop() {
    if (!is_running_) {
        return;
    }
    if (server_) {
        httpd_stop((httpd_handle_t)server_);
        server_ = nullptr;
    }
    is_running_ = false;
    ESP_LOGI(TAG, "API server stopped");
}

void ApiServer::AddIrReceivedData(const std::string& data) {
    api_methods::ir::IrDataManager::GetInstance().AddIrReceivedData(data);
}

std::vector<std::string> ApiServer::GetIrReceivedData() {
    return api_methods::ir::IrDataManager::GetInstance().GetIrReceivedData();
}

void ApiServer::ClearIrReceivedData() {
    api_methods::ir::IrDataManager::GetInstance().ClearIrReceivedData();
}
