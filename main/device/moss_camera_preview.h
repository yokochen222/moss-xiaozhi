#pragma once

// Preview policy for ov2640 live JPEG. Keep in sync with moss-desktop/server/onboard-preview.mjs.
namespace moss_camera_preview {

constexpr int kGrabPeriodMs = 300;
constexpr int kIdleMs = 15000;
constexpr int kFirstWaitMs = 400;
constexpr int kRefreshWaitMs = 500;
constexpr int kFreshMs = 350;
constexpr int kSkipAfterStart = 0;
constexpr int kGrabAttempts = 4;
constexpr int kJpegQualityPreview = 10;
constexpr int kWarmupPreviewMinMs = 250;
constexpr int kWarmupPreviewMaxMs = 700;
constexpr int kWarmupPreviewMinFrames = 3;
constexpr int kLcdRenderIdleMs = 120;
constexpr int kLcdRenderLiveMs = 200;

}  // namespace moss_camera_preview
