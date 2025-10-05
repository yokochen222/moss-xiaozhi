#include "ir_handlers.h"
#include "ir_data_manager.h"
#include "device/infrared.h"
#include <esp_log.h>
#include <cJSON.h>
#include <cstring>
#include <algorithm>

#define TAG "IrHandlers"

// 辅助函数前向声明
static esp_err_t SendJsonResponse(httpd_req_t *req, const std::string& json);
static esp_err_t SendErrorResponse(httpd_req_t *req, int status_code, const std::string& message);
static std::string ParseRequestBody(httpd_req_t *req);

namespace api_methods {
namespace ir {

esp_err_t HandleIrSend(httpd_req_t *req) {
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

esp_err_t HandleIrRead(httpd_req_t *req) {
    // 获取红外数据（应该只有一条最新的数据）
    std::vector<std::string> ir_data = IrDataManager::GetInstance().GetIrReceivedData();
    
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

} // namespace ir
} // namespace api_methods

// 辅助函数实现
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
