#include "external_mqtt_client.h"
#include "application.h"
#include "board.h"
#include <esp_log.h>

#define TAG "ExternalMqtt"

static const char* EXTERNAL_MQTT_BROKER = "v8aaa396.ala.cn-hangzhou.emqxsl.cn";
static const int EXTERNAL_MQTT_PORT = 8883;
static const char* EXTERNAL_MQTT_CLIENT_ID = "esp32-external-audio";
static const char* EXTERNAL_MQTT_SUBSCRIBE_TOPIC = "moss/client/esp32-dev-02/#";

ExternalMqttClient::ExternalMqttClient() {}

ExternalMqttClient::~ExternalMqttClient() {
    Stop();
}

bool ExternalMqttClient::Start() {
    if (running_) {
        ESP_LOGW(TAG, "External MQTT client already running");
        return true;
    }

    auto network = Board::GetInstance().GetNetwork();
    mqtt_ = network->CreateMqtt(1);
    if (!mqtt_) {
        ESP_LOGE(TAG, "Failed to create MQTT client");
        return false;
    }

    mqtt_->SetKeepAlive(240);

    mqtt_->OnConnected([this]() {
        ESP_LOGI(TAG, "External MQTT connected, subscribing to %s", EXTERNAL_MQTT_SUBSCRIBE_TOPIC);
        mqtt_->Subscribe(EXTERNAL_MQTT_SUBSCRIBE_TOPIC, 1);
    });

    mqtt_->OnDisconnected([]() {
        ESP_LOGI(TAG, "External MQTT disconnected");
    });

    mqtt_->OnMessage([this](const std::string& topic, const std::string& payload) {
        HandleMessage(topic, payload);
    });

    running_ = true;

    ESP_LOGI(TAG, "Connecting to external MQTT broker %s:%d", EXTERNAL_MQTT_BROKER, EXTERNAL_MQTT_PORT);
    if (!mqtt_->Connect(EXTERNAL_MQTT_BROKER, EXTERNAL_MQTT_PORT, EXTERNAL_MQTT_CLIENT_ID, "code", "123456")) {
        ESP_LOGE(TAG, "Failed to connect to external MQTT broker");
        mqtt_.reset();
        running_ = false;
        return false;
    }

    return true;
}

void ExternalMqttClient::Stop() {
    if (!running_) {
        return;
    }
    running_ = false;

    if (mqtt_) {
        mqtt_->Disconnect();
        mqtt_.reset();
    }
}

void ExternalMqttClient::HandleMessage(const std::string& topic, const std::string& payload) {
    ESP_LOGI(TAG, "HandleMessage: topic='%s' payload_len=%d", topic.c_str(), (int)payload.size());
    ParseTextEvent(payload);
}

bool ExternalMqttClient::ParseTextEvent(const std::string& payload) {
    cJSON* root = cJSON_Parse(payload.c_str());
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse JSON: %s", payload.c_str());
        return false;
    }

    cJSON* event = cJSON_GetObjectItem(root, "event");
    if (!cJSON_IsString(event)) {
        ESP_LOGD(TAG, "Message has no 'event' field, skipping");
        cJSON_Delete(root);
        return false;
    }

    const char* event_str = event->valuestring;
    ESP_LOGI(TAG, "Received event: %s", event_str);

    if (strcmp(event_str, "codesuccess") == 0) {
        cJSON* prompt_preview = cJSON_GetObjectItem(root, "promptPreview");
        if (cJSON_IsString(prompt_preview) && prompt_preview->valuestring != nullptr) {
            std::string text(prompt_preview->valuestring);
            ESP_LOGI(TAG, "codesuccess event: promptPreview=%s", text.c_str());

            Application::GetInstance().Schedule([text]() {
                Application::GetInstance().HandleExternalTextMessage(text);
            });
        } else {
            ESP_LOGW(TAG, "end event missing promptPreview field");
        }
    } else {
        ESP_LOGD(TAG, "Ignoring event type: %s", event_str);
    }

    cJSON_Delete(root);
    return true;
}
