#pragma once

// Shared analog / wake defaults for moss-onvif and moss-ov2640.
// Do not fork these per board. See docs/moss-boards.md.

// ES7210 MIC1 analog gain (dB). Raise if wake is deaf; lower if false-wake or clipping.
#define AUDIO_CODEC_INPUT_GAIN 37.5f
// ES7210 AEC reference-channel gain (dB). Must track MIC level or echo cancellation fails.
#define AUDIO_CODEC_REFERENCE_GAIN 30.0f
#define AUDIO_CODEC_REFERENCE_CHANNEL 2
