#ifndef MQTT_PROTOCOL_H
#define MQTT_PROTOCOL_H

#include <esp_timer.h>
#include <cJSON.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mqtt.h>
#include <psa/crypto.h>
#include <udp.h>
#include "protocol.h"

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>

#define MQTT_PING_INTERVAL_SECONDS 90
#define MQTT_RECONNECT_INTERVAL_MS 60000

#define MQTT_PROTOCOL_SERVER_HELLO_EVENT (1 << 0)
#define MQTT_PROTOCOL_CLOSE_EVENT (1 << 1)

class MqttProtocol : public Protocol {
public:
    MqttProtocol();
    ~MqttProtocol();

    bool Start() override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;
    void SendUdpHolePunch() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;

private:
    // Alive flag for safe scheduled callbacks - set to false in destructor
    std::shared_ptr<std::atomic<bool>> alive_ = std::make_shared<std::atomic<bool>>(true);

    EventGroupHandle_t event_group_handle_;

    std::string publish_topic_;

    mutable std::mutex channel_mutex_;
    std::mutex crypto_mutex_;
    std::unique_ptr<Mqtt> mqtt_;
    std::unique_ptr<Udp> udp_;
    psa_key_id_t aes_key_id_ = PSA_KEY_ID_NULL;
    std::string aes_nonce_;
    std::string udp_server_;
    int udp_port_;
    uint32_t local_sequence_;
    uint32_t remote_sequence_;
    esp_timer_handle_t reconnect_timer_;

    bool StartMqttClient(bool report_error = false);
    void ParseServerHello(const cJSON* root);
    bool DecodeHexString(const std::string& hex_string, std::string& decoded);
    bool CryptAesCtr(const uint8_t* input, size_t input_size, const uint8_t* nonce,
                     uint8_t* output);

    bool SendText(const std::string& text) override;
    std::string GetHelloMessage();
};

#endif  // MQTT_PROTOCOL_H
