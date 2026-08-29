import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HEADER = ROOT / "main/device/moss_camera_preview.h"
STREAM = ROOT / "main/device/moss_camera_stream.cc"
BOARD = ROOT / "main/boards/moss/moss-ov2640/moss_ov2640_board.cc"
API = ROOT / "main/api/api.cc"
HANDLERS = ROOT / "main/api/methods/camera/camera_handlers.cc"
HANDLERS_H = ROOT / "main/api/methods/camera/camera_handlers.h"


def slice_between(text: str, start: str, end: str) -> str:
    i = text.find(start)
    j = text.find(end, i + 1) if i >= 0 else -1
    if i < 0 or j < 0:
        raise AssertionError(f"could not slice {start!r} .. {end!r}")
    return text[i:j]


class MossCameraPreviewPolicyTests(unittest.TestCase):
    def setUp(self):
        self.header = HEADER.read_text(encoding="utf-8")
        self.stream = STREAM.read_text(encoding="utf-8")
        self.board = BOARD.read_text(encoding="utf-8")

    def test_http_preview_is_disabled(self):
        self.assertIn("501 Not Implemented", self.stream)
        self.assertIn("preview disabled", self.stream)
        self.assertNotIn("PreviewTask", self.stream)
        self.assertNotIn("cam_prev", self.stream)
        self.assertNotIn("AcquireLiveStream()", self.stream)
        self.assertNotIn("Arm(", self.stream)
        acquire = slice_between(self.board, "bool AcquireLiveStream()", "void ReleaseLiveStream()")
        self.assertIn("return false;", acquire)
        self.assertNotIn("EnsureStartedLocked", acquire)

    def test_lcd_boot_scroll_is_throttled(self):
        splash = (ROOT / "main/boards/moss/moss-ov2640/splash_player.cc").read_text(encoding="utf-8")
        header = (ROOT / "main/boards/moss/moss-ov2640/splash_player.h").read_text(encoding="utf-8")
        self.assertIn("RENDER_INTERVAL_MS = 120", splash)
        self.assertIn('xTaskCreateWithCaps(loop_task, "code_scroll", 8192, ctx, 1', splash)
        self.assertIn("vTaskDelay(1)", splash)
        self.assertIn("set_code_scroll_render_interval_ms", splash)
        self.assertIn("set_code_scroll_render_interval_ms", header)

    def test_bind_hello_keepalive_does_not_reenter_bind_mode(self):
        src = (ROOT / "main/config/moss_config_service.cc").read_text(encoding="utf-8")
        body = slice_between(src, "void MossConfigService::OnBindHello()", "void MossConfigService::OnBindClear()")
        self.assertIn("awaiting_hello_", body)
        self.assertIn("bind_mode_", body)
        self.assertIn("return;", body)

    def test_capture_still_pauses_lcd_for_photo_path(self):
        capture = slice_between(self.board, "bool Capture()", "bool SetHMirror")
        self.assertIn("PauseLcdForDvp(false)", capture)
        self.assertIn("PauseForExternalCameraUse()", capture)
        self.assertIn("WaitGimbalSettled()", capture)
        self.assertIn("EnsureStartedLocked()", capture)
        self.assertNotIn("EnsureStartedLocked(true)", capture)
        self.assertNotIn("StepperGimbalDevice::GetInstance().Stop()", capture)
        self.assertIn("CaptureJpegUnderBudgetLocked()", capture)
        budget = slice_between(self.board, "bool CaptureJpegUnderBudgetLocked()", "void DrainStillSettleLocked()")
        self.assertIn("ExportParkedJpeg", budget)
        self.assertIn("explain_jpeg_", budget)
        self.assertIn("ResumeLcdAfterDvp()", capture)
        fail_resume = slice_between(capture, "if (!ok)", "return ok;")
        self.assertIn("ResumeAfterExternalCameraUse()", fail_resume)
        explain = slice_between(self.board, "std::string Explain(", "bool AcquireLiveStream()")
        self.assertIn("ExplainJpegUpload", explain)
        self.assertIn("explain_jpeg_", explain)
        self.assertNotIn("MossDesktopPreparePlayback", explain)
        self.assertIn("ResumeAfterExternalCameraUse()", explain)
        tracker = (ROOT / "main/device/face_tracker.cc").read_text(encoding="utf-8")
        self.assertIn("TryResumeAfterVoiceIdle", tracker)
        self.assertIn("Defer face-track resume until voice idle", tracker)
        self.assertIn("ResumeAfterExternalCameraUse()", explain)
        ensure = slice_between(self.board, "bool EnsureStartedLocked(bool preview", "void ReleaseLocked()")
        self.assertIn("FRAMESIZE_VGA", ensure)
        self.assertNotIn("FRAMESIZE_UXGA", ensure)
        self.assertIn("kExplainJpegMaxBytes", self.board)
        self.assertIn("explain_jpeg_", self.board)
        self.assertIn("EncodeAndParkJpeg", self.board)
        still_h = (ROOT / "main/device/moss_jpeg_still.h").read_text(encoding="utf-8")
        self.assertIn("kExplainJpegMaxBytes = 524288", still_h)
        self.assertIn("kSwJpegQualityStart = 92", still_h)
        self.assertIn("PIXFORMAT_RGB565", ensure)
        self.assertIn("FRAMESIZE_SXGA", ensure)
        self.assertIn("FRAMESIZE_SVGA", ensure)
        cam = (ROOT / "main/boards/common/esp32_camera.cc").read_text(encoding="utf-8")
        self.assertIn("size_t Esp32Camera::CapturedJpegLen()", cam)
        self.assertIn("bool Esp32Camera::SetSensorJpegQuality", cam)
        acquire = slice_between(self.board, "bool AcquireTracking()", "void ReleaseTracking()")
        self.assertIn("StopLcdForTracking()", acquire)
        self.assertNotIn("YieldLcdForDvp()", acquire)
        self.assertIn("FRAMESIZE_HVGA", acquire)
        self.assertIn("HVGA x1", acquire)
        self.assertIn("QVGA x1", acquire)
        lcd = (ROOT / "main/boards/moss/moss-ov2640/moss_spi_lcd_display.cc").read_text(
            encoding="utf-8"
        )
        self.assertIn("stop_code_scroll_loop", lcd)
        self.assertIn("wait_code_scroll_stopped(1500)", lcd)
        enter = slice_between(lcd, "void MossSpiLcdDisplay::EnterFaceTrackMode()", "void MossSpiLcdDisplay::ExitFaceTrackMode()")
        self.assertIn("stop_code_scroll_loop", enter)
        self.assertIn("draw_face_track_hud", enter)
        exit_fn = slice_between(lcd, "void MossSpiLcdDisplay::ExitFaceTrackMode()", "void MossSpiLcdDisplay::UpdateFaceTrackOverlay")
        self.assertIn("StartSplashLoop()", exit_fn)
        self.assertNotIn("scroll stays paused for preview", lcd)
        tracker = (ROOT / "main/device/face_tracker.cc").read_text(encoding="utf-8")
        start = slice_between(tracker, "bool FaceTracker::Start()", "bool FaceTracker::Stop()")
        self.assertLess(start.find("AcquireTracking()"), start.find("start_ui()"))
        self.assertIn("xTaskCreatePinnedToCoreWithCaps", start)
        self.assertIn("MALLOC_CAP_SPIRAM", start)
        self.assertIn("vTaskDeleteWithCaps", tracker)
        fail_at = start.find("AcquireTracking failed")
        self.assertGreaterEqual(fail_at, 0)
        ret_at = start.find("return false", fail_at)
        self.assertGreater(ret_at, fail_at)
        self.assertIn("stop_ui()", start[fail_at:ret_at])
        header = (ROOT / "main/device/face_tracker.h").read_text(encoding="utf-8")
        self.assertIn("kMinStepDelayMs = 4", header)
        self.assertIn("kMaxStepDelayMs = 8", header)
        self.assertIn("kTrackHSign = -1", header)
        apply = slice_between(tracker, "void FaceTracker::ApplyControl(", "void FaceTracker::ApplyIdleFollow()")
        self.assertIn("kTrackHSign", apply)
        loop = slice_between(tracker, "void FaceTracker::TaskLoop()", "bool FaceTracker::Start()")
        self.assertLess(loop.find("ReturnTrackingFrame"), loop.find("det->run"))
        self.assertNotIn("ReleaseTracking()", loop)
        pause = slice_between(
            tracker, "bool FaceTracker::PauseForExternalCameraUse()", "static bool VoicePipelineBusy()"
        )
        self.assertIn("frame_in_flight_", pause)
        self.assertIn("pdMS_TO_TICKS(50)", pause)
        jpeg = (ROOT / "main/display/lvgl_display/jpg/image_to_jpeg.cpp").read_text(
            encoding="utf-8"
        )
        malloc_fn = slice_between(jpeg, "static void* malloc_psram(size_t size)", "static __always_inline")
        self.assertIn("MALLOC_CAP_SPIRAM", malloc_fn)
        self.assertLess(
            malloc_fn.find("MALLOC_CAP_SPIRAM"), malloc_fn.find("MALLOC_CAP_8BIT")
        )
        self.assertNotIn("malloc(size)", malloc_fn)
        stop = slice_between(tracker, "bool FaceTracker::Stop()", "bool FaceTracker::IsRunning()")
        self.assertIn("ReleaseTracking()", stop)
        self.assertIn("EnsureDetectBuffer", loop)
        self.assertIn("memcpy", loop)
        acquire = slice_between(self.board, "bool AcquireTracking()", "void ReleaseTracking()")
        self.assertIn("PulseDvpReset()", acquire)
        self.assertIn("DropOneCameraFrame()", acquire)
        release = slice_between(self.board, "void ReleaseTrackingLocked()", "static void PulseDvpReset()")
        self.assertIn("tracking_acquired_ = false", release)
        self.assertIn("DeinitDvpSafe()", release)
        self.assertNotIn("esp_camera_fb_get()", release)
        self.assertNotIn("esp_camera_fb_return", release)
        self.assertIn("SetDvpPowerDown(true)", self.board)

    def test_take_photo_explain_is_not_at_priority_one(self):
        mcp = (ROOT / "main/mcp_server.cc").read_text(encoding="utf-8")
        block = slice_between(mcp, 'AddTool("self.camera.take_photo"', "AddUserOnlyTools")
        reset_at = block.find("TaskPriorityReset")
        question_at = block.find("auto question")
        explain_at = block.find("camera->Explain")
        self.assertGreaterEqual(reset_at, 0)
        self.assertLess(reset_at, question_at)
        self.assertLess(question_at, explain_at)
        self.assertIn("Capture()", block[reset_at:question_at])
        self.assertNotIn("Explain", block[reset_at:question_at])
        cam = (ROOT / "main/boards/common/esp32_camera.cc").read_text(encoding="utf-8")
        self.assertIn("bool Esp32Camera::ReleaseSensorKeepJpeg()", cam)
        self.assertIn("bool Esp32Camera::ExportParkedJpeg", cam)
        self.assertIn("ExplainJpegUpload", cam)
        self.assertIn("jpeg_parked_len_", cam)

    def test_usb_console_and_jpeg_buffer(self):
        cfg = (ROOT / "main/boards/moss/moss-ov2640/config.json").read_text(encoding="utf-8")
        match = re.search(r"CONFIG_CAMERA_JPEG_MODE_FRAME_SIZE=(\d+)", cfg)
        self.assertIsNotNone(match)
        self.assertGreaterEqual(int(match.group(1)), 262144)
        self.assertIn("CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y", cfg)


class MossCameraDisarmRouteTests(unittest.TestCase):
    def test_disarm_is_registered_on_ov2640_api(self):
        api = API.read_text(encoding="utf-8")
        snapshot_at = api.find('add("/camera/snapshot"')
        self.assertGreaterEqual(snapshot_at, 0)
        block = api[snapshot_at : snapshot_at + 800]
        self.assertIn('add("/camera/disarm", HTTP_GET', block)
        self.assertIn('add("/camera/disarm", HTTP_POST', block)
        self.assertIn("HandleDisarm", block)
        self.assertIn('add("/gimbal/control"', block)
        self.assertIn('add("/face_track/control"', block)
        ifdef_at = api.rfind("#ifdef CONFIG_BOARD_TYPE_MOSS_OV2640", 0, snapshot_at)
        endif_at = api.find("#endif", snapshot_at)
        self.assertGreaterEqual(ifdef_at, 0)
        self.assertGreater(endif_at, snapshot_at)
        self.assertLess(snapshot_at, endif_at)

    def test_disarm_handler_calls_stream_disarm(self):
        header = HANDLERS_H.read_text(encoding="utf-8")
        src = HANDLERS.read_text(encoding="utf-8")
        self.assertIn("HandleDisarm", header)
        body = slice_between(src, "esp_err_t HandleDisarm", "esp_err_t HandleGimbal")
        self.assertIn("MossCameraStream::GetInstance().Disarm()", body)
        self.assertIn("SendJson", body)
