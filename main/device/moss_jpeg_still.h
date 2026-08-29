#pragma once

#include <cstddef>
#include <cstdint>

// Still-capture policy. OV2640 hardware JPEG bands and drops SOI after PLL
// retune; stills use RGB565 (same path as face track) then software JPEG.
// Keep in sync with scripts/tests/test_jpeg_still.py.
namespace moss_jpeg_still {

// Server probe (vision/explain, test-token): OK through ~707KB; 500 at ~796KB.
constexpr int kExplainJpegMaxBytes = 524288;
constexpr int kStillXclkHz = 10000000;
constexpr int kGimbalWaitMaxMs = 1500;
constexpr int kGimbalSettleMs = 280;
constexpr int kAeSettleMinMs = 1200;
constexpr int kAeSettleMaxMs = 2400;
constexpr int kAeSettleMinFrames = 12;
constexpr int kStableStreak = 4;
constexpr int kStillSettleFrameDelayMs = 33;
constexpr int kLenStablePct = 8;
constexpr int kMinCompleteJpegBytes = 128;
constexpr int kSwJpegQualityStart = 92;
constexpr int kSwJpegQualityMin = 52;
constexpr int kSwJpegQualityStep = 8;
constexpr int kSwJpegMaxDownscales = 2;
constexpr int kSwJpegMinEdge = 160;

// Skip quality ladder when still far over budget; downscale only as fallback.
inline bool JpegFarOverBudget(size_t len, size_t max_bytes) {
    return max_bytes > 0 && len > max_bytes + max_bytes / 2;
}

inline bool CanDownscaleRgb(int width, int height) {
    return (width / 2) >= kSwJpegMinEdge && (height / 2) >= kSwJpegMinEdge;
}

inline bool RgbLooksComplete(size_t len, int width, int height) {
    if (width <= 0 || height <= 0) {
        return false;
    }
    const size_t need = static_cast<size_t>(width) * static_cast<size_t>(height) * 2;
    return len >= need;
}

inline bool LooksComplete(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len < static_cast<size_t>(kMinCompleteJpegBytes)) {
        return false;
    }
    if (buf[0] != 0xFF || buf[1] != 0xD8) {
        return false;
    }
    return buf[len - 2] == 0xFF && buf[len - 1] == 0xD9;
}

inline bool LenStable(size_t prev, size_t cur) {
    if (prev == 0 || cur == 0) {
        return false;
    }
    const size_t hi = prev > cur ? prev : cur;
    const size_t lo = prev < cur ? prev : cur;
    size_t lim = hi * static_cast<size_t>(kLenStablePct) / 100;
    if (lim < 512) {
        lim = 512;
    }
    return (hi - lo) <= lim;
}

inline bool WithinBudget(size_t len) {
    return len > 0 && len <= static_cast<size_t>(kExplainJpegMaxBytes);
}

inline int NextLowerSwQuality(int quality) {
    if (quality <= kSwJpegQualityMin) {
        return kSwJpegQualityMin;
    }
    const int next = quality - kSwJpegQualityStep;
    return next < kSwJpegQualityMin ? kSwJpegQualityMin : next;
}

struct SettleState {
    size_t last_len = 0;
    int streak = 0;
    int frames = 0;
};

enum class SettleEvent { Progress, Ready, Failed };

inline SettleEvent OnFrame(SettleState& st, bool complete, size_t len, int elapsed_ms) {
    st.frames++;
    if (complete) {
        if (LenStable(st.last_len, len)) {
            st.streak++;
        } else {
            st.streak = 1;
        }
        st.last_len = len;
    } else {
        st.streak = 0;
    }

    if (elapsed_ms >= kAeSettleMinMs && st.frames >= kAeSettleMinFrames &&
        st.streak >= kStableStreak) {
        return SettleEvent::Ready;
    }
    if (elapsed_ms >= kAeSettleMaxMs) {
        return (st.last_len > 0 && st.streak >= 2) ? SettleEvent::Ready : SettleEvent::Failed;
    }
    return SettleEvent::Progress;
}

}  // namespace moss_jpeg_still
