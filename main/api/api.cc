#include "api.h"
#include "device/infrared.h"
#include <esp_log.h>
#include <esp_http_server.h>
#include <cJSON.h>
#include <cstring>
#include <algorithm>

#define TAG "ApiServer"

// 静态函数前向声明
static esp_err_t HandleIrSend(httpd_req_t *req);
static esp_err_t HandleIrRead(httpd_req_t *req);
static esp_err_t SendJsonResponse(httpd_req_t *req, const std::string& json);
static esp_err_t SendErrorResponse(httpd_req_t *req, int status_code, const std::string& message);
static std::string ParseRequestBody(httpd_req_t *req);

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
        .handler = HandleIrSend,
        .user_ctx = this
    };
    httpd_register_uri_handler((httpd_handle_t)server_, &ir_send_uri);
    
    httpd_uri_t ir_read_uri = {
        .uri = "/ir/read",
        .method = HTTP_GET,
        .handler = HandleIrRead,
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
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    
    // 去除换行符和回车符
    std::string clean_data = data;
    clean_data.erase(std::remove(clean_data.begin(), clean_data.end(), '\n'), clean_data.end());
    clean_data.erase(std::remove(clean_data.begin(), clean_data.end(), '\r'), clean_data.end());
    
    // 限制存储的数据条数，避免内存溢出
    const size_t max_data_count = 1;
    if (ir_received_data_.size() >= max_data_count) {
        ir_received_data_.erase(ir_received_data_.begin());
    }
    
    ir_received_data_.push_back(clean_data);
    ESP_LOGI(TAG, "Added IR data: %s", clean_data.c_str());
}

std::vector<std::string> ApiServer::GetIrReceivedData() {
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    return ir_received_data_;
}

void ApiServer::ClearIrReceivedData() {
    std::lock_guard<std::mutex> lock(ir_data_mutex_);
    ir_received_data_.clear();
    ESP_LOGI(TAG, "Cleared IR received data");
}

// HTTP处理器函数实现
static esp_err_t HandleIrSend(httpd_req_t *req) {
    // 注意：user_ctx在这个处理器中暂未使用，但保留以备将来扩展
    // ApiServer* instance = static_cast<ApiServer*>(req->user_ctx);
    
    // 解析请求体
    std::string body = ParseRequestBody(req);
    if (body.empty()) {
        return SendErrorResponse(req, HTTPD_400_BAD_REQUEST, "Empty request body");
    }
    
    // 解析JSON
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return SendErrorResponse(req, HTTPD_400_BAD_REQUEST, "Invalid JSON format");
    }
    
    // 获取ir_code参数
    cJSON* ir_code = cJSON_GetObjectItem(json, "ir_code");
    if (!cJSON_IsString(ir_code)) {
        cJSON_Delete(json);
        return SendErrorResponse(req, HTTPD_400_BAD_REQUEST, "Missing or invalid 'ir_code' field");
    }
    
    std::string ir_code_str = ir_code->valuestring;
    cJSON_Delete(json);
    
    // 发送红外指令
    bool success = InfraredDevice::GetInstance().SendIrCommand(ir_code_str);
    
    // 构造响应
    cJSON* response = cJSON_CreateObject();
    if (success) {
        cJSON_AddStringToObject(response, "status", "success");
        cJSON_AddStringToObject(response, "message", "Infrared command sent successfully");
        cJSON_AddStringToObject(response, "ir_code", ir_code_str.c_str());
    } else {
        cJSON_AddStringToObject(response, "status", "error");
        cJSON_AddStringToObject(response, "message", "Failed to send infrared command");
    }
    
    char* response_str = cJSON_PrintUnformatted(response);
    std::string response_json(response_str);
    free(response_str);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    return SendJsonResponse(req, response_json);
}

static esp_err_t HandleIrRead(httpd_req_t *req) {
    ApiServer* instance = static_cast<ApiServer*>(req->user_ctx);
    
    // 获取红外数据（应该只有一条最新的数据）
    std::vector<std::string> ir_data = instance->GetIrReceivedData();
    
    // 构造响应
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    
    if (ir_data.empty()) {
        // 没有数据时返回空字符串
        cJSON_AddStringToObject(response, "ir_data", "");
        cJSON_AddNumberToObject(response, "count", 0);
    } else {
        // 获取最新的数据（已经在存储时清理过换行符）
        std::string latest_data = ir_data.back();
        cJSON_AddStringToObject(response, "ir_data", latest_data.c_str());
        cJSON_AddNumberToObject(response, "count", 1);
    }
    
    char* response_str = cJSON_PrintUnformatted(response);
    std::string response_json(response_str);
    free(response_str);
    cJSON_Delete(response);
    
    httpd_resp_set_type(req, "application/json");
    return SendJsonResponse(req, response_json);
}

static esp_err_t SendJsonResponse(httpd_req_t *req, const std::string& json) {
    return httpd_resp_send(req, json.c_str(), HTTPD_RESP_USE_STRLEN);
}

static esp_err_t SendErrorResponse(httpd_req_t *req, int status_code, const std::string& message) {
    cJSON* error_response = cJSON_CreateObject();
    cJSON_AddStringToObject(error_response, "status", "error");
    cJSON_AddStringToObject(error_response, "message", message.c_str());
    
    char* response_str = cJSON_PrintUnformatted(error_response);
    std::string response_json(response_str);
    free(response_str);
    cJSON_Delete(error_response);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_status(req, status_code == HTTPD_400_BAD_REQUEST ? "400 Bad Request" : "500 Internal Server Error");
    return httpd_resp_send(req, response_json.c_str(), HTTPD_RESP_USE_STRLEN);
}

static std::string ParseRequestBody(httpd_req_t *req) {
    char buf[1024];
    int remaining = req->content_len;
    std::string body;
    
    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf, std::min(remaining, (int)sizeof(buf) - 1));
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return "";
        }
        buf[ret] = '\0';
        body += buf;
        remaining -= ret;
    }
    
    return body;
}