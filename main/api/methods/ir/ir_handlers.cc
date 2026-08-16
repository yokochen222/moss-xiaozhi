#include "ir_handlers.h"
#include "api/http_util.h"
#include "device/infrared.h"
#include "device/ir_catalog.h"
#include "ir_data_manager.h"

#include <esp_log.h>
#include <cJSON.h>
#include <vector>

#define TAG "IrHandlers"

namespace {
std::string JsonStr(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        return item->valuestring;
    }
    return "";
}

esp_err_t OkMessage(httpd_req_t* req, const std::string& message) {
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    cJSON_AddStringToObject(response, "message", message.c_str());
    char* printed = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    std::string json = printed ? printed : "{\"status\":\"success\"}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, json);
}
}  // namespace

namespace api_methods {
namespace ir {

esp_err_t HandleIrSend(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON format");
    }
    cJSON* ir_code = cJSON_GetObjectItem(json, "ir_code");
    if (!cJSON_IsString(ir_code)) {
        cJSON_Delete(json);
        return http_util::SendError(req, "400 Bad Request", "Missing ir_code");
    }
    std::string code = ir_code->valuestring;
    cJSON_Delete(json);
    bool success = InfraredDevice::GetInstance().SendIrCommand(code);
    if (!success) {
        return http_util::SendError(req, "500 Internal Server Error",
                                    "Failed to send infrared command");
    }
    return OkMessage(req, "Infrared command sent successfully");
}

esp_err_t HandleIrRead(httpd_req_t* req) {
    std::vector<std::string> ir_data = IrDataManager::GetInstance().GetIrReceivedData();
    cJSON* response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "success");
    if (ir_data.empty()) {
        cJSON_AddStringToObject(response, "ir_data", "");
        cJSON_AddNumberToObject(response, "count", 0);
    } else {
        std::string latest = IrCatalog::NormalizeCode(ir_data.back());
        cJSON_AddStringToObject(response, "ir_data", latest.c_str());
        cJSON_AddNumberToObject(response, "count", 1);
    }
    char* printed = cJSON_PrintUnformatted(response);
    cJSON_Delete(response);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, json);
}

esp_err_t HandleIrLearn(httpd_req_t* req) {
    IrDataManager::GetInstance().ClearIrReceivedData();
    bool ok = InfraredDevice::GetInstance().StartLearn();
    if (!ok) {
        return http_util::SendError(req, "500 Internal Server Error", "Failed to enter learn mode");
    }
    return OkMessage(req, "learning");
}

esp_err_t HandleIrTest(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }
    std::string code = JsonStr(json, "code");
    if (code.empty()) {
        code = IrCatalog::GetInstance().FindCode(JsonStr(json, "device_id"), JsonStr(json, "id"));
    }
    cJSON_Delete(json);
    if (code.empty()) {
        return http_util::SendError(req, "400 Bad Request", "Missing code");
    }
    bool ok = InfraredDevice::GetInstance().SendIrCommand(IrCatalog::UartPayload(code));
    if (!ok) {
        return http_util::SendError(req, "500 Internal Server Error", "Failed to send");
    }
    return OkMessage(req, "sent");
}

esp_err_t CatalogReply(httpd_req_t* req, IrCatalogStatus status, const char* ok_message) {
    if (status == IrCatalogStatus::kOk) {
        return OkMessage(req, ok_message);
    }
    const char* http_status = "400 Bad Request";
    if (status == IrCatalogStatus::kNotFound) {
        http_status = "404 Not Found";
    } else if (status == IrCatalogStatus::kWriteFailed) {
        http_status = "500 Internal Server Error";
    }
    return http_util::SendError(req, http_status, IrCatalog::StatusMessage(status));
}

esp_err_t HandleIrDevicesGet(httpd_req_t* req) {
    return http_util::SendJson(req, IrCatalog::GetInstance().MetadataJson());
}

esp_err_t HandleIrDevicePut(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }
    IrAppliance appliance;
    appliance.id = JsonStr(json, "id");
    appliance.name = JsonStr(json, "name");
    appliance.type = JsonStr(json, "type");
    cJSON_Delete(json);
    if (appliance.type.empty()) {
        appliance.type = "custom";
    }
    return CatalogReply(req, IrCatalog::GetInstance().UpsertAppliance(appliance, true), "saved");
}

esp_err_t HandleIrDeviceDelete(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }
    std::string id = JsonStr(json, "id");
    cJSON_Delete(json);
    return CatalogReply(req, IrCatalog::GetInstance().DeleteAppliance(id), "deleted");
}

esp_err_t HandleIrCommandPut(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }
    std::string device_id = JsonStr(json, "device_id");
    IrCommand command;
    command.id = JsonStr(json, "id");
    command.name = JsonStr(json, "name");
    command.code = JsonStr(json, "code");
    cJSON_Delete(json);
    return CatalogReply(req, IrCatalog::GetInstance().UpsertCommand(device_id, command), "saved");
}

esp_err_t HandleIrCommandDelete(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }
    std::string device_id = JsonStr(json, "device_id");
    std::string id = JsonStr(json, "id");
    cJSON_Delete(json);
    return CatalogReply(req, IrCatalog::GetInstance().DeleteCommand(device_id, id), "deleted");
}

}  // namespace ir
}  // namespace api_methods
