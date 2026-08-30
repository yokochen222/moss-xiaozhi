import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
STILL_H = ROOT / "main/device/moss_jpeg_still.h"
BOARD = ROOT / "main/boards/moss/moss-ov2640/moss_ov2640_board.cc"
CAM = ROOT / "main/boards/common/esp32_camera.cc"
CAM_HAL = ROOT / "managed_components/espressif__esp32-camera/driver/cam_hal.c"
CFG = ROOT / "main/boards/moss/moss-ov2640/config.json"


def constexpr_int(text: str, name: str) -> int:
    match = re.search(rf"constexpr int {name} = (-?\d+);", text)
    if not match:
        raise AssertionError(f"missing constexpr int {name}")
    return int(match.group(1))


# Mirror moss_jpeg_still.h. Keep in lockstep with the C++ header.
K_EXPLAIN_JPEG_MAX_BYTES = 524288
K_AE_SETTLE_MIN_MS = 1200
K_AE_SETTLE_MAX_MS = 2400
K_AE_SETTLE_MIN_FRAMES = 12
K_STABLE_STREAK = 4
K_LEN_STABLE_PCT = 8
K_MIN_COMPLETE_JPEG_BYTES = 128
K_SW_JPEG_QUALITY_START = 92
K_SW_JPEG_QUALITY_MIN = 52
K_SW_JPEG_QUALITY_STEP = 8
K_SW_JPEG_MAX_DOWNSCALES = 2
K_SW_JPEG_MIN_EDGE = 160


def looks_complete(buf: bytes | None, length: int | None = None) -> bool:
    if buf is None:
        return False
    length = len(buf) if length is None else length
    if length < K_MIN_COMPLETE_JPEG_BYTES:
        return False
    if buf[0] != 0xFF or buf[1] != 0xD8:
        return False
    if buf[length - 2] != 0xFF or buf[length - 1] != 0xD9:
        return False
    return True


def len_stable(prev: int, cur: int) -> bool:
    if prev == 0 or cur == 0:
        return False
    hi = prev if prev > cur else cur
    lo = prev if prev < cur else cur
    lim = hi * K_LEN_STABLE_PCT // 100
    if lim < 512:
        lim = 512
    return (hi - lo) <= lim


def within_budget(length: int) -> bool:
    return length > 0 and length <= K_EXPLAIN_JPEG_MAX_BYTES


def rgb_looks_complete(length: int, width: int, height: int) -> bool:
    if width <= 0 or height <= 0:
        return False
    return length >= width * height * 2


def next_lower_sw_quality(quality: int) -> int:
    if quality <= K_SW_JPEG_QUALITY_MIN:
        return K_SW_JPEG_QUALITY_MIN
    nxt = quality - K_SW_JPEG_QUALITY_STEP
    return K_SW_JPEG_QUALITY_MIN if nxt < K_SW_JPEG_QUALITY_MIN else nxt


class SettleState:
    def __init__(self):
        self.last_len = 0
        self.streak = 0
        self.frames = 0


def on_frame(st: SettleState, complete: bool, length: int, elapsed_ms: int) -> str:
    st.frames += 1
    if complete:
        if len_stable(st.last_len, length):
            st.streak += 1
        else:
            st.streak = 1
        st.last_len = length
    else:
        st.streak = 0
    if (
        elapsed_ms >= K_AE_SETTLE_MIN_MS
        and st.frames >= K_AE_SETTLE_MIN_FRAMES
        and st.streak >= K_STABLE_STREAK
    ):
        return "ready"
    if elapsed_ms >= K_AE_SETTLE_MAX_MS:
        return "ready" if st.last_len > 0 and st.streak >= 2 else "failed"
    return "progress"


def make_jpeg(payload_len: int, *, eoi: bool = True, soi: bool = True) -> bytes:
    body = b"\x00" * max(payload_len - 4, 0)
    head = b"\xff\xd8" if soi else b"\x00\x00"
    tail = b"\xff\xd9" if eoi else b"\x00\x00"
    data = head + body + tail
    if len(data) < payload_len:
        data += b"\x00" * (payload_len - len(data))
    return data[:payload_len]


class JpegStillPolicySyncTests(unittest.TestCase):
    def setUp(self):
        self.header = STILL_H.read_text(encoding="utf-8")
        self.board = BOARD.read_text(encoding="utf-8")
        self.cam = CAM.read_text(encoding="utf-8")
        self.cam_hal = CAM_HAL.read_text(encoding="utf-8")
        self.cfg = CFG.read_text(encoding="utf-8")

    def test_header_constants_match_python_and_config(self):
        self.assertEqual(constexpr_int(self.header, "kExplainJpegMaxBytes"), K_EXPLAIN_JPEG_MAX_BYTES)
        self.assertEqual(constexpr_int(self.header, "kStillXclkHz"), 10000000)
        self.assertEqual(constexpr_int(self.header, "kGimbalWaitMaxMs"), 1500)
        self.assertEqual(constexpr_int(self.header, "kGimbalSettleMs"), 280)
        self.assertEqual(constexpr_int(self.header, "kAeSettleMinMs"), K_AE_SETTLE_MIN_MS)
        self.assertEqual(constexpr_int(self.header, "kAeSettleMaxMs"), K_AE_SETTLE_MAX_MS)
        self.assertEqual(constexpr_int(self.header, "kAeSettleMinFrames"), K_AE_SETTLE_MIN_FRAMES)
        self.assertEqual(constexpr_int(self.header, "kStableStreak"), K_STABLE_STREAK)
        self.assertEqual(constexpr_int(self.header, "kSwJpegQualityStart"), K_SW_JPEG_QUALITY_START)
        self.assertEqual(constexpr_int(self.header, "kSwJpegQualityMin"), K_SW_JPEG_QUALITY_MIN)
        self.assertEqual(constexpr_int(self.header, "kSwJpegQualityStep"), K_SW_JPEG_QUALITY_STEP)
        self.assertEqual(constexpr_int(self.header, "kSwJpegMaxDownscales"), K_SW_JPEG_MAX_DOWNSCALES)
        self.assertEqual(constexpr_int(self.header, "kStillInitDelayMs"), 500)
        self.assertEqual(constexpr_int(self.header, "kStillBrightness"), 1)
        self.assertEqual(constexpr_int(self.header, "kStillAeLevel"), 1)
        self.assertEqual(constexpr_int(self.header, "kStillGainCeiling"), 4)

    def test_board_uses_rgb565_sxga_and_waits_for_settle(self):
        capture = self.board[
            self.board.find("bool Capture()") : self.board.find("bool SetHMirror")
        ]
        self.assertIn("WaitGimbalSettled()", capture)
        self.assertLess(capture.find("WaitGimbalSettled()"), capture.find("EnsureStartedLocked()"))
        self.assertIn("CaptureJpegUnderBudgetLocked()", capture)
        wait = self.board[
            self.board.find("static void WaitGimbalSettled()") : self.board.find(
                "static void PauseLcdForDvp"
            )
        ]
        self.assertIn("gimbal.Stop()", wait)
        self.assertIn("IsMoving()", wait)
        ensure = self.board[
            self.board.find("bool EnsureStartedLocked(bool preview") : self.board.find(
                "void ReleaseLocked()"
            )
        ]
        self.assertIn("PIXFORMAT_RGB565", ensure)
        self.assertIn("fb_count = 2", ensure)
        self.assertIn("CAMERA_GRAB_WHEN_EMPTY", ensure)
        self.assertNotIn("FRAMESIZE_UXGA", ensure)
        self.assertIn("still_sizes_voice", ensure)
        self.assertIn("still_sizes_dma_tight", ensure)
        self.assertIn("ApplyOv2640IndoorTuning()", ensure)
        voice = ensure.find("still_sizes_voice[]")
        self.assertGreater(voice, 0)
        voice_end = ensure.find("still_sizes_dma_tight[]", voice)
        self.assertGreater(voice_end, voice)
        voice_arr = ensure[voice:voice_end]
        self.assertLess(voice_arr.find("FRAMESIZE_VGA"), voice_arr.find("FRAMESIZE_QVGA"))
        dma_arr = ensure[
            ensure.find("still_sizes_dma_tight[]") : ensure.find("still_sizes_full[]")
        ]
        self.assertIn("FRAMESIZE_QVGA", dma_arr)
        self.assertNotIn("FRAMESIZE_HVGA", dma_arr)
        self.assertIn("fb_count = 2", ensure)
        self.assertIn("VGA-first fb_count=2", ensure)
        self.assertIn("CONFIG_CAMERA_PSRAM_DMA", ensure)
        self.assertIn("const bool dma_tight = false", ensure)
        sxga = ensure.find("FRAMESIZE_SXGA")
        svga = ensure.find("FRAMESIZE_SVGA")
        self.assertGreaterEqual(sxga, 0)
        self.assertGreater(svga, sxga)
        full = ensure[ensure.find("still_sizes_full[]") :]
        self.assertLess(full.find("FRAMESIZE_SXGA"), full.find("FRAMESIZE_VGA"))
        self.assertIn("RGB565 XCLK", ensure)
        settle = self.board[
            self.board.find("void DrainStillSettleLocked()") : self.board.find(
                "void DiscardPostSettleFramesLocked"
            )
        ]
        self.assertIn("OnFrame(", settle)
        self.assertNotIn("OnFrameVoice", settle)
        budget = self.board[
            self.board.find("bool CaptureJpegUnderBudgetLocked()") : self.board.find(
                "void DrainStillSettleLocked()"
            )
        ]
        self.assertIn("DiscardPostSettleFramesLocked", budget)
        self.assertIn("EncodeAndParkJpeg", budget)
        self.assertNotIn("SetSensorJpegQuality", budget)
        self.assertIn("RgbLooksComplete", self.board)
        self.assertIn("bool Esp32Camera::EncodeAndParkJpeg", self.cam)
        self.assertIn("memcpy(rgb, src, src_len)", self.cam)
        self.assertIn("SwapRgb565BeToLe", self.cam)
        self.assertIn("DownsampleRgb565X2", self.cam)
        self.assertIn("JpegFarOverBudget", self.cam)
        self.assertIn("image_to_jpeg", self.cam)
        self.assertIn("Drop short RGB565", self.cam)
        encode = self.cam[
            self.cam.find("bool Esp32Camera::EncodeAndParkJpeg") : self.cam.find(
                "bool Esp32Camera::SetHMirror"
            )
        ]
        self.assertIn("StopDvp()", encode)
        self.assertLess(encode.find("StopDvp()"), encode.find("image_to_jpeg"))
        self.assertIn("MossDvpQuiesceBeforeDeinit()", encode)
        self.assertLess(encode.find("MossDvpQuiesceBeforeDeinit()"), encode.find("heap_caps_malloc"))
        self.assertNotIn("ESP_CACHE_MSYNC_FLAG_UNALIGNED", encode)
        stop = self.cam[
            self.cam.find("void Esp32Camera::StopDvp()") : self.cam.find("Esp32Camera::~Esp32Camera")
        ]
        self.assertIn("MossDvpQuiesceBeforeDeinit()", stop)
        self.assertIn("MossDvpQuiesceBeforeDeinit()", self.board)
        quiesce = self.board[
            self.board.find("void MossDvpQuiesceBeforeDeinit") : self.board.find("class OnDemandEsp32Camera")
        ]
        self.assertIn("SetDvpPowerDown(true)", quiesce)
        self.assertIn("LCD_CAM.cam_ctrl1.cam_start = 0", quiesce)
        deinit_safe = self.board[
            self.board.find("static void DeinitDvpSafe()") : self.board.find("static void PulseDvpReset()")
        ]
        self.assertIn("MossDvpQuiesceBeforeDeinit()", deinit_safe)
        drop = self.cam_hal[
            self.cam_hal.find("static inline void cam_drop_psram_cache") : self.cam_hal.find(
                "CAM_WARN_THROTTLE"
            )
        ]
        self.assertIn("(p + line - 1) & ~(line - 1)", drop)
        self.assertNotIn("addr & ~(line - 1)", drop)
        self.assertIn("LCD_CAM.cam_ctrl1.cam_start = 0", self.cam_hal)
        rgb_copy = encode[encode.find("memcpy(rgb, src, src_len") :]
        self.assertNotIn("esp_camera_fb_return(current_fb_)", rgb_copy)

    def test_esp32_camera_drops_truncated_jpeg(self):
        self.assertIn("Drop truncated JPEG", self.cam)
        self.assertIn("len * 100 >=", self.cam)
        self.assertIn("* 90", self.cam)


class JpegLooksCompleteTests(unittest.TestCase):
    def test_rejects_none_and_tiny(self):
        self.assertFalse(looks_complete(None))
        self.assertFalse(looks_complete(b"\xff\xd8\xff\xd9"))
        self.assertFalse(looks_complete(make_jpeg(K_MIN_COMPLETE_JPEG_BYTES - 1)))

    def test_accepts_minimal_complete_jpeg(self):
        self.assertTrue(looks_complete(make_jpeg(K_MIN_COMPLETE_JPEG_BYTES)))
        self.assertTrue(looks_complete(make_jpeg(4096)))

    def test_rejects_missing_soi_or_eoi(self):
        self.assertFalse(looks_complete(make_jpeg(4096, soi=False)))
        self.assertFalse(looks_complete(make_jpeg(4096, eoi=False)))

    def test_rejects_missing_soi_or_eoi(self):
        self.assertFalse(looks_complete(make_jpeg(4096, soi=False)))
        self.assertFalse(looks_complete(make_jpeg(4096, eoi=False)))


def jpeg_far_over_budget(length: int, max_bytes: int) -> bool:
    return max_bytes > 0 and length > max_bytes + max_bytes // 2


def can_downscale_rgb(width: int, height: int) -> bool:
    return (width // 2) >= K_SW_JPEG_MIN_EDGE and (height // 2) >= K_SW_JPEG_MIN_EDGE


class JpegBudgetAndQualityTests(unittest.TestCase):
    def test_budget_edges(self):
        self.assertFalse(within_budget(0))
        self.assertTrue(within_budget(1))
        self.assertTrue(within_budget(K_EXPLAIN_JPEG_MAX_BYTES))
        self.assertFalse(within_budget(K_EXPLAIN_JPEG_MAX_BYTES + 1))
        self.assertFalse(within_budget(600000))

    def test_sw_quality_steps_down_to_min(self):
        self.assertEqual(next_lower_sw_quality(92), 84)
        self.assertEqual(next_lower_sw_quality(88), 80)
        self.assertEqual(next_lower_sw_quality(60), 52)
        self.assertEqual(next_lower_sw_quality(52), 52)
        self.assertEqual(next_lower_sw_quality(10), 52)

    def test_far_over_budget_skips_to_downscale(self):
        self.assertFalse(jpeg_far_over_budget(51200, 51200))
        self.assertFalse(jpeg_far_over_budget(60000, 51200))
        self.assertTrue(jpeg_far_over_budget(127039, 51200))
        self.assertTrue(jpeg_far_over_budget(800000, K_EXPLAIN_JPEG_MAX_BYTES))

    def test_svga_can_downscale_once_not_twice(self):
        self.assertTrue(can_downscale_rgb(800, 600))
        self.assertFalse(can_downscale_rgb(400, 300))
        self.assertFalse(can_downscale_rgb(320, 240))

    def test_rgb_frame_must_be_full_pixels(self):
        self.assertFalse(rgb_looks_complete(0, 800, 600))
        self.assertFalse(rgb_looks_complete(800 * 600 * 2 - 1, 800, 600))
        self.assertTrue(rgb_looks_complete(800 * 600 * 2, 800, 600))
        self.assertTrue(rgb_looks_complete(800 * 600 * 2 + 16, 800, 600))
        self.assertFalse(rgb_looks_complete(100, 0, 600))


class JpegLenStableTests(unittest.TestCase):
    def test_zero_never_stable(self):
        self.assertFalse(len_stable(0, 20000))
        self.assertFalse(len_stable(20000, 0))
        self.assertFalse(len_stable(0, 0))

    def test_small_jitter_is_stable(self):
        self.assertTrue(len_stable(20000, 20400))
        self.assertTrue(len_stable(20000, 19600))

    def test_large_swing_is_unstable(self):
        self.assertFalse(len_stable(20000, 40000))
        self.assertFalse(len_stable(12000, 20000))

    def test_floor_512_for_tiny_frames(self):
        self.assertTrue(len_stable(1000, 1400))
        self.assertFalse(len_stable(1000, 1600))


class JpegSettleStateMachineTests(unittest.TestCase):
    def test_incomplete_resets_streak(self):
        st = SettleState()
        self.assertEqual(on_frame(st, True, 20000, 100), "progress")
        self.assertEqual(st.streak, 1)
        self.assertEqual(on_frame(st, False, 20000, 200), "progress")
        self.assertEqual(st.streak, 0)

    def test_oscillating_size_never_readies_before_timeout(self):
        st = SettleState()
        sizes = [12000, 40000, 12000, 40000]
        for i in range(20):
            ev = on_frame(st, True, sizes[i % 4], 100 + i * 40)
            self.assertEqual(ev, "progress")
        self.assertLess(st.streak, K_STABLE_STREAK)

    def test_stable_streak_after_min_time_and_frames(self):
        st = SettleState()
        for i in range(K_AE_SETTLE_MIN_FRAMES - 1):
            ev = on_frame(st, True, 22000, K_AE_SETTLE_MIN_MS)
            self.assertEqual(ev, "progress", msg=f"frame {i}")
        ev = on_frame(st, True, 22100, K_AE_SETTLE_MIN_MS)
        self.assertEqual(ev, "ready")
        self.assertGreaterEqual(st.streak, K_STABLE_STREAK)

    def test_ready_requires_min_ms_even_if_streak_already_met(self):
        st = SettleState()
        for _ in range(K_AE_SETTLE_MIN_FRAMES):
            ev = on_frame(st, True, 22000, 200)
            self.assertEqual(ev, "progress")
        self.assertGreaterEqual(st.streak, K_STABLE_STREAK)
        ev = on_frame(st, True, 22050, K_AE_SETTLE_MIN_MS)
        self.assertEqual(ev, "ready")

    def test_timeout_with_two_stable_complete_frames_is_ready(self):
        st = SettleState()
        self.assertEqual(on_frame(st, True, 18000, 100), "progress")
        ev = on_frame(st, True, 18100, K_AE_SETTLE_MAX_MS)
        self.assertEqual(ev, "ready")
        self.assertEqual(st.streak, 2)

    def test_timeout_without_stable_pair_fails(self):
        st = SettleState()
        self.assertEqual(on_frame(st, False, 0, 100), "progress")
        ev = on_frame(st, False, 0, K_AE_SETTLE_MAX_MS)
        self.assertEqual(ev, "failed")

    def test_timeout_single_complete_frame_fails(self):
        st = SettleState()
        self.assertEqual(on_frame(st, True, 18000, 100), "progress")
        ev = on_frame(st, False, 0, K_AE_SETTLE_MAX_MS)
        self.assertEqual(ev, "failed")
        self.assertEqual(st.streak, 0)


if __name__ == "__main__":
    unittest.main()
