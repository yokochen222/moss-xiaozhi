#pragma once

#include <string>

struct ExtMqttConfig {
    bool bound = false;
    std::string broker;
    int port = 8883;
    std::string client_id;
    std::string display_name;
    std::string username;
    std::string password;
    std::string cmd_topic;
    std::string subscribe_topic;
    std::string publish_topic;
};

class ExtMqttSettings {
public:
    static ExtMqttConfig Load();
    static void Save(const ExtMqttConfig& config);
    static void ClearBound();
    static void ApplyTopicDefaults(ExtMqttConfig& config);
    static std::string DefaultClientId();
    static std::string MacSuffix();
};
