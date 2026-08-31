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
std::atomic<int> g_tts_downlink_logs{0};
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
            const auto state = GetDeviceState();
            if (state == kDeviceStateSpeaking) {
#if CONFIG_ENABLE_VAD_INTERRUPT
                if (listening_mode_ == kListeningModeRealtime) {
                    HoldSpeakingUplink();
                    UpdateSpeakingBargeIn();
                } else {
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                }
#else
                if (listening_mode_ != kListeningModeRealtime) {
                    while (audio_service_.PopPacketFromSendQueue())
                        ;
                } else {
                    SendUplinkFromQueue();
                }
#endif
            } else if (state != kDeviceStateListening || pending_listening_start_) {
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
            if (GetDeviceState() == kDeviceStateSpeaking &&
                listening_mode_ == kListeningModeRealtime) {
                UpdateSpeakingBargeIn();
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
                    if (protocol_) {
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
                    if (GetDeviceState() == kDeviceStateSpeaking) {
                        ESP_LOGI(TAG, "TTS stop -> listening (mode=%d)", listening_mode_);
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
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
#if CONFIG_ENABLE_VAD_INTERRUPT
                        last_tts_sentence_us_ = esp_timer_get_time();
                        CancelVadInterruptTimer();
                        vad_interrupt_armed_ = false;
                        vad_silence_started_us_ = 0;
                        barge_in_candidate_residual_ = 0;
                        barge_in_ratio_hits_ = 0;
#endif
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
        case kDeviceStateIdle:
#if CONFIG_ENABLE_VAD_INTERRUPT
            CancelVadInterruptTimer();
            vad_interrupt_armed_ = false;
            vad_silence_started_us_ = 0;
#endif
            display->SetStatus(Lang::Strings::STANDBY);
            display->ClearChatMessages();    // Clear messages first
            display->SetEmotion("neutral");  // Then set emotion (wechat mode checks child count)
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

#if CONFIG_ENABLE_VAD_INTERRUPT
            if (!keep_preroll) {
                FlushBargeInHold(false);
                while (audio_service_.PopPacketFromSendQueue())
                    ;
            }
#else
            while (audio_service_.PopPacketFromSendQueue())
                ;
#endif

            // Keep the AEC processor running across speaking -> listening
            // (realtime). Only wait for drain when we have to start it fresh.
            // Barge-in must re-send listen start even when the processor stays up.
            if (play_popup_on_listening_ || !audio_service_.IsAudioProcessorRunning() ||
                keep_preroll) {
                // Pending text-chat (reminder /chat/say) must send listen/start
                // then detect; deferring until drain used to skip start and leave
                // the first TTS with MQTT text but no UDP audio.
                if (listening_mode_ == kListeningModeAutoStop && !audio_service_.IsPlaybackIdle() &&
                    pending_text_to_send_.empty()) {
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
#if CONFIG_ENABLE_VAD_INTERRUPT
            speaking_started_us_ = esp_timer_get_time();
            last_tts_sentence_us_ = speaking_started_us_;
            audio_service_.ResetEchoProfile();
#endif

            if (listening_mode_ != kListeningModeRealtime) {
                // Half duplex (local AEC off): mute uplink during TTS. Do not arm
                // VAD barge-in here; speaker echo is often mis-detected as speech.
                audio_service_.EnableVoiceProcessing(false);
                audio_service_.EnableWakeWordDetection(audio_service_.IsAfeWakeWord());
            }
#if CONFIG_ENABLE_VAD_INTERRUPT
            CancelVadInterruptTimer();
            vad_interrupt_armed_ = false;
            vad_silence_started_us_ = 0;
            last_barge_in_reject_us_ = 0;
            barge_in_candidate_residual_ = 0;
            barge_in_ratio_hits_ = 0;
            FlushBargeInHold(false);
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
    barge_in_ratio_hits_ = 0;
    audio_service_.SetPlaybackMuted(false);
    audio_service_.SetPlaybackDuckQ8(256);
}

void Application::UpdateSpeakingBargeIn() {
    if (GetDeviceState() != kDeviceStateSpeaking || listening_mode_ != kListeningModeRealtime) {
        return;
    }

    const bool timer_active =
        vad_interrupt_timer_ != nullptr && esp_timer_is_active(vad_interrupt_timer_);
    const bool speech = audio_service_.IsVoiceDetected();
    const int64_t now = esp_timer_get_time();

    if (!speech) {
        // A 60ms VAD dip must not restart the confirm window; confirm re-checks.
        if (timer_active) {
            return;
        }
        CancelVadInterruptTimer();
        if (audio_service_.PlaybackLevel() >= 200) {
            // VAD gap while the PA is still loud is echo NLP, not a user pause.
            vad_silence_started_us_ = 0;
            vad_interrupt_armed_ = false;
            return;
        }
        if (vad_silence_started_us_ == 0) {
            vad_silence_started_us_ = now;
        }
        // A real barge-in usually starts after a pause. Echo from a loud PA
        // keeps VAD in SPEECH the whole TTS, so this arm never latches.
        if (now - vad_silence_started_us_ >= 120 * 1000) {
            vad_interrupt_armed_ = true;
        }
        return;
    }

    vad_silence_started_us_ = 0;
    if (timer_active) {
        return;
    }
    const int pb = audio_service_.PlaybackLevel();
    const int res = audio_service_.ResidualLevel();
    const int pct = pb > 0 ? (int)((int64_t)res * 100 / pb) : 0;
    // Soft duck (~40%) in the 16–20% band so NLP eases before the hard mute.
    // Steady echo is ~9–13%; ducking at 12% kept TTS at 40% and still pumped.
    if (!audio_service_.IsPlaybackMuted()) {
        if (pb >= 200 && res >= 180 && pct >= 16) {
            audio_service_.SetPlaybackDuckQ8(102);
        } else if (pct < 13) {
            audio_service_.SetPlaybackDuckQ8(256);
        }
    }
    if (!audio_service_.IsConfirmedNearEndSpeech()) {
        barge_in_ratio_hits_ = 0;
        return;
    }
    if (barge_in_ratio_hits_ < 1) {
        barge_in_ratio_hits_ = 1;
        return;
    }
    MaybeStartVadInterruptTimer(!vad_interrupt_armed_);
}

void Application::MaybeStartVadInterruptTimer(bool unarmed_path) {
    constexpr int64_t kVadInterruptGuardUs = 350 * 1000;
    if (GetDeviceState() != kDeviceStateSpeaking || protocol_ == nullptr) {
        return;
    }
    if (audio_service_.IsPlaybackIdle()) {
        return;
    }
    if (!audio_service_.IsConfirmedNearEndSpeech()) {
        return;
    }
    if (vad_interrupt_timer_ == nullptr) {
        return;
    }
    if (esp_timer_is_active(vad_interrupt_timer_)) {
        return;
    }
    constexpr int64_t kRejectCooldownUs = 400 * 1000;
    if (last_barge_in_reject_us_ > 0 &&
        esp_timer_get_time() - last_barge_in_reject_us_ < kRejectCooldownUs) {
        return;
    }

    const int64_t elapsed = esp_timer_get_time() - speaking_started_us_;
    int64_t extra_guard_us = 0;
    if (elapsed < kVadInterruptGuardUs) {
        extra_guard_us = kVadInterruptGuardUs - elapsed;
    }
    if (!audio_service_.EchoProfileReady() &&
        audio_service_.ResidualLevel() <= audio_service_.PlaybackLevel()) {
        extra_guard_us = std::max(extra_guard_us, int64_t{80 * 1000});
    }

    // Echo spikes often follow a new TTS sentence; AEC also re-adapts here.
    constexpr int64_t kPostTtsSentenceGuardUs = 280 * 1000;
    const int64_t since_sentence_us = esp_timer_get_time() - last_tts_sentence_us_;
    if (since_sentence_us < kPostTtsSentenceGuardUs) {
        extra_guard_us = std::max(extra_guard_us, kPostTtsSentenceGuardUs - since_sentence_us);
    }

    int volume = 70;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        volume = codec->output_volume();
    }

    // Mute PA as soon as we suspect near-end so NLP stops crushing the uplink.
    // Confirm still waits; a false echo spike dies once the speaker is silent.
    int64_t sustain_ms = unarmed_path ? 120 : 80;
    if (volume >= 80) {
        sustain_ms = unarmed_path ? 140 : 100;
    } else if (volume >= 60) {
        sustain_ms = unarmed_path ? 130 : 90;
    }

    const int pb = audio_service_.PlaybackLevel();
    const int res = audio_service_.ResidualLevel();
    const int pct = pb > 0 ? (int)((int64_t)res * 100 / pb) : 0;
    ESP_LOGI(TAG, "VAD barge-in candidate (%s, vol=%d pb=%d res=%d pct=%d), confirming in %d ms",
             unarmed_path ? "overlap" : "armed", volume, pb, res, pct,
             (int)(sustain_ms + extra_guard_us / 1000));
    barge_in_candidate_residual_ = audio_service_.ResidualLevel();
    audio_service_.SetPlaybackMuted(true);
    esp_timer_start_once(vad_interrupt_timer_, sustain_ms * 1000 + extra_guard_us);
}

void Application::HandleVadInterruptConfirm() {
    if (GetDeviceState() != kDeviceStateSpeaking || protocol_ == nullptr) {
        audio_service_.SetPlaybackMuted(false);
        return;
    }
    const int residual = audio_service_.ResidualLevel();
    const int playback = audio_service_.PlaybackLevel();
    const int pct = playback > 0 ? (int)((int64_t)residual * 100 / playback) : 0;
    auto reject = [this, residual, playback, pct](const char* why) {
        last_barge_in_reject_us_ = esp_timer_get_time();
        barge_in_candidate_residual_ = 0;
        audio_service_.SetPlaybackMuted(false);
        ESP_LOGI(TAG, "VAD barge-in dropped (%s: pb=%d res=%d pct=%d)", why, playback, residual,
                 pct);
    };
    if (!audio_service_.IsVoiceDetected() || audio_service_.IsPlaybackIdle()) {
        reject("idle");
        return;
    }
    if (!audio_service_.IsConfirmedNearEndSpeech()) {
        reject("echo");
        return;
    }
    // Echo residual falls after mute (437 → 314). Real barge-in rose
    // (267 → 305, 482 → 789). Apply spike reject even while the PA is muted.
    if (barge_in_candidate_residual_ > 0 && residual * 4 < barge_in_candidate_residual_ * 3) {
        reject("spike");
        return;
    }
    ESP_LOGI(TAG, "VAD barge-in confirmed (pb=%d res=%d pct=%d)", playback, residual, pct);
    barge_in_flush_residual_ = residual;
    barge_in_candidate_residual_ = 0;
    HoldSpeakingUplink();
    AbortSpeaking(kAbortReasonVadInterrupt);
    // Stop TTS immediately; keep send-queue pre-roll for ASR onset.
    audio_service_.ResetDecoder();
    barge_in_listen_ = true;
    SetListeningMode(GetDefaultListeningMode());
}

void Application::HoldSpeakingUplink() {
    // Keep the last ~1.9s of post-AEC frames. Do not require the near-end
    // energy gate here: onset of "你会…" is often below the barge-in threshold
    // and would be dropped, so ASR only hears "学会些什么".
    constexpr size_t kBargeInHoldMaxPackets = 32;
    while (auto packet = audio_service_.PopPacketFromSendQueue()) {
        barge_in_hold_.push_back(std::move(packet));
        while (barge_in_hold_.size() > kBargeInHoldMaxPackets) {
            barge_in_hold_.pop_front();
        }
    }
}

void Application::FlushBargeInHold(bool send) {
    if (send && protocol_) {
        HoldSpeakingUplink();
        if (!barge_in_hold_.empty()) {
            // Hit is the loud part after NLP. The first syllables sit below
            // onset_need ("好的拜拜"→"拜拜", "不是丫丫"→"不丫丫"). Walk back to
            // the echo floor, then pad. Cap at 20 packets so we do not dump
            // 1.9s of TTS leak again.
            constexpr int kOnsetPadPackets = 8;
            constexpr size_t kMaxSendPackets = 20;
            constexpr size_t kFallbackPackets = 16;
            const size_t n = barge_in_hold_.size();
            const size_t head = std::max<size_t>(1, n / 4);
            int echo_est = 0;
            for (size_t i = 0; i < head; ++i) {
                echo_est += barge_in_hold_[i]->residual;
            }
            echo_est /= static_cast<int>(head);
            int onset_need = echo_est + std::max(echo_est, 80);
            if (barge_in_flush_residual_ > 0) {
                const int mid = (echo_est + barge_in_flush_residual_) / 2;
                const int cap = std::max(echo_est + 40, barge_in_flush_residual_ * 2 / 3);
                onset_need = std::max(onset_need, mid);
                onset_need = std::min(onset_need, cap);
            }
            const size_t earliest = n > kMaxSendPackets ? n - kMaxSendPackets : 0;
            size_t hit = n;
            for (size_t i = earliest; i < n; ++i) {
                if (barge_in_hold_[i]->residual >= onset_need) {
                    hit = i;
                    break;
                }
            }
            size_t start;
            if (hit >= n) {
                start = n > kFallbackPackets ? n - kFallbackPackets : 0;
            } else {
                const int back_floor = echo_est + std::max(echo_est / 4, 20);
                size_t speech_at = hit;
                while (speech_at > earliest &&
                       barge_in_hold_[speech_at - 1]->residual > back_floor) {
                    speech_at--;
                }
                start = speech_at > static_cast<size_t>(kOnsetPadPackets)
                            ? speech_at - kOnsetPadPackets
                            : 0;
                if (start < earliest) {
                    start = earliest;
                }
            }
            ESP_LOGI(TAG, "barge-in preroll %u/%u packets (echo_est=%d need=%d hit=%u)",
                     (unsigned)(n - start), (unsigned)n, echo_est, onset_need, (unsigned)hit);
            for (size_t i = start; i < n; ++i) {
                if (!protocol_->SendAudio(std::move(barge_in_hold_[i]))) {
                    break;
                }
            }
        }
    }
    barge_in_hold_.clear();
    barge_in_flush_residual_ = 0;
}

#endif

void Application::SendUplinkFromQueue() {
    const auto state = GetDeviceState();
    const bool voice_only =
        state == kDeviceStateSpeaking && listening_mode_ == kListeningModeRealtime;
    while (auto packet = audio_service_.PopPacketFromSendQueue()) {
        if (voice_only && !audio_service_.IsVoiceDetected()) {
            continue;
        }
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
#if CONFIG_ENABLE_VAD_INTERRUPT
    // Cloud drops uplink that arrives before listen-start.
    FlushBargeInHold(true);
#endif

    ConfigureWakeWordForListening();

    if (play_popup_on_listening_) {
        play_popup_on_listening_ = false;
    }

    if (!pending_text_to_send_.empty()) {
        std::string text = pending_text_to_send_;
        pending_text_to_send_.clear();
        ESP_LOGI(TAG, "StartListeningAudio: sending pending text: %s", text.c_str());
        if (protocol_) {
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

void Application::SetPendingAnnounce(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_announce_ = text;
}

void Application::PushPendingAnnounce(const std::string& text) {
    if (text.empty()) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_announce_.empty()) {
        pending_announce_ = text;
        return;
    }
    if (pending_announce_ != text) {
        pending_announce_ += "\n";
        pending_announce_ += text;
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
    return text;
}

std::string Application::AckPendingAnnounce() {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_announce_.clear();
    return "已播报";
}

void Application::HandleExternalTextMessage(const std::string& text, const std::string& announce) {
    if (text.empty()) {
        return;
    }

    if (!announce.empty()) {
        PushPendingAnnounce(announce);
    }

    Schedule([this, text]() {
        auto state = GetDeviceState();

        if (state == kDeviceStateIdle) {
            if (!protocol_) {
                ESP_LOGE(TAG, "Protocol not initialized");
                return;
            }
            // Keep pending text until StartListeningAudio (listen/start then detect).
            pending_text_to_send_ = text;
            listening_mode_ = kListeningModeAutoStop;
            Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);
            if (!protocol_->IsAudioChannelOpened()) {
                SetDeviceState(kDeviceStateConnecting);
                if (!protocol_->OpenAudioChannel()) {
                    SetDeviceState(kDeviceStateIdle);
                    pending_text_to_send_.clear();
                    last_error_message_ = Lang::Strings::SERVER_NOT_CONNECTED;
                    xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
                    return;
                }
            }
            SetListeningMode(kListeningModeAutoStop);
        } else if (state == kDeviceStateSpeaking) {
            // Queue for the next listening cycle; do not abort in-flight TTS (breaks playback).
            pending_text_to_send_ = text;
        } else if (state == kDeviceStateListening || state == kDeviceStateConnecting) {
            if (protocol_ && protocol_->IsAudioChannelOpened()) {
                protocol_->SendTextChat(text);
            } else {
                pending_text_to_send_ = text;
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
