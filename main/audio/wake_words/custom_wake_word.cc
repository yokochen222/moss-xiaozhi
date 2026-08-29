#include "custom_wake_word.h"
#include "assets.h"
#include "audio_service.h"
#include "settings.h"
#include "system_info.h"

#include <esp_log.h>
#include <esp_mn_iface.h>
#include <esp_mn_models.h>
#include <esp_mn_speech_commands.h>
#include <cJSON.h>
#include <algorithm>

#define TAG "CustomWakeWord"

// NVS key names must be <= 15 characters (NVS_KEY_NAME_MAX_SIZE).
namespace {
constexpr char kNvsWakeWordsJson[] = "wake_words_json";
constexpr char kNvsWakeWordCmd[] = "wake_word_cmd";
constexpr char kNvsWakeWordDisp[] = "wake_word_disp";
constexpr char kNvsWakeWordThr[] = "wake_word_thr";
static_assert(sizeof(kNvsWakeWordsJson) - 1 <= 15, "NVS key too long");
static_assert(sizeof(kNvsWakeWordCmd) - 1 <= 15, "NVS key too long");
static_assert(sizeof(kNvsWakeWordDisp) - 1 <= 15, "NVS key too long");
static_assert(sizeof(kNvsWakeWordThr) - 1 <= 15, "NVS key too long");
}  // namespace

CustomWakeWord::CustomWakeWord() : wake_word_opus_() {}

CustomWakeWord::~CustomWakeWord() {
    if (multinet_model_data_ != nullptr && multinet_ != nullptr) {
        multinet_->destroy(multinet_model_data_);
        multinet_model_data_ = nullptr;
    }

    if (wake_word_encode_task_stack_ != nullptr) {
        heap_caps_free(wake_word_encode_task_stack_);
    }

    if (wake_word_encode_task_buffer_ != nullptr) {
        heap_caps_free(wake_word_encode_task_buffer_);
    }

    if (owns_models_ && models_ != nullptr) {
        esp_srmodel_deinit(models_);
    }
}

void CustomWakeWord::ParseWakenetModelConfig() {
    // Read index.json
    auto& assets = Assets::GetInstance();
    void* ptr = nullptr;
    size_t size = 0;
    if (!assets.GetAssetData("index.json", ptr, size)) {
        ESP_LOGE(TAG, "Failed to read index.json");
        return;
    }
    cJSON* root = cJSON_ParseWithLength(static_cast<char*>(ptr), size);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Failed to parse index.json");
        return;
    }
    cJSON* multinet_model = cJSON_GetObjectItem(root, "multinet_model");
    if (cJSON_IsObject(multinet_model)) {
        cJSON* language = cJSON_GetObjectItem(multinet_model, "language");
        cJSON* duration = cJSON_GetObjectItem(multinet_model, "duration");
        cJSON* threshold = cJSON_GetObjectItem(multinet_model, "threshold");
        cJSON* commands = cJSON_GetObjectItem(multinet_model, "commands");
        if (cJSON_IsString(language)) {
            language_ = language->valuestring;
        }
        if (cJSON_IsNumber(duration)) {
            duration_ = duration->valueint;
        }
        if (cJSON_IsNumber(threshold)) {
            threshold_ = threshold->valuedouble;
        }
        if (cJSON_IsArray(commands)) {
            for (int i = 0; i < cJSON_GetArraySize(commands); i++) {
                cJSON* command = cJSON_GetArrayItem(commands, i);
                if (cJSON_IsObject(command)) {
                    cJSON* command_name = cJSON_GetObjectItem(command, "command");
                    cJSON* text = cJSON_GetObjectItem(command, "text");
                    cJSON* action = cJSON_GetObjectItem(command, "action");
                    if (cJSON_IsString(command_name) && cJSON_IsString(text) &&
                        cJSON_IsString(action)) {
                        commands_.push_back(
                            {command_name->valuestring, text->valuestring, action->valuestring});
                        ESP_LOGI(TAG, "Command: %s, Text: %s, Action: %s",
                                 command_name->valuestring, text->valuestring, action->valuestring);
                    }
                }
            }
        }
    }
    cJSON_Delete(root);
}

void CustomWakeWord::LoadStoredConfig() {
    Settings settings("vendor");
    std::vector<Command> loaded;
    const std::string json = settings.GetString(kNvsWakeWordsJson);
    if (!json.empty() && ParseEntriesJson(json, &loaded)) {
        commands_.assign(loaded.begin(), loaded.end());
    } else {
        std::string command = settings.GetString(kNvsWakeWordCmd);
        std::string display = settings.GetString(kNvsWakeWordDisp);
        WakeWordCommandEntry entry{command, display};
        if (!command.empty() && !display.empty() && IsValidWakeWordPinyin(entry.command) &&
            IsValidWakeWordDisplay(entry.display)) {
            commands_.clear();
            commands_.push_back({entry.command, entry.display, "wake"});
        } else if (!command.empty() || !display.empty()) {
            ESP_LOGW(TAG, "Skip invalid legacy wake word: command=%s display=%s", command.c_str(),
                     display.c_str());
        }
    }

    int threshold = settings.GetInt(kNvsWakeWordThr, -1);
    if (threshold >= 1 && threshold <= 99) {
        threshold_ = threshold / 100.0f;
    }
}

bool CustomWakeWord::ParseEntriesJson(const std::string& json, std::vector<Command>* out) {
    if (!out) {
        return false;
    }
    out->clear();
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }

    const int count = cJSON_GetArraySize(root);
    for (int i = 0; i < count && out->size() < kMaxWakeWordCommands; ++i) {
        cJSON* item = cJSON_GetArrayItem(root, i);
        if (!cJSON_IsObject(item)) {
            continue;
        }
        cJSON* command = cJSON_GetObjectItem(item, "command");
        cJSON* display = cJSON_GetObjectItem(item, "display");
        if (cJSON_IsString(command) && command->valuestring && cJSON_IsString(display) &&
            display->valuestring) {
            WakeWordCommandEntry entry{command->valuestring, display->valuestring};
            if (!IsValidWakeWordPinyin(entry.command) || !IsValidWakeWordDisplay(entry.display)) {
                ESP_LOGW(TAG, "Skip invalid stored wake word: command=%s display=%s",
                         entry.command.c_str(), entry.display.c_str());
                continue;
            }
            out->push_back({entry.command, entry.display, "wake"});
        }
    }
    cJSON_Delete(root);
    return !out->empty();
}

bool CustomWakeWord::SaveStoredConfig() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& entry : commands_) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "command", entry.command.c_str());
        cJSON_AddStringToObject(item, "display", entry.text.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* printed = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!printed) {
        return false;
    }
    Settings settings("vendor", true);
    if (!settings.SetString(kNvsWakeWordsJson, printed)) {
        cJSON_free(printed);
        return false;
    }
    cJSON_free(printed);
    if (!commands_.empty()) {
        if (!settings.SetString(kNvsWakeWordCmd, commands_.front().command) ||
            !settings.SetString(kNvsWakeWordDisp, commands_.front().text)) {
            return false;
        }
    }
    settings.SetInt(kNvsWakeWordThr, GetThresholdPercent());
    return true;
}

bool CustomWakeWord::Initialize(AudioCodec* codec, srmodel_list_t* models_list) {
    codec_ = codec;
    commands_.clear();

    if (models_list == nullptr) {
        language_ = "cn";
        models_ = esp_srmodel_init("model");
        owns_models_ = models_ != nullptr;
#ifdef CONFIG_CUSTOM_WAKE_WORD
        threshold_ = CONFIG_CUSTOM_WAKE_WORD_THRESHOLD / 100.0f;
        commands_.push_back({CONFIG_CUSTOM_WAKE_WORD, CONFIG_CUSTOM_WAKE_WORD_DISPLAY, "wake"});
#endif
    } else {
        models_ = models_list;
        ParseWakenetModelConfig();
    }

    LoadStoredConfig();

    if (models_ == nullptr || models_->num == -1) {
        ESP_LOGE(TAG, "Failed to initialize wakenet model");
        return false;
    }

    // 初始化 multinet (命令词识别)
    mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, language_.c_str());
    if (mn_name_ == nullptr) {
        ESP_LOGW(TAG, "Language '%s' multinet not found, falling back to any multinet model",
                 language_.c_str());
        mn_name_ = esp_srmodel_filter(models_, ESP_MN_PREFIX, NULL);
    }
    if (mn_name_ == nullptr) {
        ESP_LOGE(TAG, "Failed to initialize multinet, mn_name is nullptr");
        ESP_LOGI(TAG,
                 "Please refer to https://pcn7cs20v8cr.feishu.cn/wiki/CpQjwQsCJiQSWSkYEvrcxcbVnwh "
                 "to add custom wake word");
        return false;
    }

    multinet_ = esp_mn_handle_from_name(mn_name_);
    multinet_model_data_ = multinet_->create(mn_name_, duration_);
    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    input_buffer_.reserve(multinet_->get_samp_chunksize(multinet_model_data_));
    if (!UpdateMultinetCommands()) {
        ESP_LOGE(TAG, "Failed to register custom wake word commands");
        return false;
    }

    multinet_->print_active_speech_commands(multinet_model_data_);
#if CONFIG_SEND_WAKE_WORD_DATA
    if (!wake_word_audio_cache_.Initialize(16000 * 2)) {
        ESP_LOGW(TAG, "Wake-word audio upload disabled: PSRAM cache allocation failed");
    }
#endif
    return true;
}

void CustomWakeWord::OnWakeWordDetected(
    std::function<void(const std::string& wake_word)> callback) {
    wake_word_detected_callback_ = callback;
}

void CustomWakeWord::Start() { running_ = true; }

void CustomWakeWord::Stop() {
    running_ = false;

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    input_buffer_.clear();
}

void CustomWakeWord::Feed(const std::vector<int16_t>& data) {
    FeedSamples(data.data(), data.size(), false);
}

void CustomWakeWord::FeedMono(const int16_t* data, size_t samples) {
    FeedSamples(data, samples, true);
}

void CustomWakeWord::FeedSamples(const int16_t* data, size_t samples, bool mono) {
    if (multinet_model_data_ == nullptr || data == nullptr || samples == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(input_buffer_mutex_);
    // Check running state inside lock to avoid TOCTOU race with Stop()
    if (!running_) {
        return;
    }

    // If input channels is 2, we need to fetch the left channel data
    if (!mono && codec_->input_channels() > 1) {
        for (size_t i = 0; i < samples; i += codec_->input_channels()) {
            input_buffer_.push_back(data[i]);
        }
    } else {
        input_buffer_.insert(input_buffer_.end(), data, data + samples);
    }

    int chunksize = multinet_->get_samp_chunksize(multinet_model_data_);
    while (input_buffer_.size() >= chunksize) {
#if CONFIG_SEND_WAKE_WORD_DATA
        wake_word_audio_cache_.Store(input_buffer_.data(), chunksize);
#endif

        esp_mn_state_t mn_state = multinet_->detect(multinet_model_data_, input_buffer_.data());

        if (mn_state == ESP_MN_STATE_DETECTED) {
            esp_mn_results_t* mn_result = multinet_->get_results(multinet_model_data_);
            for (int i = 0; i < mn_result->num && running_; i++) {
                ESP_LOGI(TAG, "Custom wake word detected: command_id=%d, string=%s, prob=%f",
                         mn_result->command_id[i], mn_result->string, mn_result->prob[i]);
                const int command_index = mn_result->command_id[i] - 1;
                std::string wake_display;
                {
                    std::lock_guard<std::mutex> lock(commands_mutex_);
                    if (command_index < 0 || command_index >= static_cast<int>(commands_.size())) {
                        continue;
                    }
                    const auto& command = commands_[command_index];
                    if (command.action != "wake") {
                        continue;
                    }
                    wake_display = command.text;
                }
                last_detected_wake_word_ = wake_display;
                running_ = false;
                input_buffer_.clear();

                if (wake_word_detected_callback_) {
                    wake_word_detected_callback_(last_detected_wake_word_);
                }
                break;
            }
            multinet_->clean(multinet_model_data_);
        } else if (mn_state == ESP_MN_STATE_TIMEOUT) {
            ESP_LOGD(TAG, "Command word detection timeout, cleaning state");
            multinet_->clean(multinet_model_data_);
        }

        if (!running_) {
            break;
        }
        input_buffer_.erase(input_buffer_.begin(), input_buffer_.begin() + chunksize);
    }
}

size_t CustomWakeWord::GetFeedSize() {
    if (multinet_model_data_ == nullptr) {
        return 0;
    }
    return multinet_->get_samp_chunksize(multinet_model_data_);
}

void CustomWakeWord::EncodeWakeWordData() {
    const size_t stack_size = 4096 * 7;
    wake_word_opus_.clear();
    if (wake_word_encode_task_stack_ == nullptr) {
        wake_word_encode_task_stack_ =
            (StackType_t*)heap_caps_malloc(stack_size, MALLOC_CAP_SPIRAM);
        assert(wake_word_encode_task_stack_ != nullptr);
    }
    if (wake_word_encode_task_buffer_ == nullptr) {
        wake_word_encode_task_buffer_ =
            (StaticTask_t*)heap_caps_malloc(sizeof(StaticTask_t), MALLOC_CAP_INTERNAL);
        assert(wake_word_encode_task_buffer_ != nullptr);
    }

    wake_word_encode_task_ = xTaskCreateStatic(
        [](void* arg) {
            auto this_ = (CustomWakeWord*)arg;
            {
                auto start_time = esp_timer_get_time();
                // Create encoder
                esp_opus_enc_config_t opus_enc_cfg = AS_OPUS_ENC_CONFIG();
                void* encoder_handle = nullptr;
                auto ret = esp_opus_enc_open(&opus_enc_cfg, sizeof(esp_opus_enc_config_t),
                                             &encoder_handle);
                if (encoder_handle == nullptr) {
                    ESP_LOGE(TAG, "Failed to create audio encoder, error code: %d", ret);
                    this_->wake_word_audio_cache_.Clear();
                    std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                    this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                    this_->wake_word_cv_.notify_all();
                    vTaskDelete(nullptr);
                    return;
                }
                // Get frame size
                int frame_size = 0;
                int outbuf_size = 0;
                esp_opus_enc_get_frame_size(encoder_handle, &frame_size, &outbuf_size);
                frame_size = frame_size / sizeof(int16_t);
                // Encode all PCM data
                int packets = 0;
                std::vector<int16_t> in_buffer(frame_size);
                esp_audio_enc_in_frame_t in = {};
                esp_audio_enc_out_frame_t out = {};
                const size_t cached_samples = this_->wake_word_audio_cache_.Size();
                for (size_t offset = 0; offset + static_cast<size_t>(frame_size) <= cached_samples;
                     offset += frame_size) {
                    if (this_->wake_word_audio_cache_.Read(offset, in_buffer.data(), frame_size) !=
                        static_cast<size_t>(frame_size)) {
                        break;
                    }
                    std::vector<uint8_t> opus_buf(outbuf_size);
                    in.buffer = reinterpret_cast<uint8_t*>(in_buffer.data());
                    in.len = frame_size * sizeof(int16_t);
                    out.buffer = opus_buf.data();
                    out.len = outbuf_size;
                    out.encoded_bytes = 0;
                    ret = esp_opus_enc_process(encoder_handle, &in, &out);
                    if (ret == ESP_AUDIO_ERR_OK) {
                        std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                        this_->wake_word_opus_.emplace_back(opus_buf.data(),
                                                            opus_buf.data() + out.encoded_bytes);
                        this_->wake_word_cv_.notify_all();
                        packets++;
                    } else {
                        ESP_LOGE(TAG, "Failed to encode audio, error code: %d", ret);
                    }
                }
                this_->wake_word_audio_cache_.Clear();
                // Close encoder
                esp_opus_enc_close(encoder_handle);
                auto end_time = esp_timer_get_time();
                ESP_LOGI(TAG, "Encode wake word opus %d packets in %ld ms", packets,
                         (long)((end_time - start_time) / 1000));

                std::lock_guard<std::mutex> lock(this_->wake_word_mutex_);
                this_->wake_word_opus_.push_back(std::vector<uint8_t>());
                this_->wake_word_cv_.notify_all();
            }
            vTaskDelete(NULL);
        },
        "encode_wake_word", stack_size, this, 2, wake_word_encode_task_stack_,
        wake_word_encode_task_buffer_);
}

bool CustomWakeWord::GetWakeWordOpus(std::vector<uint8_t>& opus) {
    std::unique_lock<std::mutex> lock(wake_word_mutex_);
    wake_word_cv_.wait(lock, [this]() { return !wake_word_opus_.empty(); });
    opus.swap(wake_word_opus_.front());
    wake_word_opus_.pop_front();
    return !opus.empty();
}

bool CustomWakeWord::UpdateMultinetCommands() {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    if (multinet_model_data_ == nullptr || commands_.empty()) {
        return false;
    }
    esp_mn_commands_clear();
    for (size_t i = 0; i < commands_.size(); ++i) {
        esp_mn_commands_add(static_cast<int>(i + 1), commands_[i].command.c_str());
    }
    return esp_mn_commands_update() == ESP_OK;
}

std::vector<std::string> CustomWakeWord::GetDisplayPhrases() const {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    std::vector<std::string> phrases;
    phrases.reserve(commands_.size());
    for (const auto& command : commands_) {
        phrases.push_back(command.text);
    }
    return phrases;
}

std::vector<WakeWordCommandEntry> CustomWakeWord::GetEntries() const {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    std::vector<WakeWordCommandEntry> entries;
    entries.reserve(commands_.size());
    for (const auto& command : commands_) {
        entries.push_back({command.command, command.text});
    }
    return entries;
}

int CustomWakeWord::GetThresholdPercent() const {
    std::lock_guard<std::mutex> lock(commands_mutex_);
    int percent = static_cast<int>(threshold_ * 100.0f + 0.5f);
    return std::clamp(percent, 1, 99);
}

bool CustomWakeWord::ApplyConfig(const std::vector<WakeWordCommandEntry>& entries,
                                 int threshold_percent) {
    std::string validation_error;
    if (!ValidateWakeWordEntries(entries, &validation_error)) {
        ESP_LOGW(TAG, "ApplyConfig rejected: %s", validation_error.c_str());
        return false;
    }

    std::deque<Command> next;
    for (const auto& entry : entries) {
        next.push_back({entry.command, entry.display, "wake"});
    }

    threshold_percent = std::clamp(threshold_percent, 1, 99);
    Stop();

    {
        std::lock_guard<std::mutex> lock(commands_mutex_);
        threshold_ = threshold_percent / 100.0f;
        commands_ = std::move(next);
    }
    if (!SaveStoredConfig()) {
        ESP_LOGE(TAG, "ApplyConfig failed to persist wake word settings");
        return false;
    }

    if (multinet_model_data_ == nullptr || multinet_ == nullptr) {
        return true;
    }

    multinet_->set_det_threshold(multinet_model_data_, threshold_);
    multinet_->clean(multinet_model_data_);
    return UpdateMultinetCommands();
}
