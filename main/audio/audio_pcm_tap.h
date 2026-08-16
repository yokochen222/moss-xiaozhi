#pragma once

#include <cstddef>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

// Optional PCM observe hook. audio_codec.cc provides weak no-ops.
// A board plugin may override with strong definitions.
// Called after codec I/O; must not block, allocate, or fail the audio path.
void AudioPcmTap_OnInput(const int16_t* data, size_t samples, int channels);
void AudioPcmTap_OnOutput(const int16_t* data, size_t samples, int channels);

#ifdef __cplusplus
}
#endif
