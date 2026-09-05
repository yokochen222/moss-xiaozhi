#include "application.h"
#include "assets.h"
#include "assets/lang_config.h"
#include "audio_codec.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "mqtt_protocol.h"
#include "settings.h"
#include "system_info.h"
#include "text_glyph_payload.h"
#include "websocket_protocol.h"

#include "config/device_config.h"

#ifdef CONFIG_BOARD_FAMILY_MOSS
#include "config/moss_config_service.h"
#include "mcp_tools.h"
#endif
#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
#include "device/face_tracker.h"
#include "device/moss_camera_stream.h"
#endif

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <arpa/inet.h>
#include <cJSON.h>
#include <algorithm>
#include <atomic>
#include <cstring>

#define TAG "Application"

namespace {
constexpr int kConnectingTimeoutTicks = 20;
constexpr const char* kYunxiangjiWakeText = "请调用工具查询提醒内容";
std::atomic<int> g_tts_downlink_logs{0};
#if CONFIG_ENABLE_VAD_INTERRUPT
// TTS / AEC settle before local VAD or cloud ASR may barge-in.
constexpr int64_t kSpeakingBargeInGuardUs = 1500 * 1000;
#endif

std::string NormalizeAnnounceText(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c <= 0x20 || c == '!' || c == '?' || c == '.' || c == ',' || c == ';' || c == ':' ||
            c == '~') {
            ++i;
            continue;
        }
        if (i + 2 < text.size()) {
            const unsigned char c1 = static_cast<unsigned char>(text[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(text[i + 2]);
            if (c == 0xE3 && c1 == 0x80 && (c2 == 0x81 || c2 == 0x82)) {
                i += 3;
                continue;
            }
            if (c == 0xEF && c1 == 0xBC && (c2 == 0x81 || c2 == 0x8C || c2 == 0x9F)) {
                i += 3;
                continue;
            }
        }
        int n = 1;
        if ((c & 0x80) == 0) {
            n = 1;
        } else if ((c & 0xE0) == 0xC0) {
            n = 2;
        } else if ((c & 0xF0) == 0xE0) {
            n = 3;
        } else if ((c & 0xF8) == 0xF0) {
            n = 4;
        }
        if (i + static_cast<size_t>(n) > text.size()) {
            n = 1;
        }
        out.append(text, i, static_cast<size_t>(n));
        i += static_cast<size_t>(n);
    }
    return out;
}
}  // namespace

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {.callback =
                                                    [](void* arg) {
                                                        Application* app = (Application*)arg;
                                                        xEventGroupSetBits(app->event_group_,
                                                                           MAIN_EVENT_CLOCK_TICK);
                                                    },
                                                .arg = this,
                                                .dispatch_method = ESP_TIMER_TASK,
                                                .name = "clock_timer",
                                                .skip_unhandled_events = true};
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);

#if CONFIG_ENABLE_VAD_INTERRUPT
    esp_timer_create_args_t vad_timer_args = {
        .callback =
            [](void* arg) {
                auto* app = static_cast<Application*>(arg);
                xEventGroupSetBits(app->event_group_, MAIN_EVENT_VAD_INTERRUPT_CONFIRM);
            },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "vad_barge_in",
        .skip_unhandled_events = true};
    esp_timer_create(&vad_timer_args, &vad_interrupt_timer_);
#endif
}

Application::~Application() {
#if CONFIG_ENABLE_VAD_INTERRUPT
    if (vad_interrupt_timer_ != nullptr) {
        esp_timer_stop(vad_interrupt_timer_);
        esp_timer_delete(vad_interrupt_timer_);
        vad_interrupt_timer_ = nullptr;
    }
#endif
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::SetDeviceState(DeviceState state) { return state_machine_.TransitionTo(state); }

void Application::Initialize() {
    auto& board = Board::GetInstance();
    SetDeviceState(kDeviceStateStarting);

    // Setup the display
    auto display = board.GetDisplay();
    display->SetupUI();
    // Print board name/version info
    display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());

    // Setup the audio service
    auto codec = board.GetAudioCodec();
    audio_service_.Initialize(codec);
    audio_service_.Start();

    AudioServiceCallbacks callbacks;
    callbacks.on_send_queue_available = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
    };
    callbacks.on_wake_word_detected = [this](const std::string& wake_word) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_WAKE_WORD_DETECTED);
    };
    callbacks.on_vad_change = [this](bool speaking) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
    };
    callbacks.on_playback_drained = [this]() {
        xEventGroupSetBits(event_group_, MAIN_EVENT_PLAYBACK_DRAINED);
    };
    audio_service_.SetCallbacks(callbacks);

#if CONFIG_USE_DEVICE_AEC
    Application::GetInstance().LoadDeviceAecFromStorage();
#endif

    // Add state change listeners
    state_machine_.AddStateChangeListener([this](DeviceState old_state, DeviceState new_state) {
        xEventGroupSetBits(event_group_, MAIN_EVENT_STATE_CHANGED);
    });

    // Start the clock timer to update the status bar
    esp_timer_start_periodic(clock_timer_handle_, 1000000);

    // Add MCP common tools (only once during initialization)
    auto& mcp_server = McpServer::GetInstance();
    mcp_server.AddCommonTools();
    mcp_server.AddUserOnlyTools();
#ifdef CONFIG_BOARD_FAMILY_MOSS
    mcp_tools::RegisterYunxiangjiTools();
#endif

    // Set network event callback for UI updates and network state handling
    board.SetNetworkEventCallback([this](NetworkEvent event, const std::string& data) {
        auto display = Board::GetInstance().GetDisplay();

        switch (event) {
            case NetworkEvent::Scanning:
                display->ShowNotification(Lang::Strings::SCANNING_WIFI, 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::Connecting: {
                if (data.empty()) {
                    // Cellular network - registering without carrier info yet
                    display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                } else {
                    // WiFi or cellular with carrier info
                    std::string msg = Lang::Strings::CONNECT_TO;
                    msg += data;
                    msg += "...";
                    display->ShowNotification(msg.c_str(), 30000);
                }
                break;
            }
            case NetworkEvent::Connected: {
                std::string msg = Lang::Strings::CONNECTED_TO;
                msg += data;
                display->ShowNotification(msg.c_str(), 30000);
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_CONNECTED);
                break;
            }
            case NetworkEvent::Disconnected:
                xEventGroupSetBits(event_group_, MAIN_EVENT_NETWORK_DISCONNECTED);
                break;
            case NetworkEvent::WifiConfigModeEnter:
                // WiFi config mode enter is handled by WifiBoard internally
                break;
            case NetworkEvent::WifiConfigModeExit:
                // WiFi config mode exit is handled by WifiBoard internally
                break;
            // Cellular modem specific events
            case NetworkEvent::ModemDetecting:
                display->SetStatus(Lang::Strings::DETECTING_MODULE);
                break;
            case NetworkEvent::ModemErrorNoSim:
                Alert(Lang::Strings::ERROR, Lang::Strings::PIN_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_PIN);
                break;
            case NetworkEvent::ModemErrorRegDenied:
                Alert(Lang::Strings::ERROR, Lang::Strings::REG_ERROR, "warning",
                      Lang::Sounds::OGG_ERR_REG);
                break;
            case NetworkEvent::ModemErrorInitFailed:
                Alert(Lang::Strings::ERROR, Lang::Strings::MODEM_INIT_ERROR, "warning",
                      Lang::Sounds::OGG_EXCLAMATION);
                break;
            case NetworkEvent::ModemErrorTimeout:
                display->SetStatus(Lang::Strings::REGISTERING_NETWORK);
                break;
        }
    });

    // Start network asynchronously
    board.StartNetwork();

    // Update the status bar immediately to show the network state
    display->UpdateStatusBar(true);
}

void Application::Run() {
    // Set the priority of the main task to 10
    vTaskPrioritySet(nullptr, 10);

    const EventBits_t ALL_EVENTS =
        MAIN_EVENT_SCHEDULE | MAIN_EVENT_SEND_AUDIO | MAIN_EVENT_WAKE_WORD_DETECTED |
        MAIN_EVENT_VAD_CHANGE | MAIN_EVENT_CLOCK_TICK | MAIN_EVENT_ERROR |
        MAIN_EVENT_NETWORK_CONNECTED | MAIN_EVENT_NETWORK_DISCONNECTED | MAIN_EVENT_TOGGLE_CHAT |
        MAIN_EVENT_START_LISTENING | MAIN_EVENT_STOP_LISTENING | MAIN_EVENT_ACTIVATION_DONE |
        MAIN_EVENT_STATE_CHANGED | MAIN_EVENT_PLAYBACK_DRAINED
#if CONFIG_ENABLE_VAD_INTERRUPT
        | MAIN_EVENT_VAD_INTERRUPT_CONFIRM
#endif
        ;

    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, ALL_EVENTS, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            Alert(Lang::Strings::ERROR, last_error_message_.c_str(), "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_NETWORK_CONNECTED) {
            HandleNetworkConnectedEvent();
        }

        if (bits & MAIN_EVENT_NETWORK_DISCONNECTED) {
            HandleNetworkDisconnectedEvent();
        }

        if (bits & MAIN_EVENT_ACTIVATION_DONE) {
            HandleActivationDoneEvent();
        }

        if (bits & MAIN_EVENT_STATE_CHANGED) {
            HandleStateChangedEvent();
        }

        if (bits & MAIN_EVENT_PLAYBACK_DRAINED) {
            // Deferred listening start (auto mode): the playback queue has
            // drained, so it is now safe to enable voice processing.
            if (pending_listening_start_ && GetDeviceState() == kDeviceStateListening &&
                audio_service_.IsPlaybackIdle()) {
                pending_listening_start_ = false;
                StartListeningAudio();
            }
        }

        if (bits & MAIN_EVENT_TOGGLE_CHAT) {
            HandleToggleChatEvent();
        }

        if (bits & MAIN_EVENT_START_LISTENING) {
            HandleStartListeningEvent();
        }

        if (bits & MAIN_EVENT_STOP_LISTENING) {
            HandleStopListeningEvent();
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            // xiaozhi: always drain the encode queue. Realtime duplex relies on
            // AEC-cleaned uplink during TTS (cloud barge-in). Dropping here
            // clips the onset and duplicates after a later flush.
            if (GetDeviceState() == kDeviceStateListening && pending_listening_start_) {
                while (audio_service_.PopPacketFromSendQueue())
                    ;
            } else {
                SendUplinkFromQueue();
            }
        }

        if (bits & MAIN_EVENT_WAKE_WORD_DETECTED) {
            HandleWakeWordDetectedEvent();
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (GetDeviceState() == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
#if CONFIG_ENABLE_VAD_INTERRUPT
            // Rising edge after silence-arm. Confirm only after TTS is playing
            // and speech is sustained (timer). Without device AEC, speaker leak
            // looks like VAD; half-duplex must not arm barge-in.
            if (GetDeviceState() == kDeviceStateSpeaking && protocol_ && IsVadBargeInEnabled()) {
                const bool voice = audio_service_.IsVoiceDetected();
                if (!voice) {
                    vad_interrupt_armed_ = true;
                    CancelVadInterruptTimer();
                } else if (vad_interrupt_armed_) {
                    MaybeStartVadInterruptTimer();
                }
            }
#endif
        }

#if CONFIG_ENABLE_VAD_INTERRUPT
        if (bits & MAIN_EVENT_VAD_INTERRUPT_CONFIRM) {
            HandleVadInterruptConfirm();
        }
#endif

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();

            if (GetDeviceState() == kDeviceStateConnecting &&
                clock_ticks_ == kConnectingTimeoutTicks) {
                ESP_LOGW(TAG, "Connecting state timeout, returning to idle");
                Schedule([this]() { CloseVoiceSession(false); });
            }

            // Print debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                SystemInfo::PrintHeapStats();
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
            }
        }
    }
}

void Application::HandleNetworkConnectedEvent() {
    ESP_LOGI(TAG, "Network connected");
#ifdef CONFIG_BOARD_FAMILY_MOSS
    MossConfigService::GetInstance().OnNetworkConnected();
#endif
    auto state = GetDeviceState();

    if (state == kDeviceStateStarting || state == kDeviceStateWifiConfiguring) {
        // Network is ready, start activation
        SetDeviceState(kDeviceStateActivating);
        if (activation_task_handle_ != nullptr) {
            ESP_LOGW(TAG, "Activation task already running");
            return;
        }

        xTaskCreate(
            [](void* arg) {
                Application* app = static_cast<Application*>(arg);
                app->ActivationTask();
                app->activation_task_handle_ = nullptr;
                vTaskDelete(NULL);
            },
            "activation", 4096 * 2, this, 2, &activation_task_handle_);
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleNetworkDisconnectedEvent() {
    // Close current conversation when network disconnected
    auto state = GetDeviceState();
    if (state == kDeviceStateConnecting || state == kDeviceStateListening ||
        state == kDeviceStateSpeaking) {
        ESP_LOGI(TAG, "Closing audio channel due to network disconnection");
        protocol_->CloseAudioChannel();
    }

    // Update the status bar immediately to show the network state
    auto display = Board::GetInstance().GetDisplay();
    display->UpdateStatusBar(true);
}

void Application::HandleActivationDoneEvent() {
    ESP_LOGI(TAG, "Activation done");

    SystemInfo::PrintHeapStats();
    SetDeviceState(kDeviceStateIdle);

    has_server_time_ = ota_->HasServerTime();

    auto display = Board::GetInstance().GetDisplay();
    std::string message = std::string(Lang::Strings::VERSION) + ota_->GetCurrentVersion();
    display->ShowNotification(message.c_str());
    display->SetChatMessage("system", "");

    // Release OTA object after activation is complete
    ota_.reset();
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);

    Schedule([this]() {
        // Play the success sound to indicate the device is ready
        audio_service_.PlaySound(Lang::Sounds::OGG_SUCCESS);
    });
}

void Application::ActivationTask() {
    // Create OTA object for activation process
    ota_ = std::make_unique<Ota>();

    // Check for new assets version
    CheckAssetsVersion();

    // Check for new firmware version
    CheckNewVersion();

    // Initialize the protocol
    InitializeProtocol();

    // Signal completion to main loop
    xEventGroupSetBits(event_group_, MAIN_EVENT_ACTIVATION_DONE);
}

void Application::CheckAssetsVersion() {
    // Only allow CheckAssetsVersion to be called once
    if (assets_version_checked_) {
        return;
    }
    assets_version_checked_ = true;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }

    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_download", Lang::Sounds::OGG_UPGRADE);

        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success =
            assets.Download(download_url, [this, display](int progress, size_t speed) -> void {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                Schedule([display, message = std::string(buffer)]() {
                    display->SetChatMessage("system", message.c_str());
                });
            });

        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "cancel",
                  Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            SetDeviceState(kDeviceStateActivating);
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("robot_2");
}

void Application::CheckNewVersion() {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10;  // Initial retry delay in seconds

    auto& board = Board::GetInstance();
    while (true) {
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota_->CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err,
                     ota_->GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay,
                     error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_off", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay,
                     retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                if (GetDeviceState() == kDeviceStateIdle) {
                    break;
                }
            }
            retry_delay *= 2;  // Double the retry delay
            continue;
        }
        retry_count = 0;
        retry_delay = 10;  // Reset retry delay

        if (ota_->HasNewVersion()) {
            if (UpgradeFirmware(ota_->GetFirmwareUrl(), ota_->GetFirmwareVersion())) {
                return;  // This line will never be reached after reboot
            }
            // If upgrade failed, continue to normal operation
        }

        // No new version, mark the current version as valid
        ota_->MarkCurrentVersionValid();
        if (!ota_->HasActivationCode() && !ota_->HasActivationChallenge()) {
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota_->HasActivationCode()) {
            ShowActivationCode(ota_->GetActivationCode(), ota_->GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota_->Activate();
            if (err == ESP_OK) {
                break;
            } else if (err == ESP_ERR_TIMEOUT) {
                vTaskDelay(pdMS_TO_TICKS(3000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(10000));
            }
            if (GetDeviceState() == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::InitializeProtocol() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto codec = board.GetAudioCodec();

    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    if (ota_->HasMqttConfig()) {
        protocol_ = std::make_unique<MqttProtocol>();
    } else if (ota_->HasWebsocketConfig()) {
        protocol_ = std::make_unique<WebsocketProtocol>();
    } else {
        ESP_LOGW(TAG, "No protocol specified in the OTA config, using MQTT");
        protocol_ = std::make_unique<MqttProtocol>();
    }

    protocol_->OnConnected([this]() { DismissAlert(); });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });

    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        auto state = GetDeviceState();
        // Duplicate reminder sentence_start sets this. Do not recover just
        // because we are already speaking — that replayed the same TTS.
        if (IsAnnounceReplaySuppressed()) {
            return;
        }
        if (protocol_->IsPendingAudioDropped()) {
            // SendTextChat sets dropped=true to discard leftover session audio.
            // Cloud TTS often arrives while still connecting/listening, before
            // tts/start or sentence_start can flip the device to speaking.
            // Recovering only in speaking drops the entire first utterance.
            if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
                state == kDeviceStateConnecting) {
                protocol_->SetPendingAudioDropped(false);
            } else {
                return;
            }
        }
        // Wake / text-trigger: MQTT binary can arrive while still connecting or
        // listening, before the scheduled tts/start flips the device to speaking.
        if (state == kDeviceStateSpeaking || state == kDeviceStateListening ||
            state == kDeviceStateConnecting) {
            if (g_tts_downlink_logs.load(std::memory_order_relaxed) < 3) {
                const int n = g_tts_downlink_logs.fetch_add(1, std::memory_order_relaxed);
                if (n < 3) {
                    ESP_LOGI(TAG, "TTS downlink #%d state=%d sr=%d bytes=%u", n + 1,
                             static_cast<int>(state), packet->sample_rate,
                             static_cast<unsigned>(packet->payload.size()));
                }
            }
            if (!audio_service_.PushPacketToDecodeQueue(std::move(packet))) {
                ESP_LOGW(TAG, "TTS downlink dropped: decode queue full");
            }
        }
    });

    protocol_->OnAudioChannelOpened([this, codec, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG,
                     "Server sample rate %d does not match device output sample rate %d, "
                     "resampling may cause distortion",
                     protocol_->server_sample_rate(), codec->output_sample_rate());
        }
        // Do not send listen/detect here. Doing it before listen/start makes the
        // cloud emit TTS JSON/MCP with no UDP audio; /chat/say after listening
        // already started is the path that has speaker output.
        if (!pending_text_to_send_.empty()) {
            ESP_LOGI(TAG, "OnAudioChannelOpened: pending text waiting for listen/start");
        }
        g_tts_downlink_logs.store(0, std::memory_order_relaxed);
    });

    protocol_->OnAudioChannelClosed([this, &board]() {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        const uint32_t generation = audio_session_generation_;
        Schedule([this, generation]() {
            if (generation != audio_session_generation_) {
                return;
            }
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });

    protocol_->OnIncomingJson([this, display](const cJSON* root) {
        // Parse JSON data
        auto type = cJSON_GetObjectItem(root, "type");
        if (!cJSON_IsString(type)) {
            ESP_LOGW(TAG, "Incoming JSON message has no type");
            return;
        }
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (!cJSON_IsString(state)) {
                return;
            }
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (!IsAnnounceReplaySuppressed() && protocol_) {
                        protocol_->SetPendingAudioDropped(false);
                    }
                    // tts/start is deferred onto the main loop. MQTT audio often
                    // queues first (wake greeting "你好，上校。"). ResetDecoder
                    // here would drop that entire utterance; only wipe when this
                    // is a new speaking turn or the queues are still empty.
                    const auto prior = GetDeviceState();
                    if (prior == kDeviceStateSpeaking || audio_service_.IsPlaybackIdle()) {
                        audio_service_.ResetDecoder();
                    } else {
                        ESP_LOGI(TAG, "tts/start: keep queued TTS packets");
                    }
                    if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
#if CONFIG_BOARD_TYPE_MOSS_OV2640
                        MossDesktopPreparePlayback(codec);
#else
                        codec->PreparePlayback();
#endif
                    }
                    SetDeviceState(kDeviceStateSpeaking);
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    suppress_announce_replay_.store(false, std::memory_order_relaxed);
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        // Announce inject used to stay in AutoStop; the cloud then
                        // sends goodbye at TTS end and the mic never comes back.
                        listening_mode_ = GetDefaultListeningMode();
                        ESP_LOGI(TAG, "TTS stop -> listening (mode=%d)", listening_mode_);
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            resume_listen_after_tts_ = true;
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    std::vector<TextGlyph> glyphs;
                    uint8_t bpp = 0;
                    if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                        glyphs.clear();
                    }
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring),
                              glyphs = std::move(glyphs), bpp]() {
                        // The model often restates the reminder after ack. Drop the
                        // second identical utterance so the speaker only plays it once.
                        if (ShouldDropDuplicateAnnounceTts(message)) {
                            ESP_LOGI(TAG, "drop duplicate reminder TTS: %s", message.c_str());
                            SuppressAnnounceReplay();
                            return;
                        }
                        // Some server paths emit sentence_start before tts/start; without
                        // this, pending_audio_dropped stays true and all TTS is silent.
                        aborted_ = false;
                        if (protocol_) {
                            protocol_->SetPendingAudioDropped(false);
                        }
                        if (GetDeviceState() != kDeviceStateSpeaking) {
                            SetDeviceState(kDeviceStateSpeaking);
                        }
                        if (auto* codec = Board::GetInstance().GetAudioCodec(); codec != nullptr) {
#if CONFIG_BOARD_TYPE_MOSS_OV2640
                            MossDesktopPreparePlayback(codec);
#else
                            codec->PreparePlayback();
#endif
                        }
                        display->AddTextGlyphs(glyphs, bpp);
                        display->SetChatMessage("assistant", message.c_str());
                        RelayChat("message", "assistant", message);
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                std::vector<TextGlyph> glyphs;
                uint8_t bpp = 0;
                if (!TextGlyphPayload::Parse(root, glyphs, bpp)) {
                    glyphs.clear();
                }
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring),
                          glyphs = std::move(glyphs), bpp]() {
                    display->AddTextGlyphs(glyphs, bpp);
                    display->SetChatMessage("user", message.c_str());
                    RelayChat("message", "user", message);
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    // Do a reboot if user requests a OTA update
                    Schedule([this]() { Reboot(); });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring,
                      Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule(
                    [this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                        display->SetChatMessage("system", payload_str.c_str());
                    });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    protocol_->Start();

#ifdef CONFIG_BOARD_FAMILY_MOSS
    // External MQTT is started by MossConfigService after Wi-Fi is up.
#endif
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    struct digit_sound {
        char digit;
        const std::string_view& sound;
    };
    static const std::array<digit_sound, 10> digit_sounds{
        {digit_sound{'0', Lang::Sounds::OGG_0}, digit_sound{'1', Lang::Sounds::OGG_1},
         digit_sound{'2', Lang::Sounds::OGG_2}, digit_sound{'3', Lang::Sounds::OGG_3},
         digit_sound{'4', Lang::Sounds::OGG_4}, digit_sound{'5', Lang::Sounds::OGG_5},
         digit_sound{'6', Lang::Sounds::OGG_6}, digit_sound{'7', Lang::Sounds::OGG_7},
         digit_sound{'8', Lang::Sounds::OGG_8}, digit_sound{'9', Lang::Sounds::OGG_9}}};

    // This sentence uses 9KB of SRAM, so we need to wait for it to finish
    Alert(Lang::Strings::ACTIVATION, message.c_str(), "link", Lang::Sounds::OGG_ACTIVATION);

    for (const auto& digit : code) {
        auto it = std::find_if(digit_sounds.begin(), digit_sounds.end(),
                               [digit](const digit_sound& ds) { return ds.digit == digit; });
        if (it != digit_sounds.end()) {
            audio_service_.PlaySound(it->sound);
        }
    }
}

void Application::Alert(const char* status, const char* message, const char* emotion,
                        const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        audio_service_.PlaySound(sound);
    }
}

void Application::DismissAlert() {
    if (GetDeviceState() == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(Lang::Strings::STANDBY);
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
        display->DismissDialog();
    }
}

void Application::ToggleChatState() { xEventGroupSetBits(event_group_, MAIN_EVENT_TOGGLE_CHAT); }

void Application::StartListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_START_LISTENING); }

void Application::StopListening() { xEventGroupSetBits(event_group_, MAIN_EVENT_STOP_LISTENING); }

void Application::HandleToggleChatEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    } else if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        ListeningMode mode = GetDefaultListeningMode();
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this, mode]() { ContinueOpenAudioChannel(mode); });
            return;
        }
        SetListeningMode(mode);
    } else if (state == kDeviceStateSpeaking) {
        CloseVoiceSession(true);
    } else if (state == kDeviceStateListening) {
        CloseVoiceSession(true);
    }
}

void Application::ContinueOpenAudioChannel(ListeningMode mode) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error)
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    SetListeningMode(mode);
}

void Application::HandleStartListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (state == kDeviceStateWifiConfiguring) {
        audio_service_.EnableAudioTesting(true);
        SetDeviceState(kDeviceStateAudioTesting);
        return;
    }

    if (!protocol_) {
        ESP_LOGE(TAG, "Protocol not initialized");
        return;
    }

    if (state == kDeviceStateIdle) {
        if (!protocol_->IsAudioChannelOpened()) {
            SetDeviceState(kDeviceStateConnecting);
            // Schedule to let the state change be processed first (UI update)
            Schedule([this]() { ContinueOpenAudioChannel(kListeningModeManualStop); });
            return;
        }
        SetListeningMode(kListeningModeManualStop);
    } else if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
        SetListeningMode(kListeningModeManualStop);
    }
}

void Application::HandleStopListeningEvent() {
    auto state = GetDeviceState();

    if (state == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    } else if (state == kDeviceStateListening) {
        if (protocol_) {
            protocol_->SendStopListening();
        }
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::HandleWakeWordDetectedEvent() {
    if (!protocol_) {
        return;
    }

    auto state = GetDeviceState();
    auto wake_word = audio_service_.GetLastWakeWord();
    ESP_LOGI(TAG, "Wake word detected: %s (state: %d)", wake_word.c_str(), (int)state);

    if (state == kDeviceStateIdle) {
        BeginWakeWordInvoke(wake_word);
    } else if (state == kDeviceStateSpeaking || state == kDeviceStateListening) {
        AbortSpeaking(kAbortReasonWakeWordDetected);
        // Clear send queue to avoid sending residues to server
        while (audio_service_.PopPacketFromSendQueue())
            ;

        if (state == kDeviceStateListening) {
            protocol_->SendStartListening(GetDefaultListeningMode());
            audio_service_.ResetDecoder();
            audio_service_.EnableWakeWordDetection(true);
        } else {
            play_popup_on_listening_ = true;
            SetListeningMode(GetDefaultListeningMode());
        }
    } else if (state == kDeviceStateActivating) {
        // Restart the activation check if the wake word is detected during activation
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::BeginWakeWordInvoke(const std::string& wake_word, bool encode_wake_audio) {
    // Must run in the main task with the device in idle state
    audio_session_generation_++;
    if (encode_wake_audio) {
        audio_service_.EncodeWakeWord();
    }

    // Always pass through the connecting state, even if the audio channel is
    // already opened. ContinueWakeWordInvoke() rejects any other state, so
    // skipping this transition would silently drop the wake word invocation.
    if (!SetDeviceState(kDeviceStateConnecting)) {
        // Wake word detection was stopped by the detection itself; restore it
        // so the device does not become unresponsive to wake words.
        audio_service_.EnableWakeWordDetection(true);
        return;
    }

    if (!protocol_->IsAudioChannelOpened()) {
        // Schedule to let the state change be processed first (UI update),
        // then continue with OpenAudioChannel which may block for ~1 second
        Schedule([this, wake_word, encode_wake_audio]() {
            ContinueWakeWordInvoke(wake_word, encode_wake_audio);
        });
        return;
    }
    ContinueWakeWordInvoke(wake_word, encode_wake_audio);
}

void Application::ContinueWakeWordInvoke(const std::string& wake_word, bool encode_wake_audio) {
    // Check state again in case it was changed during scheduling
    if (GetDeviceState() != kDeviceStateConnecting) {
        return;
    }

    // Switch to performance mode before connecting to reduce latency
    auto& board = Board::GetInstance();
    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    if (!protocol_->IsAudioChannelOpened()) {
        if (!protocol_->OpenAudioChannel()) {
            // Return to idle so the device is not stuck in the connecting
            // state (not every failure path reports a network error), and
            // wake word detection is re-enabled by the idle state handler.
            SetDeviceState(kDeviceStateIdle);
            return;
        }
    }

    ESP_LOGI(TAG, "Wake word detected: %s", wake_word.c_str());
    if (!pending_text_to_send_.empty()) {
        ESP_LOGI(TAG, "ContinueWakeWordInvoke: pending text waits for listen/start");
    } else {
#if CONFIG_SEND_WAKE_WORD_DATA
        if (encode_wake_audio) {
            while (auto packet = audio_service_.PopWakeWordPacket()) {
                protocol_->SendAudio(std::move(packet));
            }
        }
        protocol_->SendWakeWordDetected(wake_word);
#endif
    }
    play_popup_on_listening_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

void Application::HandleStateChangedEvent() {
    DeviceState new_state = state_machine_.GetState();
    clock_ticks_ = 0;
    // Any state change invalidates a pending deferred listening start;
    // the Listening case below re-arms it when needed.
    pending_listening_start_ = false;

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();

    // Screen-off / WiFi PS countdown only runs in idle.
    if (new_state == kDeviceStateIdle) {
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
    } else if (new_state != kDeviceStateUpgrading) {
        board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    }

    switch (new_state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle: {
            std::lock_guard<std::mutex> lock(mutex_);
            last_external_detect_text_.clear();
            ResetYunxiangjiSessionLocked();
        }
#if CONFIG_ENABLE_VAD_INTERRUPT
            CancelVadInterruptTimer();
            vad_interrupt_armed_ = false;
#endif
            resume_listen_after_tts_ = false;
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
            audio_service_.EndSpeakingCapture();
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(true);
#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
            FaceTracker::GetInstance().TryResumeAfterVoiceIdle();
#endif
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening: {
#if CONFIG_ENABLE_VAD_INTERRUPT
            CancelVadInterruptTimer();
            const bool keep_preroll = barge_in_listen_;
            barge_in_listen_ = false;
#else
            const bool keep_preroll = false;
#endif
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            if (!keep_preroll) {
                audio_service_.EndSpeakingCapture();
            } else {
                audio_service_.FlushSpeakingCaptureToSendQueue();
            }

            // Never listen/start while TTS is still in the speaker: realtime
            // uplink of that leak is the self-reply loop ("你好，上校").
            if (resume_listen_after_tts_) {
                resume_listen_after_tts_ = false;
                if (!audio_service_.IsPlaybackIdle() && pending_text_to_send_.empty()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning() ||
                keep_preroll) {
                if (!audio_service_.IsPlaybackIdle() && pending_text_to_send_.empty()) {
                    pending_listening_start_ = true;
                } else {
                    StartListeningAudio();
                }
            } else {
                ConfigureWakeWordForListening();
            }
#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
            FaceTracker::GetInstance().TryResumeAfterVoiceIdle();
#endif
            break;
        }
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);
            while (audio_service_.PopPacketFromSendQueue())
                ;
            audio_service_.BeginSpeakingCapture();

            if (listening_mode_ != kListeningModeRealtime) {
#if CONFIG_ENABLE_VAD_INTERRUPT
                // AutoStop + AEC off is half-duplex: close the mic so TTS cannot
                // barge-in as "user speech". Barge-in is only safe with device AEC.
                if (IsVadBargeInEnabled()) {
                    audio_service_.EnableVoiceProcessing(true);
                    audio_service_.EnableWakeWordDetection(false);
                } else {
                    audio_service_.EnableVoiceProcessing(false);
                    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
                }
#else
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#endif
            }
#if CONFIG_ENABLE_VAD_INTERRUPT
            CancelVadInterruptTimer();
            speaking_started_us_ = esp_timer_get_time();
            vad_interrupt_armed_ = IsVadBargeInEnabled() && !audio_service_.IsVoiceDetected();
#endif
            break;
        case kDeviceStateWifiConfiguring:
            audio_service_.EnableVoiceProcessing(false);
            audio_service_.EnableWakeWordDetection(false);
            break;
        default:
            // Do nothing
            break;
    }
    RelayChat("state", "", "");
}

#if CONFIG_ENABLE_VAD_INTERRUPT
void Application::CancelVadInterruptTimer() {
    if (vad_interrupt_timer_ != nullptr) {
        esp_timer_stop(vad_interrupt_timer_);
    }
}

void Application::MaybeStartVadInterruptTimer() {
    if (GetDeviceState() != kDeviceStateSpeaking || protocol_ == nullptr) {
        return;
    }
    if (!IsVadBargeInEnabled()) {
        return;
    }
    if (audio_service_.IsPlaybackIdle()) {
        return;
    }
    const int64_t elapsed = esp_timer_get_time() - speaking_started_us_;
    if (elapsed < kSpeakingBargeInGuardUs) {
        return;
    }
    if (vad_interrupt_timer_ == nullptr) {
        return;
    }
    if (esp_timer_is_active(vad_interrupt_timer_)) {
        return;
    }

    int64_t sustain_ms = 200;

    ESP_LOGI(TAG, "VAD barge-in candidate, confirming in %d ms",
             static_cast<int>(sustain_ms));
    esp_timer_start_once(vad_interrupt_timer_, sustain_ms * 1000);
}

void Application::HandleVadInterruptConfirm() {
    if (GetDeviceState() != kDeviceStateSpeaking || protocol_ == nullptr) {
        return;
    }
    if (!IsVadBargeInEnabled()) {
        return;
    }
    if (!audio_service_.IsVoiceDetected() || audio_service_.IsPlaybackIdle()) {
        return;
    }
    ESP_LOGI(TAG, "VAD barge-in confirmed pb=%d res=%d mic=%d ref=%d",
             audio_service_.PlaybackLevel(), audio_service_.ResidualLevel(),
             audio_service_.MicLevel(), audio_service_.RefLevel());
    AbortSpeaking(kAbortReasonVadInterrupt);
    audio_service_.ResetDecoder();
    barge_in_listen_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

#endif

void Application::SendUplinkFromQueue() {
    while (auto packet = audio_service_.PopPacketFromSendQueue()) {
        if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
            while (audio_service_.PopPacketFromSendQueue())
                ;
            break;
        }
    }
}

void Application::StartListeningAudio() {
    // Runs in the main loop, either directly from HandleStateChangedEvent or
    // deferred via MAIN_EVENT_PLAYBACK_DRAINED once the playback queue drains.
    if (GetDeviceState() != kDeviceStateListening) {
        return;
    }

    pending_listening_start_ = false;

    // Send the start listening command
    protocol_->SendStartListening(listening_mode_);
    audio_service_.EnableVoiceProcessing(true);

    ConfigureWakeWordForListening();

    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
    }

    if (!pending_text_to_send_.empty()) {
        std::string text = pending_text_to_send_;
        pending_text_to_send_.clear();
        if (ShouldSkipExternalDetect(text)) {
            ESP_LOGI(TAG, "StartListeningAudio: skip already-sent detect");
        } else if (protocol_) {
            last_external_detect_text_ = text;
            external_detect_sent_ = true;
            ESP_LOGI(TAG, "StartListeningAudio: sending pending text: %s", text.c_str());
            // Wake-word uplink already punched UDP. Idle text-chat has no mic
            // frames before AutoStop TTS (120ms warmup then speaking mutes RX).
            protocol_->SendUdpHolePunch();
            protocol_->SetPendingAudioDropped(true);
            protocol_->SendTextChat(text);
        }
    }
}

void Application::ConfigureWakeWordForListening() {
#ifdef CONFIG_WAKE_WORD_DETECTION_IN_LISTENING
    // Enable wake word detection in listening mode (configured via Kconfig)
    audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
#else
    // Disable wake word detection in listening mode
    audio_service_.EnableWakeWordDetection(false);
#endif
}

void Application::Schedule(std::function<void()>&& callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

ListeningMode Application::GetDefaultListeningMode() const {
    return aec_mode_ == kAecOff ? kListeningModeAutoStop : kListeningModeRealtime;
}

bool Application::IsVadBargeInEnabled() const { return GetAecMode() == kAecOnDeviceSide; }

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    audio_service_.Stop();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(const std::string& url, const std::string& version) {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();

    std::string upgrade_url = url;
    std::string version_info = version.empty() ? "(Manual upgrade)" : version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGI(TAG, "Starting firmware upgrade from URL: %s", upgrade_url.c_str());

    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download",
          Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(3000));

    SetDeviceState(kDeviceStateUpgrading);

    std::string message = std::string(Lang::Strings::NEW_VERSION) + version_info;
    display->SetChatMessage("system", message.c_str());

    board.SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
    audio_service_.Stop();
    vTaskDelay(pdMS_TO_TICKS(1000));

    bool upgrade_success = Ota::Upgrade(upgrade_url, [this, display](int progress, size_t speed) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
        Schedule([display, message = std::string(buffer)]() {
            display->SetChatMessage("system", message.c_str());
        });
    });

    if (!upgrade_success) {
        // Upgrade failed, restart audio service and continue running
        ESP_LOGE(TAG,
                 "Firmware upgrade failed, restarting audio service and continuing operation...");
        audio_service_.Start();                              // Restart audio service
        board.SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);  // Restore power save level
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "cancel",
              Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    } else {
        // Upgrade success, reboot immediately
        ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
        display->SetChatMessage("system", "Upgrade successful, rebooting...");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Brief pause to show message
        Reboot();
        return true;
    }
}

void Application::CloseVoiceSession(bool send_goodbye) {
    audio_session_generation_++;
    const auto state = GetDeviceState();
    if (state == kDeviceStateSpeaking) {
        AbortSpeaking(kAbortReasonNone);
    }
    if (protocol_) {
        protocol_->CloseAudioChannel(send_goodbye);
    }
    if (state == kDeviceStateListening || state == kDeviceStateSpeaking ||
        state == kDeviceStateConnecting) {
        SetDeviceState(kDeviceStateIdle);
    }
}

void Application::ProcessChatWake(const std::string& wake_word) {
    if (!protocol_) {
        return;
    }

    const auto state = GetDeviceState();
    switch (state) {
        case kDeviceStateIdle:
            BeginWakeWordInvoke(wake_word, false);
            break;
        case kDeviceStateListening:
        case kDeviceStateSpeaking:
            CloseVoiceSession(true);
            break;
        case kDeviceStateConnecting:
            ESP_LOGW(TAG, "Ignore chat.wake while connecting");
            break;
        default:
            break;
    }
}

void Application::RequestChatWake(const std::string& wake_word) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_chat_wake_word_ = wake_word;
        if (chat_wake_dispatch_scheduled_) {
            return;
        }
        chat_wake_dispatch_scheduled_ = true;
    }
    Schedule([this]() {
        std::string word;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            word = pending_chat_wake_word_.empty() ? "MOSS" : pending_chat_wake_word_;
            pending_chat_wake_word_.clear();
            chat_wake_dispatch_scheduled_ = false;
        }
        ProcessChatWake(word);
    });
}

void Application::WakeWordInvoke(const std::string& wake_word) { RequestChatWake(wake_word); }

void Application::ResetYunxiangjiSessionLocked() {
    last_taken_announce_.clear();
    yunxiangji_tts_played_ = false;
    external_detect_sent_ = false;
    suppress_announce_replay_.store(false, std::memory_order_relaxed);
}

void Application::SuppressAnnounceReplay() {
    suppress_announce_replay_.store(true, std::memory_order_relaxed);
    if (protocol_) {
        protocol_->SetPendingAudioDropped(true);
    }
    // First utterance already started seconds earlier; flush the replay.
    audio_service_.ResetDecoder();
}

bool Application::IsAnnounceReplaySuppressed() const {
    return suppress_announce_replay_.load(std::memory_order_relaxed);
}

bool Application::ShouldSkipExternalDetect(const std::string& text) const {
    if (text.empty() || !external_detect_sent_) {
        return false;
    }
    if (text == last_external_detect_text_) {
        return true;
    }
    return text == kYunxiangjiWakeText && last_external_detect_text_ == kYunxiangjiWakeText;
}

bool Application::ShouldDropDuplicateAnnounceTts(const std::string& spoken) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (last_taken_announce_.empty() || spoken.empty()) {
        return false;
    }
    if (NormalizeAnnounceText(spoken) != NormalizeAnnounceText(last_taken_announce_)) {
        return false;
    }
    if (yunxiangji_tts_played_) {
        return true;
    }
    yunxiangji_tts_played_ = true;
    return false;
}

void Application::SetPendingAnnounce(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_announce_ = text;
}

bool Application::PushPendingAnnounce(const std::string& text) {
    if (text.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string incoming = NormalizeAnnounceText(text);
    if (pending_announce_.empty()) {
        pending_announce_ = text;
        return true;
    }
    if (incoming == NormalizeAnnounceText(pending_announce_)) {
        return false;
    }
    pending_announce_ += "\n";
    pending_announce_ += text;
    return true;
}

void Application::PrepareAnnounceInterrupt() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_taken_announce_.clear();
        yunxiangji_tts_played_ = false;
    }
    last_external_detect_text_.clear();
    external_detect_sent_ = false;
    suppress_announce_replay_.store(false, std::memory_order_relaxed);
}

void Application::SendOrQueueExternalDetect(const std::string& text) {
    last_external_detect_text_ = text;
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        external_detect_sent_ = true;
        protocol_->SendUdpHolePunch();
        protocol_->SetPendingAudioDropped(true);
        protocol_->SendTextChat(text);
    } else {
        pending_text_to_send_ = text;
    }
}

std::string Application::PeekPendingAnnounce() {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_announce_;
}

std::string Application::TakePendingAnnounce() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text = pending_announce_;
    pending_announce_.clear();
    if (!text.empty()) {
        last_taken_announce_ = text;
        yunxiangji_tts_played_ = false;
    }
    return text;
}

std::string Application::AckPendingAnnounce() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_announce_.clear();
    if (!last_taken_announce_.empty()) {
        yunxiangji_tts_played_ = true;
    }
    return "已播报。不要再口头播报同一句话，不要说再见，不要结束会话，保持聆听等用户说话。";
}

void Application::HandleExternalTextMessage(const std::string& text, const std::string& announce) {
    if (text.empty()) {
        return;
    }

    bool interrupt = false;
    if (!announce.empty()) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last_taken_announce_.clear();
            yunxiangji_tts_played_ = false;
        }
        suppress_announce_replay_.store(false, std::memory_order_relaxed);
        interrupt = PushPendingAnnounce(announce);
    }

    Schedule([this, text, interrupt]() {
        auto state = GetDeviceState();
        const bool skip =
            !interrupt && (ShouldSkipExternalDetect(text) || text == last_external_detect_text_);

        if (state == kDeviceStateIdle) {
            if (!protocol_) {
                ESP_LOGE(TAG, "Protocol not initialized");
                return;
            }
            if (interrupt) {
                PrepareAnnounceInterrupt();
            }
            // Keep pending text until StartListeningAudio (listen/start then detect).
            pending_text_to_send_ = text;
            last_external_detect_text_ = text;
            listening_mode_ = GetDefaultListeningMode();
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    SetDeviceState(kDeviceStateIdle);
                    pending_text_to_send_.clear();
                    last_external_detect_text_.clear();
                    last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
                    xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
                    return;
                }
            }
            SetListeningMode(GetDefaultListeningMode());
        } else if (state == kDeviceStateSpeaking) {
            if (skip) {
                ESP_LOGI(TAG, "HandleExternalTextMessage: skip duplicate detect while speaking");
                return;
            }
            if (interrupt) {
                ESP_LOGI(TAG, "HandleExternalTextMessage: interrupt speaking for announce");
                PrepareAnnounceInterrupt();
                pending_text_to_send_ = text;
                last_external_detect_text_ = text;
                AbortSpeaking(kAbortReasonNone);
                while (audio_service_.PopPacketFromSendQueue())
                    ;
                audio_service_.ResetDecoder();
                if (protocol_) {
                    protocol_->SetPendingAudioDropped(true);
                }
                SetListeningMode(listening_mode_);
            } else {
                pending_text_to_send_ = text;
                last_external_detect_text_ = text;
            }
        } else if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
            if (skip) {
                ESP_LOGI(TAG, "HandleExternalTextMessage: skip duplicate detect");
                return;
            }
            if (interrupt) {
                ESP_LOGI(TAG, "HandleExternalTextMessage: inject announce while awake");
                PrepareAnnounceInterrupt();
            }
            if (state == kDeviceStateListening && protocol_ && protocol_->IsAudioChannelOpened()) {
                SendOrQueueExternalDetect(text);
            } else {
                pending_text_to_send_ = text;
                last_external_detect_text_ = text;
            }
        } else {
            ESP_LOGW(TAG, "HandleExternalTextMessage: device busy (state=%d), ignored",
                     static_cast<int>(state));
        }
    });
}

bool Application::CanEnterSleepMode() {
    if (GetDeviceState() != kDeviceStateIdle) {
        return false;
    }

    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        return false;
    }

    if (!audio_service_.IsIdle()) {
        return false;
    }

#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640
    if (MossCameraStream::GetInstance().IsArmed()) {
        return false;
    }
    if (FaceTracker::GetInstance().IsRunning()) {
        return false;
    }
#endif

    // Now it is safe to enter sleep mode
    return true;
}

void Application::RegisterChatRelayCallback(ChatRelayCallback callback) {
    chat_relay_callback_ = std::move(callback);
}

void Application::RelayChat(const std::string& event, const std::string& role,
                            const std::string& text) {
    if (!chat_relay_callback_) {
        return;
    }
    chat_relay_callback_(event, role, text, DeviceStateMachine::GetStateName(GetDeviceState()));
}

void Application::RegisterMcpBroadcastCallback(std::function<void(const std::string&)> callback) {
    mcp_broadcast_callback_ = std::move(callback);
}

void Application::SendMcpMessage(const std::string& payload) {
    // Always schedule to run in main task for thread safety
    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        if (mcp_broadcast_callback_) {
            mcp_broadcast_callback_(payload);
        }
    });
}

void Application::LoadDeviceAecFromStorage() {
#if CONFIG_USE_DEVICE_AEC
    const bool enabled = DeviceConfig::LocalAecEnabled();
    aec_mode_ = enabled ? kAecOnDeviceSide : kAecOff;
    audio_service_.EnableDeviceAec(enabled);
#endif
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
            case kAecOff:
                audio_service_.EnableDeviceAec(false);
#if CONFIG_ENABLE_VAD_INTERRUPT
                CancelVadInterruptTimer();
                vad_interrupt_armed_ = false;
#endif
                if (GetDeviceState() == kDeviceStateSpeaking &&
                    listening_mode_ != kListeningModeRealtime) {
                    audio_service_.EnableVoiceProcessing(false);
                }
                display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
                break;
            case kAecOnServerSide:
                audio_service_.EnableDeviceAec(false);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
            case kAecOnDeviceSide:
                audio_service_.EnableDeviceAec(true);
                display->ShowNotification(Lang::Strings::RTC_MODE_ON);
                break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) { audio_service_.PlaySound(sound); }

void Application::ResetProtocol() {
    Schedule([this]() {
        // Close audio channel if opened
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
        // Reset protocol
        protocol_.reset();
    });
}
