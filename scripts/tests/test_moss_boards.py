import json
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


class MossBoardIdentityTests(unittest.TestCase):
    def _config(self, board_dir: str) -> dict:
        path = ROOT / "main/boards" / board_dir / "config.json"
        return json.loads(path.read_text(encoding="utf-8"))

    def test_config_type_matches_directory_and_build_name(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            config = self._config(f"moss/{leaf}")
            self.assertEqual(config["type"], leaf)
            names = [item["name"] for item in config["builds"]]
            self.assertEqual(names, [leaf])

    def test_board_types_are_unique(self):
        types = []
        for path in (ROOT / "main/boards").rglob("config.json"):
            config = json.loads(path.read_text(encoding="utf-8"))
            types.append(config["type"])
        self.assertEqual(len(types), len(set(types)))
        self.assertEqual(set(types), {"moss-onvif", "moss-ov2640"})

    def test_both_boards_use_usb_serial_jtag_console(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            append = "\n".join(self._config(f"moss/{leaf}")["builds"][0]["sdkconfig_append"])
            self.assertIn("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", append)

    def test_both_boards_enable_custom_wake_word(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            append = "\n".join(self._config(f"moss/{leaf}")["builds"][0]["sdkconfig_append"])
            self.assertIn("CONFIG_USE_CUSTOM_WAKE_WORD=y", append)

    def test_both_boards_enable_device_aec(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            append = "\n".join(self._config(f"moss/{leaf}")["builds"][0]["sdkconfig_append"])
            self.assertIn("CONFIG_USE_DEVICE_AEC=y", append)

    def test_partition_tables_differ(self):
        onvif = ROOT / "partitions/moss-desktop-16m.csv"
        ov2640 = ROOT / "partitions/v2/16m_moss_desktop.csv"
        self.assertTrue(onvif.exists())
        self.assertTrue(ov2640.exists())
        self.assertNotEqual(onvif.read_text(encoding="utf-8"), ov2640.read_text(encoding="utf-8"))
        onvif_cfg = "\n".join(self._config("moss/moss-onvif")["builds"][0]["sdkconfig_append"])
        ov_cfg = "\n".join(self._config("moss/moss-ov2640")["builds"][0]["sdkconfig_append"])
        self.assertIn("partitions/moss-desktop-16m.csv", onvif_cfg)
        self.assertIn("partitions/v2/16m_moss_desktop.csv", ov_cfg)

    def test_wake_and_mic_defaults_are_shared(self):
        shared = (ROOT / "main/boards/moss/moss_shared_audio.h").read_text(encoding="utf-8")
        self.assertIn("#define AUDIO_CODEC_INPUT_GAIN 37.5f", shared)
        self.assertIn("#define AUDIO_CODEC_REFERENCE_GAIN 30.0f", shared)
        self.assertIn("#define AUDIO_CODEC_REFERENCE_CHANNEL 2", shared)
        for leaf in ("moss-onvif", "moss-ov2640"):
            header = (ROOT / "main/boards" / f"moss/{leaf}" / "config.h").read_text(encoding="utf-8")
            self.assertIn('#include "moss_shared_audio.h"', header)
            self.assertNotIn("#define AUDIO_CODEC_INPUT_GAIN", header)
            self.assertNotIn("#define AUDIO_CODEC_REFERENCE_GAIN", header)
            append = "\n".join(self._config(f"moss/{leaf}")["builds"][0]["sdkconfig_append"])
            self.assertIn("CONFIG_CUSTOM_WAKE_WORD_THRESHOLD=20", append)
            self.assertIn('CONFIG_CUSTOM_WAKE_WORD="mo si"', append)
            self.assertIn("CONFIG_USE_DEVICE_AEC=y", append)

    def test_both_boards_pass_shared_codec_gains_not_zero_reference(self):
        onvif = (ROOT / "main/boards/moss/moss-onvif/moss_onvif_board.cc").read_text(encoding="utf-8")
        ov2640 = (ROOT / "main/boards/moss/moss-ov2640/moss_ov2640_board.cc").read_text(encoding="utf-8")
        for src in (onvif, ov2640):
            self.assertIn("AUDIO_CODEC_INPUT_GAIN", src)
            self.assertIn("AUDIO_CODEC_REFERENCE_CHANNEL", src)
            self.assertIn("AUDIO_CODEC_REFERENCE_GAIN", src)
        self.assertNotIn("AUDIO_CODEC_INPUT_GAIN, 2, 0.0f", ov2640)

    def test_ov2640_release_playback_keeps_i2s_while_mic_on(self):
        src = (ROOT / "main/audio/audio_codec.cc").read_text(encoding="utf-8")
        start = src.find("void MossDesktopReleasePlayback")
        self.assertGreater(start, 0)
        body = src[start : src.find("AudioCodec::AudioCodec", start)]
        self.assertIn("MossDesktopSetNs4150Pa(false)", body)
        self.assertIn("MossDesktopSharedI2cHeld()", body)
        self.assertIn("duplex()", body)
        self.assertIn("codec->EnableOutput(false)", body)
        self.assertLess(body.find("if (codec->duplex())"), body.find("codec->EnableOutput(false)"))
        self.assertNotIn("input_enabled()", body)

    def test_ov2640_camera_holds_shared_i2c_during_dvp(self):
        codec = (ROOT / "main/audio/audio_codec.cc").read_text(encoding="utf-8")
        self.assertIn("void MossDesktopHoldSharedI2c", codec)
        box = (ROOT / "main/audio/codecs/box_audio_codec.cc").read_text(encoding="utf-8")
        self.assertIn("MossDesktopSharedI2cHeld()", box)
        board = (ROOT / "main/boards/moss/moss-ov2640/moss_ov2640_board.cc").read_text(
            encoding="utf-8"
        )
        cap = board.find("bool Capture() override")
        self.assertGreater(cap, 0)
        cap_body = board[cap : board.find("bool SetHMirror", cap)]
        self.assertIn("MossDesktopSharedI2cHold", cap_body)
        track = board.find("bool AcquireTracking() override")
        track_body = board[track : board.find("bool IsTrackingAcquired", track)]
        self.assertIn("MossDesktopHoldSharedI2c(true)", track_body)
        mcp = (ROOT / "main/mcp_server.cc").read_text(encoding="utf-8")
        self.assertIn("MossDesktopHoldSharedI2c(true)", mcp)

    def test_both_boards_keep_i2s_tx_while_mic_on_for_wake(self):
        src = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        start = src.find("void AudioService::CheckAndUpdateAudioPowerState")
        self.assertGreater(start, 0)
        body = src[start : src.find("void AudioService::SetModelsList", start)]
        self.assertIn("MossDesktopReleasePlayback(codec_)", body)
        self.assertIn("MossDesktopSharedI2cHeld()", body)
        self.assertIn("Keep TX clock when duplex RX is active", body)
        self.assertIn("duplex() && codec_->input_enabled()", body)
        ov2640 = body[body.find("CONFIG_BOARD_TYPE_MOSS_OV2640") : body.find("#else")]
        onvif = body[body.find("#else") :]
        self.assertIn("MossDesktopReleasePlayback", ov2640)
        self.assertNotIn("EnableOutput(false)", ov2640)
        self.assertIn("EnableOutput(false)", onvif)
        self.assertLess(
            onvif.find("duplex() && codec_->input_enabled()"),
            onvif.find("EnableOutput(false)"),
        )

    def test_ov2640_unmutes_pa_even_when_i2s_already_on(self):
        service = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        out_start = service.find("void AudioService::AudioOutputTask")
        self.assertGreater(out_start, 0)
        out_body = service[out_start : service.find("void AudioService::", out_start + 1)]
        pa = out_body.find("MossDesktopPreparePlayback(codec_)")
        self.assertGreater(pa, 0)
        opened = out_body.find("if (!codec_->output_enabled())")
        self.assertGreater(opened, 0)
        self.assertGreater(pa, out_body.find("}", opened))

        reset_start = service.find("void AudioService::ResetDecoder")
        reset_body = service[reset_start : service.find("bool AudioService::IsPlaybackDrainedLocked", reset_start)]
        self.assertNotIn("MossDesktopReleasePlayback", reset_body)

        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        tts = app.find('if (strcmp(state->valuestring, "start") == 0)')
        tts_body = app[tts : app.find('else if (strcmp(state->valuestring, "stop") == 0)', tts)]
        self.assertIn("MossDesktopPreparePlayback(codec)", tts_body)
        self.assertNotIn("codec != nullptr && !codec->output_enabled()", tts_body)

    def test_onvif_reasserts_pa_even_when_i2s_already_on(self):
        service = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        out_start = service.find("void AudioService::AudioOutputTask")
        self.assertGreater(out_start, 0)
        out_body = service[out_start : service.find("void AudioService::", out_start + 1)]
        else_body = out_body[out_body.find("#else") : out_body.find("#endif")]
        self.assertIn("PreparePlayback()", else_body)
        self.assertNotIn("if (!codec_->output_enabled())", else_body)

        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        tts = app.find('if (strcmp(state->valuestring, "start") == 0)')
        tts_body = app[tts : app.find('else if (strcmp(state->valuestring, "stop") == 0)', tts)]
        self.assertIn("PreparePlayback()", tts_body)

        box = (ROOT / "main/audio/codecs/box_audio_codec.cc").read_text(encoding="utf-8")
        self.assertIn("void BoxAudioCodec::PreparePlayback()", box)
        self.assertIn("ApplyPaPin(true)", box)
        self.assertIn("PA %s (GPIO%d)", box)

    def test_ov2640_camera_guard_restores_mic_during_tts(self):
        src = (ROOT / "main/mcp_server.cc").read_text(encoding="utf-8")
        start = src.find("class MossCameraVoiceGuard")
        self.assertGreater(start, 0)
        body = src[start : src.find("}  // namespace", start)]
        self.assertNotIn("Do not reopen the mic during TTS", body)
        dtor = body[body.find("~MossCameraVoiceGuard") :]
        self.assertIn("kDeviceStateSpeaking", dtor)
        self.assertIn("EnableInput(true)", dtor)
        self.assertIn("EnableVoiceProcessing(true)", dtor)
        self.assertIn("voice_session", dtor)
        take = src.find('AddTool("self.camera.take_photo"')
        self.assertGreater(take, 0)
        take_body = src[take : src.find("AddUserOnlyTools", take)]
        capture = take_body.find("camera->Capture()")
        explain = take_body.find("camera->Explain")
        guard = take_body.find("MossCameraVoiceGuard voice_guard")
        self.assertGreater(capture, 0)
        self.assertGreater(explain, 0)
        self.assertGreater(guard, 0)
        self.assertLess(guard, capture)
        self.assertLess(capture, explain)

    def test_apply_wake_word_does_not_touch_codec_volume(self):
        src = (ROOT / "main/audio/wake_words/custom_wake_word.cc").read_text(encoding="utf-8")
        start = src.find("bool CustomWakeWord::ApplyConfig")
        self.assertGreater(start, 0)
        body = src[start : src.find("bool CustomWakeWord::", start + 1)]
        if not body.strip():
            body = src[start:]
        self.assertIn("SaveStoredConfig", body)
        self.assertIn("set_det_threshold", body)
        self.assertNotIn("SetOutputVolume", body)
        self.assertNotIn("input_gain", body)


class MossCmakeSourceIsolationTests(unittest.TestCase):
    def setUp(self):
        self.cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")

    def test_camera_sources_are_ov2640_only(self):
        family_start = self.cmake.find("if(CONFIG_BOARD_FAMILY_MOSS)")
        self.assertGreaterEqual(family_start, 0)
        family = self.cmake[family_start:]
        camera_sources = (
            "camera_handlers.cc",
            "face_tracker.cc",
            "stepper_gimbal.cc",
            "moss_camera_stream.cc",
        )
        ov_start = family.find("if(CONFIG_BOARD_TYPE_MOSS_OV2640)")
        onvif_start = family.find("if(CONFIG_BOARD_TYPE_MOSS_ONVIF)")
        self.assertGreaterEqual(ov_start, 0)
        self.assertGreaterEqual(onvif_start, 0)
        ov2640 = family[ov_start:family.find("endif()", ov_start)]
        onvif = family[onvif_start:family.find("endif()", onvif_start)]
        for source in camera_sources:
            self.assertNotIn(source, onvif, source)
            self.assertIn(source, ov2640, source)
        self.assertIn("device/lamp_bar.cc", onvif)
        self.assertIn("device/ov2640/lamp_bar.cc", ov2640)
        self.assertNotIn("mcp/tools/*.cc", family)
        self.assertIn("config/product.cc", family)
        self.assertIn("mcp/tools/yunxiangji.cc", family)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/boards/moss", family)
        self.assertNotIn("74hc595_driver.cc", family)
        self.assertNotIn("pca9685_driver.cc", family)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/mcp/*.cc", self.cmake)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/drivers/*.cc", self.cmake)
        self.assertIn('if(IS_DIRECTORY "${_board_inc_dir}")', self.cmake)


class MossBoardMcpLayoutTests(unittest.TestCase):
    def test_yunxiangji_registers_query_and_ack_tools(self):
        src = (ROOT / "main/mcp/tools/yunxiangji.cc").read_text(encoding="utf-8")
        self.assertIn("self.yunxiangji.take", src)
        self.assertIn("self.yunxiangji.ack", src)
        self.assertIn("TakePendingAnnounce", src)
        self.assertIn("AckPendingAnnounce", src)
        self.assertIn("static bool registered = false", src)
        self.assertIn("void RegisterYunxiangjiTools()", src)
        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn("RegisterYunxiangjiTools()", app)
        invoke = app.find("void Application::ContinueWakeWordInvoke")
        self.assertGreater(invoke, 0)
        body = app[invoke : app.find("void Application::HandleStateChangedEvent", invoke)]
        self.assertIn("pending_text_to_send_", body)
        self.assertIn("pending text waits for listen/start", body)
        self.assertNotIn("SendTextChat(text)", body)

    def test_shared_mcp_stays_in_mcp_tools(self):
        tools = ROOT / "main/mcp/tools"
        for name in (
            "lamp_bar.cc",
            "lamp_panel.cc",
            "lamp_eye.cc",
            "eye_motor.cc",
            "infrared.cc",
            "api_server.cc",
            "yunxiangji.cc",
        ):
            self.assertTrue((tools / name).is_file(), name)
        self.assertFalse((tools / "gimbal.cc").exists())
        self.assertFalse((tools / "face_track.cc").exists())

    def test_ov2640_owns_gimbal_mcp_and_raw_595(self):
        board = ROOT / "main/boards/moss/moss-ov2640"
        self.assertTrue((board / "mcp/gimbal.cc").is_file())
        self.assertTrue((board / "mcp/face_track.cc").is_file())
        driver = (board / "drivers/74hc595_driver.cc").read_text(encoding="utf-8")
        self.assertNotIn("panel_state_", driver)
        self.assertNotIn("0x1F", driver)
        self.assertIn("SetOutputs", driver)

    def test_onvif_owns_lamp_merge_595(self):
        board = ROOT / "main/boards/moss/moss-onvif"
        self.assertFalse((board / "mcp").exists())
        driver = (board / "drivers/74hc595_driver.cc").read_text(encoding="utf-8")
        self.assertIn("panel_state_", driver)
        self.assertIn("0x1F", driver)
        self.assertNotIn("raw_outputs_", driver)

    def test_shared_utils_no_longer_hold_board_drivers(self):
        utils = ROOT / "main/mcp/utils"
        self.assertFalse((utils / "74hc595_driver.cc").exists())
        self.assertFalse((utils / "pca9685_driver.cc").exists())
        self.assertTrue((ROOT / "main/boards/moss/moss-ov2640/drivers/pca9685_driver.cc").is_file())

    def test_common_lamp_mcp_does_not_name_595(self):
        bar = (ROOT / "main/mcp/tools/lamp_bar.cc").read_text(encoding="utf-8")
        panel = (ROOT / "main/mcp/tools/lamp_panel.cc").read_text(encoding="utf-8")
        self.assertNotIn("74HC595", bar)
        self.assertNotIn("74HC595", panel)


class BoxAudioCodecTests(unittest.TestCase):
    def test_enable_input_does_not_abort_on_i2c_nack(self):
        src = (ROOT / "main/audio/codecs/box_audio_codec.cc").read_text(encoding="utf-8")
        start = src.find("void BoxAudioCodec::EnableInput")
        end = src.find("void BoxAudioCodec::EnableOutput")
        self.assertGreater(start, 0)
        self.assertGreater(end, start)
        body = src[start:end]
        self.assertNotIn("ESP_ERROR_CHECK(esp_codec_dev_open(input_dev_", body)
        self.assertIn("retry", body)
        self.assertIn("ESP_ERROR_CHECK_WITHOUT_ABORT", body)


class MossLanControlPlaneTests(unittest.TestCase):
    def test_cmake_has_lan_control_sources_and_no_desktop_mqtt_client(self):
        cmake = (ROOT / "main/CMakeLists.txt").read_text(encoding="utf-8")
        for source in (
            "api/methods/hw/hw_handlers.cc",
            "api/methods/chat/chat_handlers.cc",
            "config/moss_chat_log.cc",
            "config/moss_hw.cc",
        ):
            self.assertIn(source, cmake)
        self.assertNotIn("external_mqtt_client.cc", cmake)

    def test_leave_bind_mode_keeps_http(self):
        src = (ROOT / "main/config/moss_config_service.cc").read_text(encoding="utf-8")
        start = src.find("void MossConfigService::LeaveBindMode")
        self.assertGreater(start, 0)
        end = src.find("void MossConfigService::StartMdns", start)
        body = src[start:end]
        self.assertIn("StartLanServices()", body)
        self.assertNotIn("Stop(", body)
        self.assertNotIn("StopMdns", body)

    def test_health_exposes_voice_and_chat_seq_from_ram(self):
        src = (ROOT / "main/api/methods/config/config_handlers.cc").read_text(encoding="utf-8")
        start = src.find("esp_err_t HandleHealth")
        end = src.find("esp_err_t HandleMqttGet", start)
        body = src[start:end]
        self.assertIn("voice", body)
        self.assertIn("chat_seq", body)
        self.assertIn("CachedMac()", body)
        self.assertIn("MossChatLog::GetInstance().Seq()", body)
        self.assertNotIn("ExtMqttSettings::Load()", body)

    def test_chat_and_hw_routes_registered(self):
        src = (ROOT / "main/api/api.cc").read_text(encoding="utf-8")
        self.assertIn('add("/hw", HTTP_GET', src)
        self.assertIn('add("/hw", HTTP_POST', src)
        self.assertIn('add("/chat/wake", HTTP_POST', src)
        self.assertIn('add("/chat/say", HTTP_POST', src)
        self.assertIn('add("/chat/sync", HTTP_GET', src)
        self.assertIn("max_uri_handlers = 32", src)
        self.assertIn("max_open_sockets = 7", src)


class MossOnvifControlSurfaceTests(unittest.TestCase):
    def test_onvif_shares_hw_chat_ir_and_does_not_register_camera(self):
        src = (ROOT / "main/api/api.cc").read_text(encoding="utf-8")
        start_fn = src.find("bool ApiServer::Start")
        self.assertGreater(start_fn, 0)
        start = src[start_fn:]
        cam_if = start.find("#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640")
        self.assertGreater(cam_if, 0)
        shared = start[:cam_if]
        ov2640 = start[cam_if:]
        for route in (
            '"/health"',
            '"/config/device"',
            '"/ir/devices"',
            '"/ir/send"',
            '"/hw"',
            '"/chat/wake"',
            '"/chat/say"',
            '"/chat/sync"',
        ):
            self.assertIn(route, shared, route)
        for route in ("/camera/stream", "/gimbal/control", "/face_track/control"):
            self.assertNotIn(route, shared, route)
            self.assertIn(route, ov2640, route)

    def test_chat_say_keeps_announce_local_and_cloud_text_short(self):
        src = (ROOT / "main/api/methods/chat/chat_handlers.cc").read_text(encoding="utf-8")
        start = src.find("esp_err_t HandleChatSay")
        body = src[start : src.find("esp_err_t HandleChatSync", start)]
        self.assertIn('"announce"', body)
        self.assertIn("HandleExternalTextMessage(text, announce)", body)
        app = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        fn = app.find("void Application::HandleExternalTextMessage")
        self.assertGreater(fn, 0)
        end = app.find("bool Application::CanEnterSleepMode", fn)
        handle = app[fn:end]
        self.assertIn("PushPendingAnnounce(announce)", handle)
        self.assertNotIn("SetPendingAnnounce(text)", handle)
        self.assertIn("pending_text_to_send_ = text", handle)
        self.assertIn("skip duplicate detect", handle)
        self.assertIn("ShouldSkipExternalDetect", handle)
        self.assertIn("interrupt speaking for announce", handle)
        self.assertIn("inject announce while awake", handle)
        self.assertIn("AbortSpeaking", handle)
        self.assertIn("ResetDecoder", handle)
        self.assertNotIn("CONFIG_BOARD_TYPE_MOSS", handle)
        app_all = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        self.assertIn("drop duplicate reminder TTS", app_all)
        self.assertIn("SuppressAnnounceReplay()", app_all)
        self.assertIn("IsAnnounceReplaySuppressed()", app_all)
        incoming = app_all[
            app_all.find("protocol_->OnIncomingAudio") : app_all.find(
                "protocol_->OnAudioChannelOpened"
            )
        ]
        self.assertLess(
            incoming.find("IsAnnounceReplaySuppressed()"),
            incoming.find("IsPendingAudioDropped()"),
        )
        self.assertIn("skip already-sent detect", app_all)
        self.assertIn("TakePendingAnnounce", app_all)
        self.assertIn("last_taken_announce_", app_all)

    def test_idle_wake_reports_connecting_not_stale_idle(self):
        src = (ROOT / "main/api/methods/chat/chat_handlers.cc").read_text(encoding="utf-8")
        start = src.find("esp_err_t HandleChatWake")
        body = src[start : src.find("esp_err_t HandleChatSay", start)]
        self.assertIn('reported = "connecting"', body)
        self.assertIn("kDeviceStateIdle", body)
        self.assertIn("RequestChatWake", body)

    def test_device_config_brightness_uses_saved_not_idle_zero(self):
        src = (ROOT / "main/config/device_config.cc").read_text(encoding="utf-8")
        self.assertIn("SavedBrightness()", src)
        self.assertNotIn("backlight->brightness()", src)
        header = (ROOT / "main/boards/common/backlight.h").read_text(encoding="utf-8")
        self.assertIn("SavedBrightness() const", header)

    def test_device_config_exposes_press_to_talk_and_aec(self):
        src = (ROOT / "main/config/device_config.cc").read_text(encoding="utf-8")
        self.assertIn("press_to_talk", src)
        self.assertIn("local_aec_supported", src)
        self.assertIn("kNvsPressToTalk", src)

    def test_press_to_talk_button_reads_nvs_not_stale_member(self):
        src = (ROOT / "main/boards/common/press_to_talk_mcp_tool.cc").read_text(encoding="utf-8")
        start = src.find("bool PressToTalkMcpTool::IsPressToTalkEnabled")
        self.assertGreater(start, 0)
        body = src[start : src.find("\n}", start) + 2]
        self.assertIn('GetInt("press_to_talk"', body)
        self.assertNotIn("return press_to_talk_enabled_;", body)

    def test_hw_apply_covers_onvif_peripherals(self):
        src = (ROOT / "main/config/moss_hw.cc").read_text(encoding="utf-8")
        for device in ('"eye"', '"bar"', '"panel"', '"bottom"', '"motor"', '"all"'):
            self.assertIn(device, src, device)
        self.assertIn('result.message = "unknown device"', src)


class MossBargeInTests(unittest.TestCase):
    def test_realtime_tts_holds_uplink_instead_of_sending_echo(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = src.find("if (bits & MAIN_EVENT_SEND_AUDIO)")
        self.assertGreater(start, 0)
        body = src[start : src.find("if (bits & MAIN_EVENT_WAKE_WORD_DETECTED)", start)]
        speaking = body[body.find("kDeviceStateSpeaking") : body.find("#else")]
        self.assertIn("HoldSpeakingUplink()", speaking)
        self.assertIn("UpdateSpeakingBargeIn()", speaking)
        self.assertNotIn("SendUplinkFromQueue();", speaking)

    def test_vad_during_tts_uses_near_end_energy_not_vad_alone(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = src.find("if (bits & MAIN_EVENT_VAD_CHANGE)")
        self.assertGreater(start, 0)
        vad = src[start : src.find("if (bits & MAIN_EVENT_VAD_INTERRUPT_CONFIRM)", start)]
        self.assertIn("UpdateSpeakingBargeIn()", vad)
        self.assertNotIn("MaybeStartVadInterruptTimer(true)", vad)

        update = src[
            src.find("void Application::UpdateSpeakingBargeIn") : src.find(
                "void Application::MaybeStartVadInterruptTimer"
            )
        ]
        self.assertIn("IsConfirmedNearEndSpeech()", update)
        self.assertNotIn("ShouldEarlyMuteForBargeIn()", update)
        self.assertNotIn("VAD barge-in early mute", update)
        self.assertIn("barge_in_ratio_hits_", update)
        self.assertIn("SetPlaybackDuckQ8(102)", update)
        self.assertIn("pct >= 16", update)
        self.assertIn("vad_interrupt_armed_", update)
        self.assertIn("120 * 1000", update)
        self.assertIn("timer_active", update)
        self.assertIn("PlaybackLevel() >= 200", update)
        self.assertIn("vad_interrupt_armed_ = false", update)

    def test_speaking_start_records_guard_clock_and_echo_profile(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        speaking = src[
            src.find("case kDeviceStateSpeaking:") : src.find("case kDeviceStateWifiConfiguring:")
        ]
        self.assertIn("speaking_started_us_ = esp_timer_get_time()", speaking)
        self.assertIn("ResetEchoProfile()", speaking)

    def test_aec_on_does_not_skip_barge_in_confirm(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        confirm = src[
            src.find("void Application::HandleVadInterruptConfirm") : src.find(
                "void Application::HoldSpeakingUplink"
            )
        ]
        self.assertIn("AbortSpeaking", confirm)
        self.assertIn("HoldSpeakingUplink()", confirm)
        self.assertIn("IsConfirmedNearEndSpeech()", confirm)
        self.assertIn("residual * 4 < barge_in_candidate_residual_ * 3", confirm)
        self.assertNotIn("IsPlaybackMuted() && barge_in_candidate_residual_", confirm)
        self.assertIn("SetPlaybackMuted(false)", confirm)
        self.assertNotIn("IsVadBargeInEnabled()", confirm)
        start_timer = src[
            src.find("void Application::MaybeStartVadInterruptTimer") : src.find(
                "void Application::HandleVadInterruptConfirm"
            )
        ]
        self.assertNotIn("IsVadBargeInEnabled()", start_timer)
        self.assertIn("IsConfirmedNearEndSpeech()", start_timer)
        self.assertIn("EchoProfileReady()", start_timer)
        self.assertIn("kVadInterruptGuardUs = 350 * 1000", start_timer)
        self.assertIn("kPostTtsSentenceGuardUs = 280 * 1000", start_timer)
        self.assertIn("unarmed_path ? 120 : 80", start_timer)
        self.assertIn("SetPlaybackMuted(true)", start_timer)
        self.assertIn("kRejectCooldownUs = 400 * 1000", start_timer)
        sentence_start = src.find('else if (strcmp(state->valuestring, "sentence_start") == 0)')
        stt = src.find('else if (strcmp(type->valuestring, "stt") == 0)', sentence_start)
        self.assertGreater(sentence_start, 0)
        self.assertGreater(stt, sentence_start)
        sentence = src[sentence_start:stt]
        self.assertIn("vad_interrupt_armed_ = false", sentence)
        self.assertIn("CancelVadInterruptTimer()", sentence)

    def test_hold_uplink_keeps_onset_without_near_end_energy(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        hold = src[
            src.find("void Application::HoldSpeakingUplink") : src.find(
                "void Application::FlushBargeInHold"
            )
        ]
        self.assertNotIn("IsLikelyNearEndSpeech()", hold)
        self.assertIn("kBargeInHoldMaxPackets = 32", hold)

    def test_barge_in_preroll_flushes_after_listen_start(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = src[
            src.find("void Application::StartListeningAudio") : src.find(
                "void Application::ConfigureWakeWordForListening"
            )
        ]
        listen = start.find("SendStartListening")
        flush_call = start.find("FlushBargeInHold(true)")
        self.assertGreater(listen, 0)
        self.assertGreater(flush_call, listen)
        flush = src[
            src.find("void Application::FlushBargeInHold") : src.find(
                "void Application::SendUplinkFromQueue"
            )
        ]
        self.assertIn("kOnsetPadPackets = 8", flush)
        self.assertIn("kMaxSendPackets = 20", flush)
        self.assertIn("back_floor", flush)
        self.assertIn("onset_need", flush)
        self.assertIn("kFallbackPackets = 16", flush)
        self.assertIn("barge_in_flush_residual_", flush)
        self.assertIn("HoldSpeakingUplink()", flush)
        self.assertNotIn("kMinSpeechAbs", flush)
        self.assertNotIn("kOnsetPadPackets = 6", flush)
        self.assertNotIn("kMaxSendPackets = 16", flush)
        confirm = src[
            src.find("void Application::HandleVadInterruptConfirm") : src.find(
                "void Application::HoldSpeakingUplink"
            )
        ]
        self.assertIn("barge_in_flush_residual_ = residual", confirm)

    def test_audio_service_learns_echo_floor_before_barge_in(self):
        header = (ROOT / "main/audio/audio_service.h").read_text(encoding="utf-8")
        src = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        self.assertIn("IsLikelyNearEndSpeech()", header)
        self.assertIn("IsConfirmedNearEndSpeech()", header)
        self.assertIn("ShouldEarlyMuteForBargeIn()", header)
        self.assertIn("ResetEchoProfile()", header)
        self.assertIn("echo_learn_frames_", header)
        self.assertNotIn("echo_coupling_", header)
        self.assertIn("NotePlaybackPcm", src)
        self.assertIn("NoteResidualPcm", src)
        self.assertIn("NoteCapturePcm", src)
        self.assertNotIn("AdaptEchoCoupling", src)
        self.assertIn("kEchoLearnFrames", src)
        self.assertIn("near_end_latched_", src)
        self.assertIn("floor / 3", src)
        self.assertIn("kMinResidualPctOfPlayback = 20", src)
        self.assertIn("kPlaybackStaleUs = 300 * 1000", src)
        self.assertIn("packet->residual = PcmMeanAbs", src)
        self.assertIn("vTaskDelay(pdMS_TO_TICKS(10))", src)
        self.assertIn("SetPlaybackMuted", header)
        self.assertIn("SetPlaybackDuckQ8", header)
        self.assertIn("playback_muted_", src)
        self.assertIn("playback_duck_q8_", src)
        self.assertIn("if (playback_muted_.load(std::memory_order_relaxed))", src)
        afe = (ROOT / "main/audio/engines/afe_audio_engine.cc").read_text(encoding="utf-8")
        self.assertIn("aec_nlp_level = AEC_NLP_LEVEL_NORMAL", afe)
        self.assertNotIn("aec_nlp_level = AEC_NLP_LEVEL_VERYAGGR", afe)
        self.assertNotIn("vad_delay_ms", afe)
        self.assertNotIn("AFE_TYPE_SR", afe)

    def test_barge_in_is_shared_not_forked_per_board(self):
        markers = (
            "ShouldEarlyMuteForBargeIn",
            "HoldSpeakingUplink",
            "SetPlaybackMuted",
            "kOnsetPadPackets",
        )
        for leaf in ("moss-onvif", "moss-ov2640"):
            board_dir = ROOT / "main/boards/moss" / leaf
            for path in board_dir.rglob("*.cc"):
                text = path.read_text(encoding="utf-8")
                for marker in markers:
                    self.assertNotIn(marker, text, f"{path.name} {marker}")
            for path in board_dir.rglob("*.h"):
                text = path.read_text(encoding="utf-8")
                for marker in markers:
                    self.assertNotIn(marker, text, f"{path.name} {marker}")


class MossWakeTtsTests(unittest.TestCase):
    def test_tts_start_keeps_packets_queued_before_scheduled_callback(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = src.find('if (strcmp(state->valuestring, "start") == 0)')
        self.assertGreater(start, 0)
        body = src[start : src.find('else if (strcmp(state->valuestring, "stop") == 0)', start)]
        self.assertIn("IsPlaybackIdle()", body)
        self.assertIn("keep queued TTS packets", body)
        self.assertIn("ResetDecoder()", body)
        self.assertLess(body.find("IsPlaybackIdle()"), body.find("keep queued TTS packets"))

    def test_incoming_tts_accepted_before_speaking_state(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        start = src.find("protocol_->OnIncomingAudio")
        self.assertGreater(start, 0)
        body = src[start : src.find("protocol_->OnAudioChannelOpened", start)]
        self.assertIn("kDeviceStateConnecting", body)
        self.assertIn("kDeviceStateListening", body)
        self.assertIn("kDeviceStateSpeaking", body)
        dropped = body[body.find("IsPendingAudioDropped()") :]
        recover = dropped[dropped.find("if (state ==") : dropped.find("} else {")]
        self.assertIn("kDeviceStateListening", recover)
        self.assertIn("kDeviceStateConnecting", recover)

    def test_voice_processing_start_does_not_drop_queued_tts(self):
        src = (ROOT / "main/audio/audio_service.cc").read_text(encoding="utf-8")
        start = src.find("void AudioService::EnableVoiceProcessing")
        self.assertGreater(start, 0)
        body = src[start : src.find("void AudioService::EnableAudioTesting", start)]
        self.assertIn("if (IsPlaybackIdle())", body)
        self.assertIn("ResetDecoder()", body)

    def test_pending_text_sent_after_listen_start_not_on_channel_open(self):
        src = (ROOT / "main/application.cc").read_text(encoding="utf-8")
        opened = src.find("protocol_->OnAudioChannelOpened")
        self.assertGreater(opened, 0)
        opened_body = src[opened : src.find("protocol_->OnAudioChannelClosed", opened)]
        self.assertNotIn("protocol_->SendTextChat", opened_body)
        self.assertIn("pending text waiting for listen/start", opened_body)

        listen = src.find("void Application::StartListeningAudio")
        self.assertGreater(listen, 0)
        listen_body = src[
            listen : src.find("void Application::ConfigureWakeWordForListening", listen)
        ]
        self.assertLess(listen_body.find("SendStartListening"), listen_body.find("SendTextChat"))
        self.assertIn("StartListeningAudio: sending pending text", listen_body)
        self.assertLess(listen_body.find("SendUdpHolePunch"), listen_body.find("SendTextChat"))

        mqtt = (ROOT / "main/protocols/mqtt_protocol.cc").read_text(encoding="utf-8")
        self.assertIn("void MqttProtocol::SendUdpHolePunch()", mqtt)
        self.assertIn("UDP hole punch sent", mqtt)
        open_fn = mqtt.find("bool MqttProtocol::OpenAudioChannel()")
        self.assertGreater(open_fn, 0)
        open_body = mqtt[open_fn : mqtt.find("std::string MqttProtocol::GetHelloMessage", open_fn)]
        self.assertIn("SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE)", open_body)
        self.assertLess(open_body.find("SetPowerSaveLevel"), open_body.find("udp->Connect"))
        self.assertLess(open_body.find("udp->Connect"), open_body.find("SendUdpHolePunch()"))

        handle = src.find("void Application::HandleExternalTextMessage")
        self.assertGreater(handle, 0)
        handle_body = src[handle : src.find("bool Application::CanEnterSleepMode", handle)]
        self.assertIn("SetListeningMode(kListeningModeAutoStop)", handle_body)
        self.assertGreater(
            handle_body.find("SetListeningMode(kListeningModeAutoStop)", handle_body.find("OpenAudioChannel")),
            handle_body.find("OpenAudioChannel"),
        )

        listening = src.find("case kDeviceStateListening:")
        self.assertGreater(listening, 0)
        listening_body = src[listening : src.find("case kDeviceStateSpeaking:", listening)]
        self.assertIn("pending_text_to_send_.empty()", listening_body)


class MossLcdDmaTests(unittest.TestCase):
    def test_both_boards_preallocate_lcd_spi_bounce(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            header = (ROOT / "main/boards" / f"moss/{leaf}" / "config.h").read_text(encoding="utf-8")
            self.assertIn("#define DISPLAY_LCD_BOUNCE_ROWS 16", header)
            self.assertIn("DISPLAY_SPI_MAX_TRANSFER", header)
            board = (
                ROOT / "main/boards" / f"moss/{leaf}" / f"{leaf.replace('-', '_')}_board.cc"
            ).read_text(encoding="utf-8")
            self.assertIn("DISPLAY_SPI_MAX_TRANSFER", board)
            self.assertIn("trans_queue_depth = 1", board)
            splash = (ROOT / "main/boards" / f"moss/{leaf}" / "splash_player.cc").read_text(
                encoding="utf-8"
            )
            self.assertIn("s_lcd_bounce", splash)
            self.assertIn("DrawPanelBitmapChunked", splash)
            self.assertNotIn(
                "esp_lcd_panel_draw_bitmap(cfg.panel, 0, 0, pw, ph, disp_buf)", splash
            )
            display = (ROOT / "main/boards" / f"moss/{leaf}" / "moss_spi_lcd_display.cc").read_text(
                encoding="utf-8"
            )
            self.assertIn("panel_set_disp_on_off", display)
            self.assertIn("panel_set_disp_on_off", splash)

    def test_eaf_compat_prefers_psram_over_internal_dma(self):
        for leaf in ("moss-onvif", "moss-ov2640"):
            src = (ROOT / "main/boards" / f"moss/{leaf}" / "eaf_compat.cc").read_text(
                encoding="utf-8"
            )
            start = src.find("alloc_aligned16")
            self.assertGreater(start, 0)
            body = src[start : src.find("posix_memalign", start)]
            self.assertLess(body.find("MALLOC_CAP_SPIRAM"), body.find("MALLOC_CAP_INTERNAL"))

