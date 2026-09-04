#ifndef AUDIO_SERVICE_H
#define AUDIO_SERVICE_H

#include <memory>
#include <atomic>
#include <cstdint>
#include <deque>
#include <vector>
#include <condition_variable>
#include <chrono>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/event_groups.h>
#include <esp_timer.h>
#include <model_path.h>
#include "esp_audio_enc.h"
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include "audio_codec.h"
#include "audio_debugger.h"
#include "audio_engine.h"
#include "protocol.h"
#include "ogg_demuxer.h"

/*
 * There are two types of audio data flow:
 * 1. (MIC) -> [Audio Engine] -> {Encode Queue} -> [Opus Encoder] -> {Send Queue} -> (Server)
 * 2. (Server) -> {Decode Queue} -> [Opus Decoder] -> {Playback Queue} -> (Speaker)
 *
 * We use dedicated tasks for input, output, and Opus encoding/decoding.
 * 
 * Decode Queue and Send Queue are the main queues, because Opus packets are quite smaller than PCM packets.
 * 
 */

#define OPUS_FRAME_DURATION_MS 60
// Barge-in flushes ~1–2 s of PCM preroll after TTS decode stops. Size 2 dropped
// the onset while OpusCodecTask was busy decoding ("Encode queue is full").
#define MAX_ENCODE_TASKS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_PLAYBACK_TASKS_IN_QUEUE 2
#define MAX_DECODE_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define MAX_SEND_PACKETS_IN_QUEUE (2400 / OPUS_FRAME_DURATION_MS)
#define AUDIO_TESTING_MAX_DURATION_MS 10000
#define MAX_TIMESTAMPS_IN_QUEUE 3

#define AUDIO_POWER_TIMEOUT_MS 15000
#define AUDIO_POWER_CHECK_INTERVAL_MS 1000

#define AS_EVENT_AUDIO_TESTING_RUNNING      (1 << 0)
#define AS_EVENT_WAKE_WORD_RUNNING          (1 << 1)
#define AS_EVENT_AUDIO_PROCESSOR_RUNNING    (1 << 2)
#define AS_EVENT_AUDIO_INPUT_STOP_REQUEST   (1 << 4)

#define AS_OPUS_GET_FRAME_DRU_ENUM(duration_ms)                   \
    ((duration_ms) == 5 ? ESP_OPUS_ENC_FRAME_DURATION_5_MS :      \
     (duration_ms) == 10 ? ESP_OPUS_ENC_FRAME_DURATION_10_MS :    \
     (duration_ms) == 20 ? ESP_OPUS_ENC_FRAME_DURATION_20_MS :    \
     (duration_ms) == 40 ? ESP_OPUS_ENC_FRAME_DURATION_40_MS :    \
     (duration_ms) == 60 ? ESP_OPUS_ENC_FRAME_DURATION_60_MS :    \
     (duration_ms) == 80 ? ESP_OPUS_ENC_FRAME_DURATION_80_MS :    \
     (duration_ms) == 100 ? ESP_OPUS_ENC_FRAME_DURATION_100_MS :  \
     (duration_ms) == 120 ? ESP_OPUS_ENC_FRAME_DURATION_120_MS : -1)

#define AS_OPUS_ENC_CONFIG() {                                                                                    \
        .sample_rate        = ESP_AUDIO_SAMPLE_RATE_16K,                                                          \
        .channel            = ESP_AUDIO_MONO,                                                                     \
        .bits_per_sample    = ESP_AUDIO_BIT16,                                                                    \
        .bitrate            = ESP_OPUS_BITRATE_AUTO,                                                              \
        .frame_duration     = (esp_opus_enc_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(OPUS_FRAME_DURATION_MS),  \
        .application_mode   = ESP_OPUS_ENC_APPLICATION_AUDIO,                                                     \
        .complexity         = 0,                                                                                  \
        .enable_fec         = false,                                                                              \
        .enable_dtx         = false,                                                                               \
        .enable_vbr         = true,                                                                               \
    }

struct AudioServiceCallbacks {
    std::function<void(void)> on_send_queue_available;
    std::function<void(const std::string&)> on_wake_word_detected;
    std::function<void(bool)> on_vad_change;
    std::function<void(void)> on_audio_testing_queue_full;
    // Fired when the decode/playback queues and their in-flight work are drained.
    std::function<void(void)> on_playback_drained;
};


enum AudioTaskType {
    kAudioTaskTypeEncodeToSendQueue,
    kAudioTaskTypeEncodeToTestingQueue,
    kAudioTaskTypeDecodeToPlaybackQueue,
};

struct AudioTask {
    AudioTaskType type;
    std::vector<int16_t> pcm;
    uint32_t timestamp = 0;
};

struct DebugStatistics {
    uint32_t input_count = 0;
    uint32_t decode_count = 0;
    uint32_t encode_count = 0;
    uint32_t playback_count = 0;
    uint32_t encode_drop_count = 0;
};

class AudioService {
public:
    AudioService();
    ~AudioService();

    void Initialize(AudioCodec* codec);
    void Start();
    void Stop();
    void EncodeWakeWord();
    std::unique_ptr<AudioStreamPacket> PopWakeWordPacket();
    const std::string& GetLastWakeWord() const;
    bool IsVoiceDetected() const { return voice_detected_; }
    // True when post-AEC mic energy is clearly above the learned TTS echo floor.
    // Used so speaker leak cannot barge-in; VAD alone is not enough on loud PA boards.
    bool IsLikelyNearEndSpeech() const;
    // Enter-threshold only. Confirm uses IsSustainedNearEndSpeech (hysteresis);
    // the same high bar on the 220 ms snapshot missed real barge-in (log:
    // res=1140/need=1134, then ignored and never retried).
    bool IsConfirmedNearEndSpeech() const;
    bool IsSustainedNearEndSpeech() const;
    // Floor gate without the playback ratio. Mute the PA here so NLP stops
    // eating onset; confirm still waits for the ratio gate. Do not use raw
    // mic/ref: echo already has mic~3k/ref~28k and false-mutes TTS.
    bool ShouldEarlyMuteForBargeIn() const;
    bool EchoProfileReady() const;
    void ResetEchoProfile();
    // New TTS sentence: AEC residual tracks the louder onset for ~300 ms
    // (log: res=2093/pb=4023 aborted). Ignore near-end for one window only.
    // Do not key this off PCM energy jumps — speech always has those.
    // Do not reset speaking_started_us_ (that made list playback deaf).
    void NoteTtsSentenceStart();
    int PlaybackLevel() const;
    int ResidualLevel() const;
    int EchoFloor() const;
    int MicLevel() const;
    int RefLevel() const;
    bool IsIdle();
    bool IsPlaybackIdle();
    bool IsWakeWordRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_WAKE_WORD_RUNNING; }
    bool IsAudioProcessorRunning() const { return xEventGroupGetBits(event_group_) & AS_EVENT_AUDIO_PROCESSOR_RUNNING; }
    bool IsAfeWakeWord();
    std::vector<std::string> GetWakeWordPhrases() const;
    std::string GetWakeWordEngine() const;
    bool GetWakeWordConfig(std::vector<WakeWordCommandEntry>* entries, int* threshold_percent) const;
    bool ApplyWakeWordConfig(const std::vector<WakeWordCommandEntry>& entries, int threshold_percent);

    void EnableWakeWordDetection(bool enable);
    void EnableVoiceProcessing(bool enable);
    void EnableAudioTesting(bool enable);
    void EnableDeviceAec(bool enable);
    // TTS decode and mic encode share one Opus task. Hold encode while speaking
    // and keep PCM; flush on barge-in so the cloud gets a continuous utterance.
    void SetHoldUplinkEncode(bool hold);
    void ClearPcmPreroll();
    void FlushBargeInPcmToEncodeQueue();
    bool HasBargeInCapture() const;
    // Failed 220 ms confirm must drop the latch so a later near-end can fire
    // VAD again. Keep the PCM: clearing it cut the onset ("会些什么？").
    void ReleaseBargeInCapture();
    void GateUplinkForMs(int ms);
    // After TTS: drop residual until it goes quiet, then wait for new speech.
    // A wall-clock mute still encoded "上校" into "上你都会些什么？".
    void GateUplinkUntilEchoQuiet();

    void SetPlaybackMuted(bool muted);
    bool IsPlaybackMuted() const;
    // 256 = full. Used to ease NLP before the 20% confirm mute (not a hard gate).
    void SetPlaybackDuckQ8(int duck_q8);

    void SetCallbacks(AudioServiceCallbacks& callbacks);

    bool PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait = false);
    std::unique_ptr<AudioStreamPacket> PopPacketFromSendQueue();
    void PlaySound(const std::string_view& sound);
    bool ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples);
    void ResetDecoder();
    void SetModelsList(srmodel_list_t* models_list);

private:
    AudioCodec* codec_ = nullptr;
    AudioServiceCallbacks callbacks_;
    std::unique_ptr<AudioEngine> audio_engine_;
    std::unique_ptr<AudioDebugger> audio_debugger_;
    void* opus_encoder_ = nullptr;
    void* opus_decoder_ = nullptr;
    std::mutex decoder_mutex_;
    std::mutex input_resampler_mutex_;
    esp_ae_rate_cvt_handle_t input_resampler_ = nullptr;
    esp_ae_rate_cvt_handle_t output_resampler_ = nullptr;
    
    // Encoder/Decoder state
    int encoder_sample_rate_ = 16000;
    int encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int encoder_frame_size_ = 0;
    int encoder_outbuf_size_ = 0;
    int decoder_sample_rate_ = 0;
    int decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
    int decoder_frame_size_ = 0;
    DebugStatistics debug_statistics_;
    int64_t last_encode_drop_log_time_ = 0;
    srmodel_list_t* models_list_ = nullptr;

    EventGroupHandle_t event_group_;

    // Audio encode / decode
    TaskHandle_t audio_input_task_handle_ = nullptr;
    TaskHandle_t audio_output_task_handle_ = nullptr;
    TaskHandle_t opus_codec_task_handle_ = nullptr;
    std::mutex audio_queue_mutex_;
    std::condition_variable audio_queue_cv_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_decode_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_send_queue_;
    std::deque<std::unique_ptr<AudioStreamPacket>> audio_testing_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_encode_queue_;
    std::deque<std::unique_ptr<AudioTask>> audio_playback_queue_;
    bool decode_in_flight_ = false;
    bool output_in_flight_ = false;
    bool playback_drained_notified_ = true;
    uint32_t playback_generation_ = 0;
    // For server AEC
    std::deque<uint32_t> timestamp_queue_;

    bool audio_engine_initialized_ = false;
    bool voice_detected_ = false;
    static constexpr size_t kPcmPrerollFrames = 10;        // 600 ms before VAD (covers 你都)
    static constexpr size_t kBargeCaptureMaxFrames = 40;   // 2.4 s cap after VAD
    std::atomic<bool> hold_uplink_encode_{false};
    mutable std::mutex pcm_preroll_mutex_;
    std::deque<std::vector<int16_t>> pcm_preroll_;
    std::deque<std::vector<int16_t>> barge_capture_;
    bool barge_capture_active_ = false;
    void PushHeldPcmFrame(std::vector<int16_t>&& data);
    void StartBargeCaptureFromPrerollLocked();
    void EncodePrerollDroppingQuiet();
    bool IsNearEndSpeechWithMargin(int margin) const;
    std::atomic<int> playback_level_{0};
    std::atomic<int> residual_level_{0};
    std::atomic<int> echo_floor_{250};
    std::atomic<int> echo_mic_floor_{200};
    std::atomic<int> echo_playback_frames_{0};
    std::atomic<int> echo_converged_streak_{0};
    std::atomic<bool> echo_ready_{false};
    std::atomic<int> echo_sentence_guard_frames_{0};
    std::atomic<int64_t> uplink_gate_until_us_{0};
    enum class EchoTailGate : uint8_t { Off, WaitQuiet, WaitSpeech };
    std::atomic<EchoTailGate> echo_tail_gate_{EchoTailGate::Off};
    std::atomic<int> echo_quiet_frames_{0};
    std::atomic<int64_t> echo_tail_deadline_us_{0};
    std::atomic<int> echo_tail_start_residual_{0};
    mutable std::atomic<bool> near_end_latched_{false};
    std::atomic<int64_t> last_playback_us_{0};
    std::atomic<bool> playback_muted_{false};
    std::atomic<int> playback_duck_q8_{256};
    std::atomic<int> mic_level_{0};
    std::atomic<int> ref_level_{0};
#if CONFIG_USE_DEVICE_AEC
    bool device_aec_enabled_ = true;
#else
    bool device_aec_enabled_ = false;
#endif
    std::atomic<bool> service_stopped_{true};
    std::atomic<bool> audio_input_need_warmup_{false};

    esp_timer_handle_t audio_power_timer_ = nullptr;
    std::chrono::steady_clock::time_point last_input_time_;
    std::chrono::steady_clock::time_point last_output_time_;

    void AudioInputTask();
    void AudioOutputTask();
    void OpusCodecTask();
    void PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm);
    bool InitializeAudioEngine();
    void SetDecodeSampleRate(int sample_rate, int frame_duration);
    void CheckAndUpdateAudioPowerState();
    bool IsPlaybackDrainedLocked() const;
    bool MarkPlaybackDrainedLocked();
    static int PcmMeanAbs(const int16_t* data, size_t samples);
    int EffectivePlaybackLevel() const;
    void NotePlaybackPcm(const int16_t* data, size_t samples);
    void NoteResidualPcm(const int16_t* data, size_t samples);
    void NoteCapturePcm(const int16_t* data, size_t samples, int channels);
    void AdaptEchoFloor(int playback, int residual);
    bool ConsumeEchoTailGate(std::vector<int16_t>& data);
};

#endif
