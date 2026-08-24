#include "ext_mqtt_config.h"
#include "settings.h"

#include <esp_mac.h>
#include <cstdio>

namespace {
constexpr const char* kNs = "ext_mqtt";
}

std::string ExtMqttSettings::MacSuffix() {
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char buf[8];
    snprintf(buf, sizeof(buf), "%02X%02X", mac[4], mac[5]);
    return buf;
}

std::string ExtMqttSettings::DefaultClientId() {
    return "moss-" + MacSuffix();
}

void ExtMqttSettings::ApplyTopicDefaults(ExtMqttConfig& config) {
    if (config.client_id.empty()) {
        config.client_id = DefaultClientId();
    }
    if (config.cmd_topic.empty()) {
        config.cmd_topic = "moss/client/" + config.client_id + "/cmd";
    }
    if (config.subscribe_topic.empty()) {
        config.subscribe_topic = "moss/client/" + config.client_id + "/#";
    }
    if (config.publish_topic.empty()) {
        config.publish_topic = "moss/device/" + config.client_id + "/up";
    }
    if (config.port <= 0) {
        config.port = 8883;
    }
}

ExtMqttConfig ExtMqttSettings::Load() {
    Settings settings(kNs, false);
    ExtMqttConfig config;
    config.bound = settings.GetInt("bound", 0) != 0;
    config.broker = settings.GetString("broker");
    config.port = settings.GetInt("port", 8883);
    config.client_id = settings.GetString("client_id");
    config.display_name = settings.GetString("display_name");
    config.username = settings.GetString("username");
    config.password = settings.GetString("password");
    config.cmd_topic = settings.GetString("cmd_topic");
    config.subscribe_topic = settings.GetString("sub_topic");
    config.publish_topic = settings.GetString("pub_topic");
    ApplyTopicDefaults(config);
    return config;
}

void ExtMqttSettings::Save(const ExtMqttConfig& config) {
    ExtMqttConfig copy = config;
    ApplyTopicDefaults(copy);
    Settings settings(kNs, true);
    settings.SetInt("bound", copy.bound ? 1 : 0);
    settings.SetString("broker", copy.broker);
    settings.SetInt("port", copy.port);
    settings.SetString("client_id", copy.client_id);
    settings.SetString("display_name", copy.display_name);
    settings.SetString("username", copy.username);
    if (!copy.password.empty()) {
        settings.SetString("password", copy.password);
    }
    settings.SetString("cmd_topic", copy.cmd_topic);
    settings.SetString("sub_topic", copy.subscribe_topic);
    settings.SetString("pub_topic", copy.publish_topic);
}

void ExtMqttSettings::ClearBound() {
    Settings settings(kNs, true);
    settings.SetInt("bound", 0);
}
