#include "config_handlers.h"
#include "api/http_util.h"
#include "application.h"
#include "config/device_config.h"
#include "config/ext_mqtt_config.h"
#include "config/moss_chat_log.h"
#include "config/moss_config_service.h"
#include "config/product.h"
#include "config/yunxiangji_outbox.h"
#include "device_state_machine.h"

#include <cJSON.h>
#include <wifi_manager.h>
#include <chrono>
#include <functional>
#include <future>

namespace api_methods {
namespace config {

namespace {

struct DeviceConfigResponse {
    bool ok = false;
    std::string error;
    std::string json;
};

DeviceConfigResponse RunOnMainThread(std::function<DeviceConfigResponse()> task) {
    std::promise<DeviceConfigResponse> promise;
    auto future = promise.get_future();
    Application::GetInstance().Schedule(
        [task = std::move(task), &promise]() mutable { promise.set_value(task()); });
    if (future.wait_for(std::chrono::seconds(8)) != std::future_status::ready) {
        return {false, "device config timeout",
                "{\"ok\":false,\"message\":\"device config timeout\"}"};
    }
    return future.get();
}

}  // namespace

esp_err_t HandleHealth(httpd_req_t* req) {
    auto& cfg = MossConfigService::GetInstance();
    cJSON* obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "status", "ok");
    cJSON_AddStringToObject(obj, "name", BOARD_TYPE);
    MossProduct::AddIdentity(obj);
    cJSON_AddStringToObject(obj, "hostname", cfg.Hostname().c_str());
    cJSON_AddStringToObject(obj, "instance", cfg.InstanceName().c_str());
    cJSON_AddStringToObject(obj, "ip", WifiManager::GetInstance().GetIpAddress().c_str());
    cJSON_AddStringToObject(obj, "mac", cfg.CachedMac().c_str());
    cJSON_AddStringToObject(obj, "client_id", cfg.CachedClientId().c_str());
    cJSON_AddStringToObject(obj, "version", cfg.CachedVersion().c_str());
    cJSON_AddBoolToObject(obj, "bound", true);
    cJSON_AddStringToObject(obj, "display_name", cfg.CachedDisplayName().c_str());
    cJSON_AddStringToObject(
        obj, "voice",
        DeviceStateMachine::GetStateName(Application::GetInstance().GetDeviceState()));
    cJSON_AddNumberToObject(obj, "chat_seq", MossChatLog::GetInstance().Seq());
    cJSON_AddNumberToObject(obj, "yunxiangji_outbox",
                            static_cast<double>(YunxiangjiOutbox::GetInstance().Size()));
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
    MossConfigService::GetInstance().CacheIdentity();

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", "ok");
    cJSON_AddStringToObject(resp, "message", "saved");
    char* printed = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    std::string out = printed ? printed : "{\"status\":\"ok\"}";
    if (printed) {
        cJSON_free(printed);
    }
    return http_util::SendJson(req, out);
}

esp_err_t HandleDeviceGet(httpd_req_t* req) {
    const DeviceConfigResponse result = RunOnMainThread([]() -> DeviceConfigResponse {
        cJSON* obj = DeviceConfig::BuildJson();
        cJSON_AddBoolToObject(obj, "ok", true);
        char* printed = cJSON_PrintUnformatted(obj);
        cJSON_Delete(obj);
        DeviceConfigResponse response;
        response.ok = true;
        response.json = printed ? printed : "{\"ok\":true}";
        if (printed) {
            cJSON_free(printed);
        }
        return response;
    });
    return http_util::SendJson(req, result.json);
}

esp_err_t HandleDevicePut(httpd_req_t* req) {
    const std::string body = http_util::ReadBody(req);
    if (body.empty()) {
        return http_util::SendError(req, "400 Bad Request", "Empty body");
    }

    const DeviceConfigResponse result = RunOnMainThread([body]() -> DeviceConfigResponse {
        cJSON* json = cJSON_Parse(body.c_str());
        if (!json) {
            return {false, "Invalid JSON", "{\"ok\":false,\"message\":\"Invalid JSON\"}"};
        }

        std::string error;
        const bool ok = DeviceConfig::Apply(json, &error);
        cJSON_Delete(json);

        cJSON* resp = DeviceConfig::BuildJson();
        cJSON_AddBoolToObject(resp, "ok", ok);
        if (!ok) {
            cJSON_AddStringToObject(resp, "message", error.c_str());
        }
        char* printed = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);

        DeviceConfigResponse response;
        response.ok = ok;
        response.error = error;
        response.json = printed ? printed : "{\"ok\":false}";
        if (printed) {
            cJSON_free(printed);
        }
        return response;
    });

    return result.ok ? http_util::SendJson(req, result.json)
                     : http_util::SendJson(req, result.json, "400 Bad Request");
}

}  // namespace config
}  // namespace api_methods
