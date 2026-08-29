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
        self.assertNotIn("74hc595_driver.cc", family)
        self.assertNotIn("pca9685_driver.cc", family)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/mcp/*.cc", self.cmake)
        self.assertIn("${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD_DIR}/drivers/*.cc", self.cmake)
        self.assertIn('if(IS_DIRECTORY "${_board_inc_dir}")', self.cmake)


class MossBoardMcpLayoutTests(unittest.TestCase):
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

