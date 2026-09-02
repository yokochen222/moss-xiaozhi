#include "chat_handlers.h"

#include "api/http_util.h"
#include "application.h"
#include "config/moss_chat_log.h"
#include "config/yunxiangji_outbox.h"
#include "device_state_machine.h"

#include <cJSON.h>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace api_methods {
namespace chat {

namespace {

std::string JsonString(cJSON* obj, const char* key) {
    if (!obj)
        return "";
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        return item->valuestring;
    return "";
}

}  // namespace

esp_err_t HandleChatWake(httpd_req_t* req) {
    auto& app = Application::GetInstance();
    const auto state = app.GetDeviceState();
    bool ok = true;
    const char* message = "waking";
    bool invoke = false;
    if (state == kDeviceStateConnecting) {
        message = "connecting";
    } else if (state == kDeviceStateIdle || state == kDeviceStateListening ||
               state == kDeviceStateSpeaking) {
        message = state == kDeviceStateIdle ? "waking" : "toggling";
        invoke = true;
    } else {
        ok = false;
        message = "busy";
    }
    if (invoke) {
        app.RequestChatWake("MOSS");
    }
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "ok", ok);
    cJSON_AddStringToObject(obj, "message", message);
    const char* reported = DeviceStateMachine::GetStateName(state);
    if (invoke && state == kDeviceStateIdle) {
        reported = "connecting";
    }
    cJSON_AddStringToObject(obj, "state", reported);
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{\"ok\":false}";
    if (printed)
        cJSON_free(printed);
    return http_util::SendJson(req, json, ok ? "200 OK" : "409 Conflict");
}

esp_err_t HandleChatSay(httpd_req_t* req) {
    const std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    std::string text = JsonString(json, "text");
    if (text.empty())
        text = JsonString(json, "prompt");
    std::string announce = JsonString(json, "announce");
    if (json)
        cJSON_Delete(json);
    if (text.empty()) {
        return http_util::SendError(req, "400 Bad Request", "missing text");
    }
    Application::GetInstance().HandleExternalTextMessage(text, announce);
    return http_util::SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleChatSync(httpd_req_t* req) {
    uint32_t since = 0;
    char query[64] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char since_buf[16] = {0};
        if (httpd_query_key_value(query, "since", since_buf, sizeof(since_buf)) == ESP_OK) {
            since = static_cast<uint32_t>(strtoul(since_buf, nullptr, 10));
        }
    }
    auto& log = MossChatLog::GetInstance();
    auto lines = log.Since(since);
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddNumberToObject(obj, "seq", log.Seq());
    cJSON_AddStringToObject(
        obj, "voice",
        DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()));
    cJSON* arr = cJSON_AddArrayToObject(obj, "lines");
    for (const auto& line : lines) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "seq", line.seq);
        cJSON_AddStringToObject(item, "role", line.role.c_str());
        cJSON_AddStringToObject(item, "text", line.text.c_str());
        if (!line.state.empty()) {
            cJSON_AddStringToObject(item, "state", line.state.c_str());
        }
        cJSON_AddItemToArray(arr, item);
    }
    cJSON_AddItemToObject(obj, "yunxiangji_creates", YunxiangjiOutbox::GetInstance().ToJsonArray());
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{\"seq\":0,\"lines\":[]}";
    if (printed)
        cJSON_free(printed);
    return http_util::SendJson(req, json);
}

esp_err_t HandleYunxiangjiOutboxGet(httpd_req_t* req) {
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddItemToObject(obj, "items", YunxiangjiOutbox::GetInstance().ToJsonArray());
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{\"items\":[]}";
    if (printed)
        cJSON_free(printed);
    return http_util::SendJson(req, json);
}

esp_err_t HandleYunxiangjiOutboxAck(httpd_req_t* req) {
    const std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    std::vector<std::string> ids;
    cJSON* arr = json ? cJSON_GetObjectItem(json, "ids") : nullptr;
    if (cJSON_IsArray(arr)) {
        const int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(arr, i);
            if (cJSON_IsString(item) && item->valuestring) {
                ids.emplace_back(item->valuestring);
            }
        }
    }
    cJSON* one = json ? cJSON_GetObjectItem(json, "id") : nullptr;
    if (cJSON_IsString(one) && one->valuestring) {
        ids.emplace_back(one->valuestring);
    }
    if (json)
        cJSON_Delete(json);
    YunxiangjiOutbox::GetInstance().Ack(ids);
    return http_util::SendJson(req, "{\"ok\":true}");
}

esp_err_t HandleYunxiangjiInboxPut(httpd_req_t* req) {
    const std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "invalid json");
    }
    cJSON* arr = cJSON_GetObjectItem(json, "items");
    if (!cJSON_IsArray(arr) && cJSON_IsArray(json)) {
        arr = json;
    }
    std::vector<YunxiangjiInboxItem> items;
    if (cJSON_IsArray(arr)) {
        const int n = cJSON_GetArraySize(arr);
        for (int i = 0; i < n; ++i) {
            cJSON* row = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsObject(row)) {
                continue;
            }
            YunxiangjiInboxItem item;
            item.id = JsonString(row, "id");
            item.content = JsonString(row, "content");
            item.title = JsonString(row, "title");
            item.status = JsonString(row, "status");
            item.at = JsonString(row, "at");
            item.device_create_id = JsonString(row, "deviceCreateId");
            if (item.id.empty() && item.content.empty()) {
                continue;
            }
            items.push_back(item);
        }
    }
    if (json) {
        cJSON_Delete(json);
    }
    YunxiangjiInbox::GetInstance().Replace(std::move(items));
    return http_util::SendJson(req, "{\"ok\":true}");
}

}  // namespace chat
}  // namespace api_methods
