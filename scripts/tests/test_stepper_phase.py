import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
PHASE_H = ROOT / "main/device/stepper_phase.h"
GIMBAL_CC = ROOT / "main/device/stepper_gimbal.cc"
GIMBAL_H = ROOT / "main/device/stepper_gimbal.h"
OV2640_595 = ROOT / "main/boards/moss/moss-ov2640/drivers/74hc595_driver.cc"
ONVIF_595 = ROOT / "main/boards/moss/moss-onvif/drivers/74hc595_driver.cc"
MQTT_CC = ROOT / "main/external_mqtt_client.cc"
BOARD_CC = ROOT / "main/boards/moss/moss-ov2640/moss_ov2640_board.cc"


def coil_mask_h(a, b, c, d):
    # M2: A=Q5, B=Q6, C=Q7, D=Q0
    return (0x01 if d else 0) | (0x20 if a else 0) | (0x40 if b else 0) | (0x80 if c else 0)


def coil_mask_v(a, b, c, d):
    # M1: A=Q1, B=Q2, C=Q3, D=Q4
    return (0x02 if a else 0) | (0x04 if b else 0) | (0x08 if c else 0) | (0x10 if d else 0)


def merge_lamp_style(data, panel_state=0):
    return (data & 0x1F) | (panel_state & 0xE0)


class StepperPhaseTests(unittest.TestCase):
    def test_header_matches_python_masks(self):
        text = PHASE_H.read_text(encoding="utf-8")
        self.assertIn("Q0=M2_L4", text)
        self.assertIn("Q5=M2_L1", text)
        self.assertEqual(coil_mask_h(1, 0, 0, 0), 0x20)
        self.assertEqual(coil_mask_h(0, 0, 0, 1), 0x01)
        self.assertEqual(coil_mask_v(1, 0, 0, 0), 0x02)
        self.assertEqual(coil_mask_v(0, 0, 0, 1), 0x10)

    def test_horizontal_phase_uses_high_bits(self):
        half_h = [
            coil_mask_h(1, 0, 0, 0),
            coil_mask_h(1, 1, 0, 0),
            coil_mask_h(0, 1, 0, 0),
            coil_mask_h(0, 1, 1, 0),
            coil_mask_h(0, 0, 1, 0),
            coil_mask_h(0, 0, 1, 1),
            coil_mask_h(0, 0, 0, 1),
            coil_mask_h(1, 0, 0, 1),
        ]
        for mask in half_h:
            self.assertTrue(mask & 0xE1, hex(mask))

    def test_lamp_merge_strips_horizontal_coils(self):
        for a, b, c, d in (
            (1, 0, 0, 0),
            (1, 1, 0, 0),
            (0, 1, 0, 0),
            (0, 0, 1, 0),
            (0, 0, 0, 1),
        ):
            raw = coil_mask_h(a, b, c, d)
            merged = merge_lamp_style(raw, 0)
            self.assertEqual(merged & 0xE0, 0, hex(raw))
            self.assertEqual(merged & 0x01, raw & 0x01)

    def test_lamp_merge_keeps_vertical_coils(self):
        raw = coil_mask_v(1, 1, 1, 1)
        self.assertEqual(merge_lamp_style(raw, 0), raw)
        self.assertEqual(raw & 0xE0, 0)

    def test_raw_mode_would_preserve_horizontal(self):
        raw = coil_mask_h(1, 1, 0, 0)
        self.assertEqual(raw, 0x20 | 0x40)
        self.assertNotEqual(merge_lamp_style(raw, 0), raw)


class GimbalDriverWiringTests(unittest.TestCase):
    def test_gimbal_constructs_board_595(self):
        text = GIMBAL_CC.read_text(encoding="utf-8")
        self.assertIn("ShiftRegister74HC595>(SER_PIN, RCK_PIN, SCK_PIN)", text)
        self.assertNotIn("SCK_PIN, true)", text)
        self.assertIn("bool StepperGimbalDevice::Idle()", text)

    def test_ov2640_595_writes_full_byte(self):
        text = OV2640_595.read_text(encoding="utf-8")
        self.assertNotIn("panel_state_", text)
        self.assertNotIn("0x1F", text)
        self.assertIn("current_data_ = data", text)

    def test_onvif_595_keeps_lamp_merge(self):
        text = ONVIF_595.read_text(encoding="utf-8")
        self.assertIn("panel_state_", text)
        self.assertIn("0x1F", text)
        self.assertIn("0xE0", text)

    def test_mqtt_gimbal_runs_inline_and_stop_idles(self):
        text = MQTT_CC.read_text(encoding="utf-8")
        self.assertIn("HandleGimbalControl", text)
        self.assertIn('type == "gimbal.control"', text)
        gimbal_block = text[text.find("gimbal.control") : text.find("face_track.control")]
        self.assertIn("HandleGimbalControl(id, payload)", gimbal_block)
        self.assertNotIn("Application::GetInstance().Schedule", gimbal_block)
        self.assertIn("gimbal.Idle()", text)
        self.assertIn("follow task failed", text)
        self.assertIn('action == "move"', text)
        self.assertIn("invalid direction", text)
        self.assertIn("gimbal.Move", text)
        self.assertIn("StepMode::Half, 4", text)
        self.assertIn("4, StepMode::Half", text)

    def test_step_delay_is_not_sliced_to_1ms(self):
        header = GIMBAL_H.read_text(encoding="utf-8")
        text = GIMBAL_CC.read_text(encoding="utf-8")
        self.assertIn("DEFAULT_DELAY_MS = 4", header)
        self.assertIn("max_slice = pdMS_TO_TICKS(4)", text)
        self.assertNotIn("pdMS_TO_TICKS(1) ? pdMS_TO_TICKS(1)", text)

    def test_horizontal_motor_sign_matches_visual_left(self):
        text = GIMBAL_CC.read_text(encoding="utf-8")
        self.assertIn("kHSign = -1", text)
        self.assertIn("kHSign * clamp_axis(h_steps)", text)
        self.assertIn("h_idx_ += kHSign * h_dir", text)

    def test_board_does_not_start_follow_at_boot(self):
        text = BOARD_CC.read_text(encoding="utf-8")
        ctor = text[text.find("MossOv2640Board()") : text.find("AudioCodec* GetAudioCodec")]
        self.assertIn("GetInstance().Stop()", ctor)
        self.assertNotIn("SetFollowRates", ctor)
        gimbal = GIMBAL_CC.read_text(encoding="utf-8")
        self.assertIn("xTaskCreatePinnedToCoreWithCaps", gimbal)
        self.assertNotIn("xTaskCreatePinnedToCore(FollowTask, \"GimbalFollow\", 3072, this, 2", gimbal)


if __name__ == "__main__":
    unittest.main()
