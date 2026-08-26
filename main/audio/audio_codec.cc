#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <cstring>
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_MOSS_DESKTOP
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "mcp/utils/pca9685_driver.h"
#endif

#define TAG "AudioCodec"

#if CONFIG_BOARD_TYPE_MOSS_DESKTOP
namespace {

constexpr uint8_t kNs4150PaPcaChannel = 1;
bool ns4150_pa_enabled = false;

}  // namespace

void MossDesktopSetNs4150Pa(bool enable) {
    if (ns4150_pa_enabled == enable) {
        return;
    }
    if (!Pca9685::GetInstance().IsReady()) {
        return;
    }
    if (!Pca9685::GetInstance().SetDigital(kNs4150PaPcaChannel, enable)) {
        ESP_LOGW(TAG, "NS4150B EN %s failed (PCA9685 ch%d)", enable ? "on" : "off",
                 (int)kNs4150PaPcaChannel);
        return;
    }
    ns4150_pa_enabled = enable;
    ESP_LOGI(TAG, "NS4150B EN %s (PCA9685 ch%d)", enable ? "on" : "off", (int)kNs4150PaPcaChannel);
}

void MossDesktopPreparePlayback(AudioCodec* codec) {
    if (codec == nullptr) {
        return;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
        // Brief settle for ES8311 DAC before PA enable reduces turn-on thump.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    MossDesktopSetNs4150Pa(true);
}

void MossDesktopReleasePlayback(AudioCodec* codec) {
    MossDesktopSetNs4150Pa(false);
    if (codec != nullptr && codec->output_enabled()) {
        codec->EnableOutput(false);
    }
}
#endif

AudioCodec::AudioCodec() {}

AudioCodec::~AudioCodec() {}

void AudioCodec::OutputData(std::vector<int16_t>& data) { Write(data.data(), data.size()); }

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    Settings settings("audio", false);
    output_volume_ = settings.GetInt("output_volume", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)",
                 output_volume_);
        output_volume_ = 10;
    }

    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);

    Settings settings("audio", true);
    settings.SetInt("output_volume", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
