#include "audio_service.h"
#include <esp_log.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include "audio_codec.h"
#include "sdkconfig.h"

#define RATE_CVT_CFG(_src_rate, _dest_rate, _channel)                                        \
    (esp_ae_rate_cvt_cfg_t) {                                                                \
        .src_rate = (uint32_t)(_src_rate), .dest_rate = (uint32_t)(_dest_rate),              \
        .channel = (uint8_t)(_channel), .bits_per_sample = ESP_AUDIO_BIT16, .complexity = 2, \
        .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,                                        \
    }

#define OPUS_DEC_CFG(_sample_rate, _frame_duration_ms)                                     \
    (esp_opus_dec_cfg_t) {                                                                 \
        .sample_rate = (uint32_t)(_sample_rate), .channel = ESP_AUDIO_MONO,                \
        .frame_duration =                                                                  \
            (esp_opus_dec_frame_duration_t)AS_OPUS_GET_FRAME_DRU_ENUM(_frame_duration_ms), \
        .self_delimited = false,                                                           \
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
#include "engines/afe_audio_engine.h"
#else
#include "engines/lite_audio_engine.h"
#endif

#define TAG "AudioService"

AudioService::AudioService() { event_group_ = xEventGroupCreate(); }

AudioService::~AudioService() {
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
    }
    if (opus_encoder_ != nullptr) {
        esp_opus_enc_close(opus_encoder_);
    }
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
    }
    if (input_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(input_resampler_);
    }
    if (output_resampler_ != nullptr) {
        esp_ae_rate_cvt_close(output_resampler_);
    }
}

void AudioService::Initialize(AudioCodec* codec) {
    codec_ = codec;
    codec_->Start();

    esp_opus_dec_cfg_t opus_dec_cfg =
        OPUS_DEC_CFG(codec->output_sample_rate(), OPUS_FRAME_DURATION_MS);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
    } else {
        decoder_sample_rate_ = codec->output_sample_rate();
        decoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        decoder_frame_size_ = decoder_sample_rate_ / 1000 * OPUS_FRAME_DURATION_MS;
    }
    esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
    ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t), &opus_encoder_);
    if (opus_encoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
    } else {
        encoder_sample_rate_ = 16000;
        encoder_duration_ms_ = OPUS_FRAME_DURATION_MS;
        esp_opus_enc_get_frame_size(opus_encoder_, &encoder_frame_size_, &encoder_outbuf_size_);
        encoder_frame_size_ = encoder_frame_size_ / sizeof(int16_t);
    }

    if (codec->input_sample_rate() != 16000) {
        esp_ae_rate_cvt_cfg_t input_resampler_cfg = RATE_CVT_CFG(
            codec->input_sample_rate(), ESP_AUDIO_SAMPLE_RATE_16K, codec->input_channels());
        auto resampler_ret = esp_ae_rate_cvt_open(&input_resampler_cfg, &input_resampler_);
        if (input_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create input resampler, error code: %d", resampler_ret);
        }
    }

#if CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32P4 || CONFIG_IDF_TARGET_ESP32S31
    audio_engine_ = std::make_unique<AfeAudioEngine>();
#else
    audio_engine_ = std::make_unique<LiteAudioEngine>();
#endif
    audio_engine_->OnOutput([this](std::vector<int16_t>&& data) {
        NoteResidualPcm(data.data(), data.size());
        if (speaking_capture_.load(std::memory_order_relaxed)) {
            MaybeAppendSpeakingCapture(data);
            // xiaozhi realtime: keep AEC-cleaned uplink flowing during TTS so
            // the cloud (and local VAD) can hear the user. Half-duplex holds.
            if (!device_aec_enabled_) {
                return;
            }
        }
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue, std::move(data));
    });
    audio_engine_->OnVadStateChange([this](bool speaking) {
        voice_detected_ = speaking;
        if (callbacks_.on_vad_change) {
            callbacks_.on_vad_change(speaking);
        }
    });
    audio_engine_->OnWakeWordDetected([this](const std::string& wake_word) {
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
        if (callbacks_.on_wake_word_detected) {
            callbacks_.on_wake_word_detected(wake_word);
        }
    });

    esp_timer_create_args_t audio_power_timer_args = {
        .callback =
            [](void* arg) {
                AudioService* audio_service = (AudioService*)arg;
                audio_service->CheckAndUpdateAudioPowerState();
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "audio_power_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&audio_power_timer_args, &audio_power_timer_);
}

void AudioService::Start() {
    service_stopped_.store(false);
    xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                           AS_EVENT_AUDIO_PROCESSOR_RUNNING |
                                           AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

    esp_timer_start_periodic(audio_power_timer_, 1000000);

#if CONFIG_USE_AUDIO_PROCESSOR
    /* Start the audio input task */
    xTaskCreatePinnedToCore(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 3, this, 8, &audio_input_task_handle_, 0);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048 * 2, this, 4, &audio_output_task_handle_);
#else
    /* Start the audio input task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioInputTask();
            vTaskDelete(NULL);
        },
        "audio_input", 2048 * 2, this, 8, &audio_input_task_handle_);

    /* Start the audio output task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->AudioOutputTask();
            vTaskDelete(NULL);
        },
        "audio_output", 2048, this, 4, &audio_output_task_handle_);
#endif

    /* Start the opus codec task */
    xTaskCreate(
        [](void* arg) {
            AudioService* audio_service = (AudioService*)arg;
            audio_service->OpusCodecTask();
            vTaskDelete(NULL);
        },
        "opus_codec", 2048 * 12, this, 2, &opus_codec_task_handle_);
}

void AudioService::Stop() {
    esp_timer_stop(audio_power_timer_);
    service_stopped_.store(true);
    xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING | AS_EVENT_WAKE_WORD_RUNNING |
                                         AS_EVENT_AUDIO_PROCESSOR_RUNNING);

    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        audio_encode_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
#if CONFIG_BOARD_TYPE_MOSS_OV2640
    MossDesktopReleasePlayback(codec_);
#endif
}

bool AudioService::ReadAudioData(std::vector<int16_t>& data, int sample_rate, int samples) {
    if (!codec_->input_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        codec_->EnableInput(true);
    }

    if (codec_->input_sample_rate() != sample_rate) {
        data.resize(samples * codec_->input_sample_rate() / sample_rate * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
        if (input_resampler_ != nullptr) {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            uint32_t in_sample_num = data.size() / codec_->input_channels();
            uint32_t output_samples = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(input_resampler_, in_sample_num,
                                                   &output_samples);
            auto resampled = std::vector<int16_t>(output_samples * codec_->input_channels());
            uint32_t actual_output = output_samples;
            esp_ae_rate_cvt_process(input_resampler_, (esp_ae_sample_t)data.data(), in_sample_num,
                                    (esp_ae_sample_t)resampled.data(), &actual_output);
            resampled.resize(actual_output * codec_->input_channels());
            data = std::move(resampled);
        }
    } else {
        data.resize(samples * codec_->input_channels());
        if (!codec_->InputData(data)) {
            return false;
        }
    }

    NoteCapturePcm(data.data(), data.size(), codec_->input_channels());

    /* Update the last input time */
    last_input_time_ = std::chrono::steady_clock::now();
    debug_statistics_.input_count++;

#if CONFIG_USE_AUDIO_DEBUGGER
    // 音频调试：发送原始音频数据
    if (audio_debugger_ == nullptr) {
        audio_debugger_ = std::make_unique<AudioDebugger>();
    }
    audio_debugger_->Feed(data);
#endif

    return true;
}

void AudioService::AudioInputTask() {
    constexpr EventBits_t kAudioInputActiveBits = AS_EVENT_AUDIO_TESTING_RUNNING |
                                                  AS_EVENT_WAKE_WORD_RUNNING |
                                                  AS_EVENT_AUDIO_PROCESSOR_RUNNING;

    while (true) {
        EventBits_t bits = xEventGroupWaitBits(
            event_group_, kAudioInputActiveBits | AS_EVENT_AUDIO_INPUT_STOP_REQUEST, pdFALSE,
            pdFALSE, portMAX_DELAY);

        if (service_stopped_.load()) {
            // ADC continuous mode keeps its hardware mutex from start until stop,
            // so the input task that started it must also stop it before exiting.
            if (codec_->input_enabled()) {
                codec_->EnableInput(false);
            }
            break;
        }

        if (bits & AS_EVENT_AUDIO_INPUT_STOP_REQUEST) {
            xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);

            // Recheck the active state in this task. Audio capture may have been
            // enabled after the timer posted the stop request.
            bits = xEventGroupGetBits(event_group_);
            if ((bits & kAudioInputActiveBits) == 0) {
                if (codec_->input_enabled()) {
                    codec_->EnableInput(false);
                }
                // Do not process the stale active bits returned by waitBits().
                continue;
            }
        }

        if (audio_input_need_warmup_.exchange(false)) {
            vTaskDelay(pdMS_TO_TICKS(120));
            continue;
        }

        /* Used for audio testing in NetworkConfiguring mode by clicking the BOOT button */
        if (bits & AS_EVENT_AUDIO_TESTING_RUNNING) {
            if (audio_testing_queue_.size() >=
                AUDIO_TESTING_MAX_DURATION_MS / OPUS_FRAME_DURATION_MS) {
                ESP_LOGW(TAG, "Audio testing queue is full, stopping audio testing");
                EnableAudioTesting(false);
                continue;
            }
            std::vector<int16_t> data;
            int samples = OPUS_FRAME_DURATION_MS * 16000 / 1000;
            if (ReadAudioData(data, 16000, samples)) {
                // If input channels is 2, we need to fetch the left channel data
                if (codec_->input_channels() == 2) {
                    auto mono_data = std::vector<int16_t>(data.size() / 2);
                    for (size_t i = 0, j = 0; i < mono_data.size(); ++i, j += 2) {
                        mono_data[i] = data[j];
                    }
                    data = std::move(mono_data);
                }
                PushTaskToEncodeQueue(kAudioTaskTypeEncodeToTestingQueue, std::move(data));
                continue;
            }
        }

        /* Feed the selected audio engine */
        if (bits & (AS_EVENT_WAKE_WORD_RUNNING | AS_EVENT_AUDIO_PROCESSOR_RUNNING)) {
            int samples = 160;  // 10ms
            std::vector<int16_t> data;
            if (ReadAudioData(data, 16000, samples)) {
                audio_engine_->Feed(std::move(data));
                continue;
            }
        }

        // Read timeout/error should not terminate the input task.
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    ESP_LOGW(TAG, "Audio input task stopped");
}

void AudioService::AudioOutputTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(
            lock, [this]() { return !audio_playback_queue_.empty() || service_stopped_.load(); });
        if (service_stopped_.load()) {
            break;
        }

        auto task = std::move(audio_playback_queue_.front());
        audio_playback_queue_.pop_front();
        output_in_flight_ = true;
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (!codec_->output_enabled()) {
            esp_timer_stop(audio_power_timer_);
            esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
        }
#if CONFIG_BOARD_TYPE_MOSS_OV2640
        // PA is independent of I2S on this board: idle listening keeps TX for
        // wake but turns the amp off. Always unmute before PCM, even if TX is up.
        MossDesktopPreparePlayback(codec_);
#else
        codec_->PreparePlayback();
#endif

        if (playback_muted_.load(std::memory_order_relaxed)) {
            std::fill(task->pcm.begin(), task->pcm.end(), 0);
        } else {
            const int duck = playback_duck_q8_.load(std::memory_order_relaxed);
            if (duck > 0 && duck < 256) {
                for (int16_t& s : task->pcm) {
                    s = static_cast<int16_t>((static_cast<int>(s) * duck) >> 8);
                }
            }
        }
        NotePlaybackPcm(task->pcm.data(), task->pcm.size());
        codec_->OutputData(task->pcm);

        /* Update the last output time */
        last_output_time_ = std::chrono::steady_clock::now();
        debug_statistics_.playback_count++;

        bool notify_drained = false;
        lock.lock();
#if CONFIG_USE_SERVER_AEC
        /* Record the timestamp for server AEC */
        if (task->timestamp > 0) {
            timestamp_queue_.push_back(task->timestamp);
        }
#endif
        output_in_flight_ = false;
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
        lock.unlock();

        if (notify_drained && callbacks_.on_playback_drained) {
            callbacks_.on_playback_drained();
        }
    }

    ESP_LOGW(TAG, "Audio output task stopped");
}

void AudioService::OpusCodecTask() {
    while (true) {
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);
        audio_queue_cv_.wait(lock, [this]() {
            return service_stopped_.load() || !audio_encode_queue_.empty() ||
                   (!audio_decode_queue_.empty() &&
                    audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE);
        });
        if (service_stopped_.load()) {
            break;
        }

        /* Decode the audio from decode queue */
        if (!audio_decode_queue_.empty() &&
            audio_playback_queue_.size() < MAX_PLAYBACK_TASKS_IN_QUEUE) {
            auto packet = std::move(audio_decode_queue_.front());
            audio_decode_queue_.pop_front();
            decode_in_flight_ = true;
            const uint32_t generation = playback_generation_;
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto task = std::make_unique<AudioTask>();
            task->type = kAudioTaskTypeDecodeToPlaybackQueue;
            task->timestamp = packet->timestamp;

            SetDecodeSampleRate(packet->sample_rate, packet->frame_duration);
            bool decoded = false;
            if (opus_decoder_ != nullptr) {
                task->pcm.resize(decoder_frame_size_);
                esp_audio_dec_in_raw_t raw = {
                    .buffer = (uint8_t*)(packet->payload.data()),
                    .len = (uint32_t)(packet->payload.size()),
                    .consumed = 0,
                    .frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE,
                };
                esp_audio_dec_out_frame_t out_frame = {
                    .buffer = (uint8_t*)(task->pcm.data()),
                    .len = (uint32_t)(task->pcm.size() * sizeof(int16_t)),
                    .decoded_size = 0,
                };
                esp_audio_dec_info_t dec_info = {};
                std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
                auto ret = esp_opus_dec_decode(opus_decoder_, &raw, &out_frame, &dec_info);
                decoder_lock.unlock();
                if (ret == ESP_AUDIO_ERR_OK) {
                    task->pcm.resize(out_frame.decoded_size / sizeof(int16_t));
                    if (decoder_sample_rate_ != codec_->output_sample_rate() &&
                        output_resampler_ != nullptr) {
                        uint32_t target_size = 0;
                        esp_ae_rate_cvt_get_max_out_sample_num(output_resampler_, task->pcm.size(),
                                                               &target_size);
                        std::vector<int16_t> resampled(target_size);
                        uint32_t actual_output = target_size;
                        esp_ae_rate_cvt_process(output_resampler_,
                                                (esp_ae_sample_t)task->pcm.data(), task->pcm.size(),
                                                (esp_ae_sample_t)resampled.data(), &actual_output);
                        resampled.resize(actual_output);
                        task->pcm = std::move(resampled);
                    }
                    decoded = true;
                } else {
                    ESP_LOGE(TAG, "Failed to decode audio after resize, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG, "Audio decoder is not configured");
            }

            lock.lock();
            if (decoded && generation == playback_generation_ && !service_stopped_.load()) {
                audio_playback_queue_.push_back(std::move(task));
            }
            decode_in_flight_ = false;
            debug_statistics_.decode_count++;
            const bool notify_drained = MarkPlaybackDrainedLocked();
            audio_queue_cv_.notify_all();
            lock.unlock();
            if (notify_drained && callbacks_.on_playback_drained) {
                callbacks_.on_playback_drained();
            }
            lock.lock();
        }
        /* Encode the audio to send queue */
        if (!audio_encode_queue_.empty()) {
            auto task = std::move(audio_encode_queue_.front());
            audio_encode_queue_.pop_front();
            audio_queue_cv_.notify_all();
            lock.unlock();

            auto packet = std::make_unique<AudioStreamPacket>();
            packet->frame_duration = OPUS_FRAME_DURATION_MS;
            packet->sample_rate = 16000;
            packet->timestamp = task->timestamp;
            // Per-frame energy, not the global EMA (that lags by later frames
            // and makes preroll trim cut "云台"/"你会").
            packet->residual = PcmMeanAbs(task->pcm.data(), task->pcm.size());

            if (opus_encoder_ != nullptr && task->pcm.size() == encoder_frame_size_) {
                std::vector<uint8_t> buf(encoder_outbuf_size_);
                esp_audio_enc_in_frame_t in = {
                    .buffer = (uint8_t*)(task->pcm.data()),
                    .len = (uint32_t)(encoder_frame_size_ * sizeof(int16_t)),
                };
                esp_audio_enc_out_frame_t out = {
                    .buffer = buf.data(),
                    .len = (uint32_t)encoder_outbuf_size_,
                    .encoded_bytes = 0,
                };
                auto ret = esp_opus_enc_process(opus_encoder_, &in, &out);
                if (ret == ESP_AUDIO_ERR_OK) {
                    packet->payload.assign(buf.data(), buf.data() + out.encoded_bytes);

                    if (task->type == kAudioTaskTypeEncodeToSendQueue) {
                        {
                            std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                            /* Never let a full send queue stall encoding: stale realtime
                             * audio is useless to the server, so drop the oldest packet. */
                            if (audio_send_queue_.size() >= MAX_SEND_PACKETS_IN_QUEUE) {
                                audio_send_queue_.pop_front();
                            }
                            audio_send_queue_.push_back(std::move(packet));
                        }
                        if (callbacks_.on_send_queue_available) {
                            callbacks_.on_send_queue_available();
                        }
                    } else if (task->type == kAudioTaskTypeEncodeToTestingQueue) {
                        std::lock_guard<std::mutex> lock2(audio_queue_mutex_);
                        audio_testing_queue_.push_back(std::move(packet));
                    }
                    debug_statistics_.encode_count++;
                } else {
                    ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                }
            } else {
                ESP_LOGE(TAG,
                         "Failed to encode audio: encoder not configured or invalid frame size "
                         "(got %u, expected %u)",
                         task->pcm.size(), encoder_frame_size_);
            }
            lock.lock();
        }
    }

    ESP_LOGW(TAG, "Opus codec task stopped");
}

void AudioService::SetDecodeSampleRate(int sample_rate, int frame_duration) {
    if (decoder_sample_rate_ == sample_rate && decoder_duration_ms_ == frame_duration) {
        return;
    }
    std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
    if (opus_decoder_ != nullptr) {
        esp_opus_dec_close(opus_decoder_);
        opus_decoder_ = nullptr;
    }
    decoder_lock.unlock();
    esp_opus_dec_cfg_t opus_dec_cfg = OPUS_DEC_CFG(sample_rate, frame_duration);
    auto ret = esp_opus_dec_open(&opus_dec_cfg, sizeof(esp_opus_dec_cfg_t), &opus_decoder_);
    if (opus_decoder_ == nullptr) {
        ESP_LOGE(TAG, "Failed to create audio decoder, error code: %d", ret);
        return;
    }
    decoder_sample_rate_ = sample_rate;
    decoder_duration_ms_ = frame_duration;
    decoder_frame_size_ = decoder_sample_rate_ / 1000 * frame_duration;

    auto codec = Board::GetInstance().GetAudioCodec();
    if (decoder_sample_rate_ != codec->output_sample_rate()) {
        ESP_LOGI(TAG, "Resampling audio from %d to %d", decoder_sample_rate_,
                 codec->output_sample_rate());
        if (output_resampler_ != nullptr) {
            esp_ae_rate_cvt_close(output_resampler_);
            output_resampler_ = nullptr;
        }
        esp_ae_rate_cvt_cfg_t output_resampler_cfg =
            RATE_CVT_CFG(decoder_sample_rate_, codec->output_sample_rate(), ESP_AUDIO_MONO);
        auto resampler_ret = esp_ae_rate_cvt_open(&output_resampler_cfg, &output_resampler_);
        if (output_resampler_ == nullptr) {
            ESP_LOGE(TAG, "Failed to create output resampler, error code: %d", resampler_ret);
        }
    }
}

void AudioService::PushTaskToEncodeQueue(AudioTaskType type, std::vector<int16_t>&& pcm) {
    auto task = std::make_unique<AudioTask>();
    task->type = type;
    task->pcm = std::move(pcm);

    uint32_t dropped_total = 0;
    {
        /* Push the task to the encode queue */
        std::unique_lock<std::mutex> lock(audio_queue_mutex_);

        /* If the task is to send queue, we need to set the timestamp */
        if (type == kAudioTaskTypeEncodeToSendQueue && !timestamp_queue_.empty()) {
            if (timestamp_queue_.size() <= MAX_TIMESTAMPS_IN_QUEUE) {
                task->timestamp = timestamp_queue_.front();
            } else {
                ESP_LOGW(TAG, "Timestamp queue (%u) is full, dropping timestamp",
                         timestamp_queue_.size());
            }
            timestamp_queue_.pop_front();
        }

        /* Microphone audio is realtime, so drop the oldest frame instead of blocking.
         * Blocking here would stall the audio engine task (AFE fetch) and deadlock the
         * whole input pipeline when the send queue stops being drained (e.g. network
         * congestion or a failed UDP send). */
        if (audio_encode_queue_.size() >= MAX_ENCODE_TASKS_IN_QUEUE) {
            audio_encode_queue_.pop_front();
            dropped_total = ++debug_statistics_.encode_drop_count;
        }
        audio_encode_queue_.push_back(std::move(task));
        audio_queue_cv_.notify_all();
    }

    /* Log outside the lock (UART writes are slow and would starve the codec task),
     * at most once per second. */
    if (dropped_total > 0) {
        int64_t now = esp_timer_get_time();
        if (now - last_encode_drop_log_time_ >= 1000000) {
            last_encode_drop_log_time_ = now;
            ESP_LOGW(TAG, "Encode queue is full, dropping oldest frame (dropped %lu so far)",
                     (unsigned long)dropped_total);
        }
    }
}

bool AudioService::PushPacketToDecodeQueue(std::unique_ptr<AudioStreamPacket> packet, bool wait) {
    std::unique_lock<std::mutex> lock(audio_queue_mutex_);
    if (audio_decode_queue_.size() >= MAX_DECODE_PACKETS_IN_QUEUE) {
        if (wait) {
            audio_queue_cv_.wait(lock, [this]() {
                return service_stopped_.load() ||
                       audio_decode_queue_.size() < MAX_DECODE_PACKETS_IN_QUEUE;
            });
        } else {
            return false;
        }
    }
    if (service_stopped_.load()) {
        return false;
    }
    playback_drained_notified_ = false;
    audio_decode_queue_.push_back(std::move(packet));
    audio_queue_cv_.notify_all();
    return true;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopPacketFromSendQueue() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    if (audio_send_queue_.empty()) {
        return nullptr;
    }
    auto packet = std::move(audio_send_queue_.front());
    audio_send_queue_.pop_front();
    audio_queue_cv_.notify_all();
    return packet;
}

void AudioService::EncodeWakeWord() {
    if (audio_engine_) {
        audio_engine_->EncodeWakeWordData();
    }
}

const std::string& AudioService::GetLastWakeWord() const {
    static const std::string empty;
    return audio_engine_ ? audio_engine_->GetLastDetectedWakeWord() : empty;
}

std::unique_ptr<AudioStreamPacket> AudioService::PopWakeWordPacket() {
    auto packet = std::make_unique<AudioStreamPacket>();
    if (audio_engine_ && audio_engine_->GetWakeWordOpus(packet->payload)) {
        return packet;
    }
    return nullptr;
}

void AudioService::EnableWakeWordDetection(bool enable) {
    ESP_LOGD(TAG, "%s wake word detection", enable ? "Enabling" : "Disabling");
    if (enable) {
        if (!InitializeAudioEngine() || !audio_engine_->HasWakeWord()) {
            xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
            return;
        }
        {
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableWakeWordDetection(true);
        xEventGroupSetBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableWakeWordDetection(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_WAKE_WORD_RUNNING);
    }
}

void AudioService::EnableVoiceProcessing(bool enable) {
    ESP_LOGD(TAG, "%s voice processing", enable ? "Enabling" : "Disabling");

    if (enable) {
        if (!InitializeAudioEngine()) {
            return;
        }
        // Already capturing (realtime AEC / wake-word): do not sleep 120ms or
        // reset the resampler. That overflows I2S and drops the first utterance
        // after wake and the onset of a barge-in.
        if (IsAudioProcessorRunning()) {
            audio_engine_->EnableVoiceProcessing(true);
            return;
        }
        // Wake greeting TTS can queue while still connecting. ResetDecoder here
        // would drop it the same way tts/start used to.
        if (IsPlaybackIdle()) {
            ResetDecoder();
        }
        if (!IsWakeWordRunning()) {
            audio_input_need_warmup_ = true;
            std::lock_guard<std::mutex> lock(input_resampler_mutex_);
            if (input_resampler_ != nullptr) {
                esp_ae_rate_cvt_reset(input_resampler_);
            }
        }
        audio_engine_->EnableVoiceProcessing(true);
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    } else {
        if (audio_engine_initialized_) {
            audio_engine_->EnableVoiceProcessing(false);
        }
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_PROCESSOR_RUNNING);
    }
}

void AudioService::EnableAudioTesting(bool enable) {
    ESP_LOGI(TAG, "%s audio testing", enable ? "Enabling" : "Disabling");
    if (enable) {
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
    } else {
        xEventGroupClearBits(event_group_, AS_EVENT_AUDIO_TESTING_RUNNING);
        /* Copy audio_testing_queue_ to audio_decode_queue_ */
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        audio_decode_queue_ = std::move(audio_testing_queue_);
        if (!audio_decode_queue_.empty()) {
            playback_drained_notified_ = false;
        }
        audio_queue_cv_.notify_all();
    }
}

namespace {
constexpr int kEchoFloorInit = 250;
constexpr int kQuietPlayback = 45;
constexpr int kMinNearEndAbs = 200;
constexpr int kPauseNearEndAbs = 260;
constexpr int kMinResidualPctOfPlayback = 24;
constexpr int kPlaybackStaleUs = 300 * 1000;
constexpr int kEchoLearnFrames = 4;  // ~240ms of real TTS, not pre-roll silence
}  // namespace

int AudioService::PcmMeanAbs(const int16_t* data, size_t samples) {
    if (data == nullptr || samples == 0) {
        return 0;
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < samples; ++i) {
        int v = data[i];
        sum += static_cast<uint64_t>(v < 0 ? -v : v);
    }
    return static_cast<int>(sum / samples);
}

int AudioService::EffectivePlaybackLevel() const {
    const int stored = playback_level_.load(std::memory_order_relaxed);
    if (speaking_capture_.load(std::memory_order_relaxed)) {
        // Packet gaps must not look like "speaker silent" or TTS leak
        // takes the quiet-pause gate and aborts into a self-reply loop.
        return stored;
    }
    const int64_t last = last_playback_us_.load(std::memory_order_relaxed);
    if (last <= 0) {
        return 0;
    }
    const int64_t now = esp_timer_get_time();
    if (now - last > kPlaybackStaleUs) {
        return 0;
    }
    return stored;
}

int AudioService::PlaybackLevel() const { return EffectivePlaybackLevel(); }

int AudioService::EchoFloor() const { return echo_floor_.load(std::memory_order_relaxed); }

void AudioService::BeginSpeakingCapture() {
    speaking_capture_.store(true, std::memory_order_relaxed);
    capture_latched_.store(false, std::memory_order_relaxed);
    near_end_latched_.store(false, std::memory_order_relaxed);
    if (echo_floor_.load(std::memory_order_relaxed) <= kEchoFloorInit) {
        echo_learn_frames_.store(kEchoLearnFrames, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(capture_mutex_);
    speaking_capture_pcm_.clear();
    speaking_capture_pcm_.reserve(16000 * 2);
}

void AudioService::EndSpeakingCapture() {
    speaking_capture_.store(false, std::memory_order_relaxed);
    capture_latched_.store(false, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(capture_mutex_);
    speaking_capture_pcm_.clear();
}

bool AudioService::HasNearEndCapture() const {
    return speaking_capture_.load(std::memory_order_relaxed) &&
           capture_latched_.load(std::memory_order_relaxed);
}

void AudioService::MaybeAppendSpeakingCapture(const std::vector<int16_t>& pcm) {
    if (pcm.empty()) {
        return;
    }
    const bool near = IsLikelyNearEndSpeech() || IsConfirmedNearEndSpeech();
    if (near) {
        capture_latched_.store(true, std::memory_order_relaxed);
    } else if (!capture_latched_.load(std::memory_order_relaxed)) {
        return;
    }
    constexpr size_t kMaxSamples = 16000 * 2;
    std::lock_guard<std::mutex> lock(capture_mutex_);
    speaking_capture_pcm_.insert(speaking_capture_pcm_.end(), pcm.begin(), pcm.end());
    if (speaking_capture_pcm_.size() > kMaxSamples) {
        const size_t extra = speaking_capture_pcm_.size() - kMaxSamples;
        speaking_capture_pcm_.erase(speaking_capture_pcm_.begin(),
                                    speaking_capture_pcm_.begin() + static_cast<std::ptrdiff_t>(extra));
    }
}

void AudioService::FlushSpeakingCaptureToSendQueue() {
    const bool latched = capture_latched_.exchange(false, std::memory_order_relaxed);
    speaking_capture_.store(false, std::memory_order_relaxed);
    std::vector<int16_t> pcm;
    {
        std::lock_guard<std::mutex> lock(capture_mutex_);
        pcm.swap(speaking_capture_pcm_);
    }
    if (!latched || pcm.empty() || encoder_frame_size_ <= 0) {
        return;
    }
    const size_t frame = static_cast<size_t>(encoder_frame_size_);
    if (pcm.size() % frame != 0) {
        pcm.resize((pcm.size() / frame + 1) * frame, 0);
    }
    for (size_t off = 0; off + frame <= pcm.size(); off += frame) {
        PushTaskToEncodeQueue(kAudioTaskTypeEncodeToSendQueue,
                              std::vector<int16_t>(pcm.begin() + static_cast<std::ptrdiff_t>(off),
                                                   pcm.begin() + static_cast<std::ptrdiff_t>(off + frame)));
    }
}

int AudioService::ResidualLevel() const { return residual_level_.load(std::memory_order_relaxed); }

int AudioService::MicLevel() const { return mic_level_.load(std::memory_order_relaxed); }

int AudioService::RefLevel() const { return ref_level_.load(std::memory_order_relaxed); }

void AudioService::ResetEchoProfile() {
    playback_level_.store(0, std::memory_order_relaxed);
    residual_level_.store(0, std::memory_order_relaxed);
    echo_floor_.store(kEchoFloorInit, std::memory_order_relaxed);
    echo_learn_frames_.store(kEchoLearnFrames, std::memory_order_relaxed);
    near_end_latched_.store(false, std::memory_order_relaxed);
    last_playback_us_.store(0, std::memory_order_relaxed);
    mic_level_.store(0, std::memory_order_relaxed);
    ref_level_.store(0, std::memory_order_relaxed);
}

void AudioService::NotePlaybackPcm(const int16_t* data, size_t samples) {
    // Keep the timestamp alive while muted so the 300ms stale window does not
    // treat a muted PA as "playback stopped" (quiet-path false barge-in).
    last_playback_us_.store(esp_timer_get_time(), std::memory_order_relaxed);
    if (data == nullptr || samples == 0) {
        return;
    }
    const int level = PcmMeanAbs(data, samples);
    const int prev = playback_level_.load(std::memory_order_relaxed);
    playback_level_.store((prev * 3 + level) / 4, std::memory_order_relaxed);
}

void AudioService::NoteCapturePcm(const int16_t* data, size_t samples, int channels) {
    if (data == nullptr || samples == 0 || channels < 2) {
        return;
    }
    const size_t frames = samples / static_cast<size_t>(channels);
    if (frames == 0) {
        return;
    }
    uint64_t mic_sum = 0;
    uint64_t ref_sum = 0;
    for (size_t i = 0; i < frames; ++i) {
        const size_t base = i * static_cast<size_t>(channels);
        int mic = data[base];
        int ref = data[base + 1];
        mic_sum += static_cast<uint64_t>(mic < 0 ? -mic : mic);
        ref_sum += static_cast<uint64_t>(ref < 0 ? -ref : ref);
    }
    const int mic = static_cast<int>(mic_sum / frames);
    const int ref = static_cast<int>(ref_sum / frames);
    const int prev_mic = mic_level_.load(std::memory_order_relaxed);
    const int prev_ref = ref_level_.load(std::memory_order_relaxed);
    mic_level_.store((prev_mic * 2 + mic) / 3, std::memory_order_relaxed);
    ref_level_.store((prev_ref * 2 + ref) / 3, std::memory_order_relaxed);
}

void AudioService::AdaptEchoFloor(int playback, int residual) {
    if (playback < kQuietPlayback) {
        return;
    }
    int floor = echo_floor_.load(std::memory_order_relaxed);
    int learn = echo_learn_frames_.load(std::memory_order_relaxed);
    if (learn > 0) {
        // Pre-TTS "speaking" state has leftover pb~190. Do not finish learning
        // until the speaker is actually playing.
        if (playback < 250) {
            return;
        }
        echo_learn_frames_.store(learn - 1, std::memory_order_relaxed);
        if (residual > floor) {
            floor = residual;
        }
        echo_floor_.store(std::max(floor, kEchoFloorInit), std::memory_order_relaxed);
        if (learn == 1) {
            ESP_LOGI(TAG, "AEC echo floor learned floor=%d pb=%d res=%d mic=%d ref=%d",
                     echo_floor_.load(std::memory_order_relaxed), playback, residual,
                     mic_level_.load(std::memory_order_relaxed),
                     ref_level_.load(std::memory_order_relaxed));
        }
        return;
    }
    if (speaking_capture_.load(std::memory_order_relaxed)) {
        // TTS residual is the echo floor. Skipping "2x spikes" left floor=250
        // while res=1200, so VAD aborted on the speaker ("超级人工智能").
        if (residual > floor) {
            floor = (floor + residual) / 2;
        } else {
            floor = (floor * 7 + residual) / 8;
        }
        echo_floor_.store(std::max(floor, kEchoFloorInit), std::memory_order_relaxed);
        return;
    }
    if (residual > floor + floor) {
        return;
    }
    if (residual >= floor) {
        floor = (floor * 7 + residual) / 8;
    } else {
        floor = (floor * 3 + residual) / 4;
    }
    if (floor < 40) {
        floor = 40;
    }
    echo_floor_.store(floor, std::memory_order_relaxed);
}

void AudioService::NoteResidualPcm(const int16_t* data, size_t samples) {
    const int level = PcmMeanAbs(data, samples);
    const int prev = residual_level_.load(std::memory_order_relaxed);
    const int ema = (prev * 2 + level) / 3;
    residual_level_.store(ema, std::memory_order_relaxed);
    AdaptEchoFloor(EffectivePlaybackLevel(), ema);
}

bool AudioService::EchoProfileReady() const {
    return echo_learn_frames_.load(std::memory_order_relaxed) <= 0;
}

bool AudioService::IsConfirmedNearEndSpeech() const {
    const int residual = residual_level_.load(std::memory_order_relaxed);
    if (residual < kMinNearEndAbs) {
        return false;
    }
    const int playback = EffectivePlaybackLevel();
    const bool tts_hold = speaking_capture_.load(std::memory_order_relaxed);
    if (tts_hold && !EchoProfileReady()) {
        return false;
    }
    if (playback < kQuietPlayback) {
        // During TTS a low playback reading is a measurement gap, not a pause.
        if (tts_hold) {
            return false;
        }
        return residual >= kPauseNearEndAbs;
    }
    const int floor = echo_floor_.load(std::memory_order_relaxed);
    const int need = floor + std::max(floor / 3, 80);
    if (residual < need) {
        return false;
    }
    return (int64_t)residual * 100 >= (int64_t)playback * kMinResidualPctOfPlayback;
}

bool AudioService::ShouldEarlyMuteForBargeIn() const {
    const int residual = residual_level_.load(std::memory_order_relaxed);
    if (residual < kMinNearEndAbs) {
        return false;
    }
    const int playback = EffectivePlaybackLevel();
    if (playback < kQuietPlayback) {
        return false;
    }
    const int floor = echo_floor_.load(std::memory_order_relaxed);
    const int need = floor + std::max(floor / 3, 80);
    return residual >= need;
}

bool AudioService::IsLikelyNearEndSpeech() const {
    const int residual = residual_level_.load(std::memory_order_relaxed);
    if (residual < kMinNearEndAbs) {
        near_end_latched_.store(false, std::memory_order_relaxed);
        return false;
    }
    const int playback = EffectivePlaybackLevel();
    if (speaking_capture_.load(std::memory_order_relaxed) && !EchoProfileReady()) {
        near_end_latched_.store(false, std::memory_order_relaxed);
        return false;
    }
    if (playback < kQuietPlayback) {
        if (speaking_capture_.load(std::memory_order_relaxed)) {
            near_end_latched_.store(false, std::memory_order_relaxed);
            return false;
        }
        const bool hit = residual >= kPauseNearEndAbs;
        near_end_latched_.store(hit, std::memory_order_relaxed);
        return hit;
    }
    const int floor = echo_floor_.load(std::memory_order_relaxed);
    const int need = floor + std::max(floor / 3, 80);
    const int drop = floor + std::max(floor / 6, 40);
    const bool latched = near_end_latched_.load(std::memory_order_relaxed);
    bool hit = residual >= (latched ? drop : need);
    if (hit && (int64_t)residual * 100 < (int64_t)playback * kMinResidualPctOfPlayback) {
        hit = false;
    }
    near_end_latched_.store(hit, std::memory_order_relaxed);
    return hit;
}

void AudioService::EnableDeviceAec(bool enable) {
    ESP_LOGI(TAG, "%s device AEC", enable ? "Enabling" : "Disabling");
    device_aec_enabled_ = enable;

    if (audio_engine_initialized_) {
        audio_engine_->EnableDeviceAec(enable);
    } else {
        ESP_LOGI(TAG, "Deferring AEC change until the audio engine is initialized");
    }
}

void AudioService::SetPlaybackMuted(bool muted) {
    playback_muted_.store(muted, std::memory_order_relaxed);
    if (!muted) {
        playback_duck_q8_.store(256, std::memory_order_relaxed);
    }
}

bool AudioService::IsPlaybackMuted() const {
    return playback_muted_.load(std::memory_order_relaxed);
}

void AudioService::SetPlaybackDuckQ8(int duck_q8) {
    if (duck_q8 < 1) {
        duck_q8 = 1;
    } else if (duck_q8 > 256) {
        duck_q8 = 256;
    }
    playback_duck_q8_.store(duck_q8, std::memory_order_relaxed);
}

void AudioService::SetCallbacks(AudioServiceCallbacks& callbacks) { callbacks_ = callbacks; }

void AudioService::PlaySound(const std::string_view& ogg) {
    if (!codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
        esp_timer_start_periodic(audio_power_timer_, AUDIO_POWER_CHECK_INTERVAL_MS * 1000);
    }
#if CONFIG_BOARD_TYPE_MOSS_OV2640
    MossDesktopPreparePlayback(codec_);
#else
    codec_->PreparePlayback();
#endif

    const auto* buf = reinterpret_cast<const uint8_t*>(ogg.data());
    size_t size = ogg.size();

    auto demuxer = std::make_unique<OggDemuxer>();
    demuxer->OnDemuxerFinished([this](const uint8_t* data, int sample_rate, size_t size) {
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = sample_rate;
        packet->frame_duration = 60;
        packet->payload.resize(size);
        std::memcpy(packet->payload.data(), data, size);
        PushPacketToDecodeQueue(std::move(packet), true);
    });
    demuxer->Reset();
    demuxer->Process(buf, size);
}

bool AudioService::IsIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return audio_encode_queue_.empty() && IsPlaybackDrainedLocked() && audio_testing_queue_.empty();
}

bool AudioService::IsPlaybackIdle() {
    std::lock_guard<std::mutex> lock(audio_queue_mutex_);
    return IsPlaybackDrainedLocked();
}

void AudioService::ResetDecoder() {
    playback_muted_.store(false, std::memory_order_relaxed);
    playback_duck_q8_.store(256, std::memory_order_relaxed);
    bool notify_drained = false;
    {
        std::lock_guard<std::mutex> lock(audio_queue_mutex_);
        ++playback_generation_;
        std::unique_lock<std::mutex> decoder_lock(decoder_mutex_);
        if (opus_decoder_ != nullptr) {
            esp_opus_dec_reset(opus_decoder_);
        }
        decoder_lock.unlock();
        timestamp_queue_.clear();
        audio_decode_queue_.clear();
        audio_playback_queue_.clear();
        audio_testing_queue_.clear();
        notify_drained = MarkPlaybackDrainedLocked();
        audio_queue_cv_.notify_all();
    }
    if (notify_drained && callbacks_.on_playback_drained) {
        callbacks_.on_playback_drained();
    }
    // Do not ReleasePlayback here. tts/start calls ResetDecoder while the mic
    // is still on; turning the amp off while leaving I2S up made the next
    // PreparePlayback skip (output_enabled already true) and TTS was silent.
}

bool AudioService::IsPlaybackDrainedLocked() const {
    return audio_decode_queue_.empty() && audio_playback_queue_.empty() && !decode_in_flight_ &&
           !output_in_flight_;
}

bool AudioService::MarkPlaybackDrainedLocked() {
    if (!IsPlaybackDrainedLocked() || playback_drained_notified_) {
        return false;
    }
    playback_drained_notified_ = true;
    return true;
}

void AudioService::CheckAndUpdateAudioPowerState() {
    auto now = std::chrono::steady_clock::now();
    auto input_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_input_time_).count();
    auto output_elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - last_output_time_).count();
    if (input_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->input_enabled()) {
        // ADC continuous start/stop must run in the same task. Wake the audio
        // input task instead of closing the codec from the esp_timer task.
        xEventGroupSetBits(event_group_, AS_EVENT_AUDIO_INPUT_STOP_REQUEST);
    }
#if CONFIG_BOARD_TYPE_MOSS_OV2640
    if (codec_->output_enabled() && !MossDesktopSharedI2cHeld()) {
        if (output_elapsed > 800 && IsPlaybackIdle()) {
            // PA off only. Never close I2S TX (duplex). Skip entirely while
            // the camera holds the shared I2C bus (SCCB + PCA9685 + codec).
            MossDesktopReleasePlayback(codec_);
        }
    }
#else
    if (output_elapsed > AUDIO_POWER_TIMEOUT_MS && codec_->output_enabled()) {
        // Keep TX clock when duplex RX is active; otherwise RX may stall on some boards.
        if (!(codec_->duplex() && codec_->input_enabled())) {
            codec_->EnableOutput(false);
        }
    }
#endif
    if (!codec_->input_enabled() && !codec_->output_enabled()) {
        esp_timer_stop(audio_power_timer_);
    }
}

void AudioService::SetModelsList(srmodel_list_t* models_list) {
    if (audio_engine_initialized_ && models_list_ != models_list) {
        ESP_LOGW(TAG, "Ignoring speech model replacement after audio engine initialization");
        return;
    }
    models_list_ = models_list;
}

bool AudioService::IsAfeWakeWord() {
    return audio_engine_initialized_ && audio_engine_->IsAfeWakeWord();
}

std::vector<std::string> AudioService::GetWakeWordPhrases() const {
    if (!audio_engine_initialized_ || !audio_engine_) {
        return {};
    }
    return audio_engine_->GetWakeWordPhrases();
}

std::string AudioService::GetWakeWordEngine() const {
    if (!audio_engine_initialized_ || !audio_engine_) {
        return "none";
    }
    return audio_engine_->GetWakeWordEngine();
}

bool AudioService::GetWakeWordConfig(std::vector<WakeWordCommandEntry>* entries,
                                     int* threshold_percent) const {
    if (!audio_engine_initialized_ || !audio_engine_) {
        return false;
    }
    return audio_engine_->GetWakeWordConfig(entries, threshold_percent);
}

bool AudioService::ApplyWakeWordConfig(const std::vector<WakeWordCommandEntry>& entries,
                                       int threshold_percent) {
    if (!audio_engine_initialized_ || !audio_engine_) {
        return false;
    }
    return audio_engine_->ApplyWakeWordConfig(entries, threshold_percent);
}

bool AudioService::InitializeAudioEngine() {
    if (!audio_engine_) {
        return false;
    }
    if (audio_engine_initialized_) {
        return true;
    }
    if (!audio_engine_->Initialize(codec_, OPUS_FRAME_DURATION_MS, models_list_)) {
        ESP_LOGE(TAG, "Failed to initialize audio engine");
        return false;
    }
    audio_engine_initialized_ = true;
    audio_engine_->EnableDeviceAec(device_aec_enabled_);
    return true;
}
