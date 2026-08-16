#include "config_handlers.h"
#include "api/http_util.h"
#include "config/ext_mqtt_config.h"
#include "config/moss_config_service.h"
#include "system_info.h"

#include <cJSON.h>
#include <esp_app_desc.h>
#include <wifi_manager.h>

namespace api_methods {
namespace config {

esp_err_t HandleHealth(httpd_req_t* req) {
    auto mqtt = ExtMqttSettings::Load();
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    cJSON_AddStringToObject(obj, "name", "moss-desktop");
    cJSON_AddStringToObject(obj, "hostname", MossConfigService::GetInstance().Hostname().c_str());
    cJSON_AddStringToObject(obj, "instance", MossConfigService::GetInstance().InstanceName().c_str());
    cJSON_AddStringToObject(obj, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    cJSON_AddStringToObject(obj, "mac", SystemInfo::GetMacAddress().c_str());
    const esp_app_desc_t* app = esp_app_get_description();
    cJSON_AddStringToObject(obj, "version", app ? app->version : "");
    cJSON_AddBoolToObject(obj, "bound", mqtt.bound);
    cJSON_AddStringToObject(obj, "display_name", mqtt.display_name.c_str());
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, json);
}

esp_err_t HandleMqttGet(httpd_req_t* req) {
    auto mqtt = ExtMqttSettings::Load();
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddBoolToObject(obj, "bound", mqtt.bound);
    cJSON_AddStringToObject(obj, "broker", mqtt.broker.c_str());
    cJSON_AddNumberToObject(obj, "port", mqtt.port);
    cJSON_AddStringToObject(obj, "client_id", mqtt.client_id.c_str());
    cJSON_AddStringToObject(obj, "display_name", mqtt.display_name.c_str());
    cJSON_AddStringToObject(obj, "username", mqtt.username.c_str());
    cJSON_AddBoolToObject(obj, "password_set", !mqtt.password.empty());
    cJSON_AddStringToObject(obj, "cmd_topic", mqtt.cmd_topic.c_str());
    cJSON_AddStringToObject(obj, "subscribe_topic", mqtt.subscribe_topic.c_str());
    cJSON_AddStringToObject(obj, "publish_topic", mqtt.publish_topic.c_str());
    char* printed = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    std::string json = printed ? printed : "{}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, json);
}

esp_err_t HandleMqttPut(httpd_req_t* req) {
    std::string body = http_util::ReadBody(req);
    cJSON* json = cJSON_Parse(body.c_str());
    if (!json) {
        return http_util::SendError(req, "400 Bad Request", "Invalid JSON");
    }

    auto current = ExtMqttSettings::Load();
    auto str = [&](const char* key, const std::string& fallback) {
        cJSON* item = cJSON_GetObjectItem(json, key);
        if (cJSON_IsString(item) && item->valuestring) {
            return std::string(item->valuestring);
        }
        return fallback;
    };
    auto num = [&](const char* key, int fallback) {
        cJSON* item = cJSON_GetObjectItem(json, key);
        if (cJSON_IsNumber(item)) {
            return item->valueint;
        }
        return fallback;
    };

    ExtMqttConfig next = current;
    next.bound = true;
    next.broker = str("broker", current.broker);
    next.port = num("port", current.port);
    next.client_id = str("client_id", current.client_id);
    next.display_name = str("display_name", current.display_name);
    next.username = str("username", current.username);
    std::string password = str("password", "");
    if (!password.empty()) {
        next.password = password;
    }
    next.cmd_topic = str("cmd_topic", "");
    next.subscribe_topic = str("subscribe_topic", "");
    next.publish_topic = str("publish_topic", "");
    cJSON_Delete(json);

    if (next.broker.empty()) {
        return http_util::SendError(req, "400 Bad Request", "broker is required");
    }

    ExtMqttSettings::ApplyTopicDefaults(next);
    ExtMqttSettings::Save(next);
    MossConfigService::GetInstance().OnConfigSavedStartMqtt();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "message", "saved, connecting cloud channel");
    char* printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    std::string out = printed ? printed : "{\"status\":\"ok\"}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, out);
}

}  // namespace config
}  // namespace api_methods
