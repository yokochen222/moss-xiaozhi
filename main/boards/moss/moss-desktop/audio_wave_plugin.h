#pragma once

#include "config.h"

#include <cstdint>

#ifndef MOSS_AUDIO_WAVE_PLUGIN
#define MOSS_AUDIO_WAVE_PLUGIN 1
#endif

namespace moss_wave {

#if MOSS_AUDIO_WAVE_PLUGIN

void Attach();
void Detach();
void SetPlaybackSource(bool playback);
bool CopyBins(uint8_t* dst, int n, int64_t now_us);

#else

inline void Attach() {}
inline void Detach() {}
inline void SetPlaybackSource(bool) {}
inline bool CopyBins(uint8_t*, int, int64_t) { return false; }

#endif

}  // namespace moss_wave
