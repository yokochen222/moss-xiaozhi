#ifndef EXTERNAL_MQTT_CLIENT_H
#define EXTERNAL_MQTT_CLIENT_H

#include <string>
#include <functional>
#include <memory>
#include <mqtt.h>
#include <cJSON.h>

class ExternalMqttClient {
public:
    using TextMessageCallback = std::function<void(const std::string& prompt_preview)>;

    ExternalMqttClient();
    ~ExternalMqttClient();

    bool Start();
    void Stop();

    void OnTextMessage(TextMessageCallback callback) { on_text_message_ = callback; }

private:
    void HandleMessage(const std::string& topic, const std::string& payload);
    bool ParseTextEvent(const std::string& payload);

    std::unique_ptr<Mqtt> mqtt_;
    bool running_ = false;
    TextMessageCallback on_text_message_;
};

#endif // EXTERNAL_MQTT_CLIENT_H
