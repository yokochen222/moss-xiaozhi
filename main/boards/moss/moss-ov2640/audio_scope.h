#pragma once

#include <cstddef>
#include <cstdint>

namespace moss_splash {

enum class AudioScopeSource : uint8_t {
    Capture = 0,
    Playback = 1,
};

// 仅信号弹窗打开时使能; 关闭后 Feed 只读一次 atomic.
void audio_scope_set_enabled(bool enabled);

// codec Read/Write 之后调用. 禁止分配、加锁或改写 PCM.
void audio_scope_feed(AudioScopeSource source, const int16_t* samples, size_t count, int channels);

// 拷贝滚动包络到 dst[0..width), 右侧最新, 取值 0..255. 无近期音频返回 false.
bool audio_scope_copy_envelope(uint8_t* dst, int width);

}  // namespace moss_splash
