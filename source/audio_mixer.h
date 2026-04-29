// audio_mixer.h — minimal 8-channel SFX mixer for sx-doom-overlay
//
// Pure mixing math, decoupled from the source format (Doom DSXXXX lumps),
// the engine integration (i_sound_switch.c), and the output backend
// (audio_backend.h). Lets us unit-test the math against synthesized inputs
// without needing the full Doom engine running.
//
// Scope (v1):
//   - 8 simultaneous channels, each with: 16-bit PCM source, position
//     (read pointer), length, volume_l (0–255), volume_r (0–255), playing flag.
//   - Each call to mix_frames produces N stereo frames (16-bit signed) by
//     summing active channels with per-channel pan/volume scaling and
//     clamping to int16 range.
//   - No music synth (deferred to Task 9 hardware integration).
//   - No resampling (caller is responsible for ensuring source rate matches
//     output rate; see vanilla Doom convention of resampling lumps once at
//     load time).
//
// Licensed under GPLv2.

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_MIXER_CHANNELS 8

typedef struct audio_mixer_channel_s {
    const int16_t* pcm;       // owned externally; mixer does not free
    size_t         length;    // number of frames in pcm (at SOURCE rate)
    uint64_t       pos_fp;    // 16.16 fixed-point read position (whole | frac)
    uint32_t       step_fp;   // 16.16 fixed-point per-output-frame increment
                              // = (source_rate << 16) / output_rate
                              // 0x10000 means same-rate playback (no resample)
    uint8_t        vol_l;     // 0–255
    uint8_t        vol_r;     // 0–255
    bool           active;    // true while pcm is being mixed
} audio_mixer_channel_t;

typedef struct audio_mixer_s {
    audio_mixer_channel_t chans[AUDIO_MIXER_CHANNELS];
    uint8_t               master_vol;  // 0–255, applied AFTER per-channel
} audio_mixer_t;

// Initialize all channels inactive, master volume = 255 (full).
void audio_mixer_init(audio_mixer_t* m);

// Start a sound on channel `slot`. Returns true if started, false if slot
// is invalid or `pcm` is NULL. step_fp is the per-output-frame increment
// in 16.16 fixed point. For "play at native rate" pass 0x10000. For
// resample-on-mix from a slower source, pass (src_rate << 16) / out_rate.
// length is in source-rate frames.
bool audio_mixer_play(audio_mixer_t* m, int slot,
                      const int16_t* pcm, size_t length,
                      uint32_t step_fp,
                      uint8_t vol_l, uint8_t vol_r);

// Stop a channel.
void audio_mixer_stop(audio_mixer_t* m, int slot);

// Is the channel still playing?
bool audio_mixer_is_playing(const audio_mixer_t* m, int slot);

// Mix `frames` stereo frames into `dst` (interleaved L/R int16). Active
// channels are advanced by `frames` samples each (or finished if their
// length is exhausted). `dst` is written, not added — callers should
// expect this to overwrite, not blend.
void audio_mixer_mix(audio_mixer_t* m, int16_t* dst, size_t frames);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_MIXER_H
