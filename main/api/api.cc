#include "api.h"
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
    
    // 初始化HTTP服务器配置
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.max_open_sockets = 7;
    config.max_resp_headers = 8;
    config.backlog_conn = 5;
    config.lru_purge_enable = true;
    
    // 启动HTTP服务器
    esp_err_t ret = httpd_start((httpd_handle_t*)&server_, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: %s", esp_err_to_name(ret));
        return false;
    }
    
    // 注册URI处理器
    httpd_uri_t ir_send_uri = {
        .uri = "/ir/send",
        .method = HTTP_POST,
        .handler = api_methods::ir::HandleIrSend,
        .user_ctx = this
    };
    httpd_register_uri_handler((httpd_handle_t)server_, &ir_send_uri);
    
    httpd_uri_t ir_read_uri = {
        .uri = "/ir/read",
        .method = HTTP_GET,
        .handler = api_methods::ir::HandleIrRead,
        .user_ctx = this
    };
    httpd_register_uri_handler((httpd_handle_t)server_, &ir_read_uri);
    
    is_running_ = true;
    ESP_LOGI(TAG, "API server started on port %d", port_);
    ESP_LOGI(TAG, "Available endpoints:");
    ESP_LOGI(TAG, "  POST /ir/send - Send infrared command");
    ESP_LOGI(TAG, "  GET  /ir/read - Read received infrared data");
    
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
