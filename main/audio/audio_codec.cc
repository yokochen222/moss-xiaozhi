#include "audio_codec.h"
#include "audio_pcm_tap.h"
#include "board.h"
#include "settings.h"

#include <driver/i2s_common.h>
#include <esp_log.h>
#include <atomic>
#include <cstring>
#include "sdkconfig.h"

#if CONFIG_BOARD_TYPE_MOSS_OV2640
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "pca9685_driver.h"
#endif

extern "C" {
void __attribute__((weak)) AudioPcmTap_OnInput(const int16_t*, size_t, int) {}
void __attribute__((weak)) AudioPcmTap_OnOutput(const int16_t*, size_t, int) {}
}

#define TAG "AudioCodec"

#if CONFIG_BOARD_TYPE_MOSS_OV2640
namespace {

constexpr uint8_t kNs4150PaPcaChannel = 1;
bool ns4150_pa_enabled = false;
std::atomic<int> shared_i2c_hold{0};

}  // namespace

void MossDesktopHoldSharedI2c(bool hold) {
    if (hold) {
        shared_i2c_hold.fetch_add(1, std::memory_order_acq_rel);
        return;
    }
    int v = shared_i2c_hold.load(std::memory_order_relaxed);
    while (v > 0 && !shared_i2c_hold.compare_exchange_weak(v, v - 1, std::memory_order_acq_rel,
                                                           std::memory_order_relaxed)) {
    }
}

bool MossDesktopSharedI2cHeld() { return shared_i2c_hold.load(std::memory_order_acquire) > 0; }

void MossDesktopSetNs4150Pa(bool enable) {
    if (MossDesktopSharedI2cHeld()) {
        return;
    }
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
    if (MossDesktopSharedI2cHeld()) {
        return;
    }
    if (codec == nullptr) {
        return;
    }
    if (!codec->output_enabled()) {
        codec->EnableOutput(true);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    MossDesktopSetNs4150Pa(true);
}

void MossDesktopReleasePlayback(AudioCodec* codec) {
    if (MossDesktopSharedI2cHeld()) {
        return;
    }
    MossDesktopSetNs4150Pa(false);
    if (codec == nullptr || !codec->output_enabled()) {
        return;
    }
    // Duplex I2S: never close TX on this board. Mic-off during camera used to
    // EnableOutput(false) on the shared I2C bus and crash SCCB_Deinit.
    if (codec->duplex()) {
        return;
    }
    codec->EnableOutput(false);
}
#endif

AudioCodec::AudioCodec() {}

AudioCodec::~AudioCodec() {}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
    AudioPcmTap_OnOutput(data.data(), data.size(), output_channels_);
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        AudioPcmTap_OnInput(data.data(), static_cast<size_t>(samples), input_channels_);
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

void AudioCodec::PreparePlayback() {
    if (!output_enabled()) {
        EnableOutput(true);
    }
}
