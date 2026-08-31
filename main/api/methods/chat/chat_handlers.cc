#include "chat_handlers.h"

#include "api/http_util.h"
#include "application.h"
#include "config/moss_chat_log.h"
#include "device_state_machine.h"

#include <cJSON.h>
#include <cstdlib>
#include <cstring>
#include <string>

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
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{\"seq\":0,\"lines\":[]}";
    if (printed)
        cJSON_free(printed);
    return http_util::SendJson(req, json);
}

}  // namespace chat
}  // namespace api_methods
