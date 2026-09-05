#pragma once

// Shared analog / wake defaults for moss-onvif and moss-ov2640.
// Do not fork these per board. See docs/moss-boards.md.

// ES7210 MIC1 analog gain (dB). Raise if wake is deaf; lower if false-wake or clipping.
#define AUDIO_CODEC_INPUT_GAIN 37.5f

// Same as 78/xiaozhi-esp32 lichuang-dev BoxAudioCodec:
// AFE is always MR (one M + one R). MIC1 is the only analog mic. MIC2 is unused.
// TDM slot order is MIC1, MIC3, MIC2, MIC4 (#2036), so mask(0)|mask(1) is
// MIC1 + MIC3 ES8311 speaker loopback — not a second microphone.
