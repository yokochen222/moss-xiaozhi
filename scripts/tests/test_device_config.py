import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
DEVICE_CONFIG = ROOT / "main/config/device_config.cc"
SETTINGS = ROOT / "main/settings.cc"
WAKE = ROOT / "main/audio/wake_words/custom_wake_word.cc"

# ESP-IDF NVS_KEY_NAME_MAX_SIZE is 16 including NUL.
NVS_KEY_MAX = 15


def nvs_key_literals(text: str) -> list[str]:
    keys = []
    for match in re.finditer(
        r'(?:Get|Set|Erase)(?:Int|String|Bool)\(\s*"([^"]+)"', text
    ):
        keys.append(match.group(1))
    for match in re.finditer(r'kNvs\w+\s*\[\s*\]\s*=\s*"([^"]+)"', text):
        keys.append(match.group(1))
    for match in re.finditer(r'constexpr const char\*\s+kNvs\w+\s*=\s*"([^"]+)"', text):
        keys.append(match.group(1))
    return keys


class DeviceConfigNvsTests(unittest.TestCase):
    def setUp(self):
        self.cfg = DEVICE_CONFIG.read_text(encoding="utf-8")
        self.settings = SETTINGS.read_text(encoding="utf-8")
        self.wake = WAKE.read_text(encoding="utf-8")

    def test_motor_speed_json_is_not_used_as_nvs_key(self):
        self.assertIn('"default_motor_speed"', self.cfg)
        self.assertIn('kNvsDefaultMotorSpeed[] = "def_motor_spd"', self.cfg)
        self.assertNotIn('GetInt("default_motor_speed"', self.cfg)
        self.assertNotIn('SetInt("default_motor_speed"', self.cfg)
        self.assertIn("sizeof(kNvsDefaultMotorSpeed) - 1 <= 15", self.cfg)

    def test_vendor_nvs_keys_fit_esp_idf_limit(self):
        for key in nvs_key_literals(self.cfg) + nvs_key_literals(self.wake):
            self.assertLessEqual(
                len(key),
                NVS_KEY_MAX,
                msg=f"NVS key {key!r} is {len(key)} chars (max {NVS_KEY_MAX})",
            )

    def test_settings_does_not_abort_on_nvs_write_error(self):
        self.assertNotIn("ESP_ERROR_CHECK", self.settings)
        self.assertIn("EnsureWritable", self.settings)
        self.assertIn("NVS_KEY_NAME_MAX_SIZE", self.settings)
        set_int = self.settings[
            self.settings.find("bool Settings::SetInt") : self.settings.find(
                "bool Settings::GetBool"
            )
        ]
        self.assertIn("return false", set_int)
        self.assertIn("nvs_set_i32", set_int)

    def test_apply_reports_save_failure_instead_of_crashing(self):
        apply = self.cfg[self.cfg.find("bool Apply(") :]
        self.assertIn("SetDefaultMotorSpeedPercent", apply)
        self.assertIn("failed to save default_motor_speed", apply)
        self.assertIn("if (!SetDefaultMotorSpeedPercent(speed))", apply)
