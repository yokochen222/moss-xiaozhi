#include "audio_wave_plugin.h"

#if MOSS_AUDIO_WAVE_PLUGIN

#include "audio_pcm_tap.h"

#include "esp_timer.h"

#include <atomic>
#include <cstddef>

namespace moss_wave {

static constexpr int kWaveBins = 128;
static constexpr int kWaveHop = 192;
static constexpr int kWaveFull = 6000;
static constexpr int64_t kWaveFadeStartUs = 80000;
static constexpr int64_t kWaveFadeSpanUs = 200000;

struct WaveRing {
    uint8_t bins[kWaveBins]{};
    std::atomic<uint8_t> write{0};
    int acc_peak = 0;
    int acc_count = 0;
    std::atomic<int64_t> last_us{0};
};

static WaveRing s_mic;
static WaveRing s_spk;
static std::atomic<bool> s_attached{false};
static std::atomic<bool> s_playback{false};

static uint8_t MapPeak(int peak) {
    int v = (peak * 255) / kWaveFull;
    if (v > 255) {
        v = 255;
    }
    return static_cast<uint8_t>(v);
}

static void Push(WaveRing* ring, uint8_t level) {
    uint8_t w = ring->write.load(std::memory_order_relaxed);
    ring->bins[w] = level;
    ring->write.store(static_cast<uint8_t>((w + 1) & (kWaveBins - 1)), std::memory_order_release);
}

static void Feed(WaveRing* ring, const int16_t* data, size_t samples, int channels) {
    if (data == nullptr || samples == 0) {
        return;
    }
    if (channels < 1) {
        channels = 1;
    }
    size_t frames = samples / static_cast<size_t>(channels);
    if (frames == 0) {
        return;
    }

    int peak = ring->acc_peak;
    int count = ring->acc_count;
    for (size_t f = 0; f < frames; ++f) {
        int16_t s = data[f * static_cast<size_t>(channels)];
        int a = s < 0 ? -s : s;
        if (a > peak) {
            peak = a;
        }
        ++count;
        if (count >= kWaveHop) {
            Push(ring, MapPeak(peak));
            peak = 0;
            count = 0;
        }
    }
    ring->acc_peak = peak;
    ring->acc_count = count;
    ring->last_us.store(esp_timer_get_time(), std::memory_order_release);
}

void OnInputPcm(const int16_t* data, size_t samples, int channels) {
    if (!s_attached.load(std::memory_order_relaxed)) {
        return;
    }
    Feed(&s_mic, data, samples, channels);
}

void OnOutputPcm(const int16_t* data, size_t samples, int channels) {
    if (!s_attached.load(std::memory_order_relaxed)) {
        return;
    }
    Feed(&s_spk, data, samples, channels);
}

void Attach() { s_attached.store(true, std::memory_order_release); }

void Detach() { s_attached.store(false, std::memory_order_release); }

void SetPlaybackSource(bool playback) { s_playback.store(playback, std::memory_order_relaxed); }

bool CopyBins(uint8_t* dst, int n, int64_t now_us) {
    if (!s_attached.load(std::memory_order_acquire) || dst == nullptr || n <= 0) {
        return false;
    }
    if (n > kWaveBins) {
        n = kWaveBins;
    }

    WaveRing* ring = s_playback.load(std::memory_order_relaxed) ? &s_spk : &s_mic;
    uint8_t w = ring->write.load(std::memory_order_acquire);
    for (int i = 0; i < n; ++i) {
        dst[i] = ring->bins[(w + kWaveBins - n + i) & (kWaveBins - 1)];
    }

    int64_t last = ring->last_us.load(std::memory_order_acquire);
    int64_t age = (last > 0 && now_us > last) ? (now_us - last) : 0;
    if (age > kWaveFadeStartUs) {
        int scale = 255 - static_cast<int>((age - kWaveFadeStartUs) * 255 / kWaveFadeSpanUs);
        if (scale < 0) {
            scale = 0;
        }
        for (int i = 0; i < n; ++i) {
            dst[i] = static_cast<uint8_t>((dst[i] * scale) / 255);
        }
    }
    return true;
}

}  // namespace moss_wave

extern "C" void AudioPcmTap_OnInput(const int16_t* data, size_t samples, int channels) {
    moss_wave::OnInputPcm(data, samples, channels);
}

extern "C" void AudioPcmTap_OnOutput(const int16_t* data, size_t samples, int channels) {
    moss_wave::OnOutputPcm(data, samples, channels);
}

#endif  // MOSS_AUDIO_WAVE_PLUGIN
