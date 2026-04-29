// audio_mixer.c — see audio_mixer.h.
//
// Per-frame mixing math:
//
//   for each output frame:
//     int32_t accum_l = 0, accum_r = 0
//     for each active channel:
//       sample = channel.pcm[channel.pos]
//       accum_l += (sample * channel.vol_l) / 255
//       accum_r += (sample * channel.vol_r) / 255
//       channel.pos += 1
//       if pos >= length: channel.active = false
//     accum_l = (accum_l * master) / 255
//     accum_r = (accum_r * master) / 255
//     clamp accum_l, accum_r to int16
//     dst[frame*2 + 0] = accum_l
//     dst[frame*2 + 1] = accum_r
//
// Volume scaling uses 0–255 to align with Doom's internal volume scale and
// to avoid floating-point math (libnx homebrew prefers integer math for
// audio DSP — int32 accumulator is sufficient for 8 channels of 16-bit
// PCM at any volume).
//
// Licensed under GPLv2.

#include "audio_mixer.h"
#include <string.h>

void audio_mixer_init(audio_mixer_t* m) {
    if (!m) return;
    memset(m, 0, sizeof(*m));
    m->master_vol = 255;
}

bool audio_mixer_play(audio_mixer_t* m, int slot,
                      const int16_t* pcm, size_t length,
                      uint32_t step_fp,
                      uint8_t vol_l, uint8_t vol_r) {
    if (!m || slot < 0 || slot >= AUDIO_MIXER_CHANNELS || !pcm || length == 0) {
        return false;
    }
    if (step_fp == 0) step_fp = 0x10000;  // safety: 1.0 default
    audio_mixer_channel_t* c = &m->chans[slot];
    c->pcm     = pcm;
    c->length  = length;
    c->pos_fp  = 0;
    c->step_fp = step_fp;
    c->vol_l   = vol_l;
    c->vol_r   = vol_r;
    c->active  = true;
    return true;
}

void audio_mixer_stop(audio_mixer_t* m, int slot) {
    if (!m || slot < 0 || slot >= AUDIO_MIXER_CHANNELS) return;
    m->chans[slot].active = false;
}

bool audio_mixer_is_playing(const audio_mixer_t* m, int slot) {
    if (!m || slot < 0 || slot >= AUDIO_MIXER_CHANNELS) return false;
    return m->chans[slot].active;
}

static inline int16_t clamp_int16(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

void audio_mixer_mix(audio_mixer_t* m, int16_t* dst, size_t frames) {
    if (!m || !dst) {
        if (dst) memset(dst, 0, frames * 2 * sizeof(int16_t));
        return;
    }
    const int32_t master = m->master_vol;

    const uint64_t length_fp_max = ((uint64_t)0xFFFFFFFFFFFFFFFFULL);
    (void)length_fp_max;

    // Smoothed active-channel count for per-frame normalization. We track
    // an exponential follower (s_norm_q8 in 8.8 fixed point) instead of
    // dividing by the raw active count each frame — a sound starting or
    // ending mid-tic would produce an audible amplitude step on sustained
    // sounds otherwise. Time constant ~10 ms at 48 kHz.
    static int32_t s_norm_q8 = 1 << 8;   // == 1.0 at startup
    const int32_t  one_q8    = 1 << 8;
    const int32_t  alpha     = 12;       // out of 256 — smoothing aggressiveness

    for (size_t f = 0; f < frames; ++f) {
        int32_t acc_l = 0, acc_r = 0;
        int active_count = 0;

        for (int s = 0; s < AUDIO_MIXER_CHANNELS; ++s) {
            audio_mixer_channel_t* c = &m->chans[s];
            if (!c->active) continue;

            // 16.16 fixed-point pos. Whole part is the source-frame index.
            // Linear interpolation between consecutive source samples kills
            // the aliasing harshness from nearest-neighbor on heavy
            // 11025→48000 upsampling.
            const uint64_t whole = c->pos_fp >> 16;
            const uint32_t frac  = (uint32_t)(c->pos_fp & 0xFFFF);

            if (whole >= c->length) {
                c->active = false;
                continue;
            }

            const int32_t s0 = (int32_t)c->pcm[whole];
            const int32_t s1 = (whole + 1 < c->length)
                             ? (int32_t)c->pcm[whole + 1]
                             : s0;
            // Linear blend.
            const int32_t sample = s0 + (((s1 - s0) * (int32_t)frac) >> 16);

            acc_l += (sample * (int32_t)c->vol_l) / 255;
            acc_r += (sample * (int32_t)c->vol_r) / 255;
            active_count++;

            c->pos_fp += c->step_fp;
        }

        // Average instead of sum: per-frame normalization by the smoothed
        // active count keeps the output peak at single-sound max regardless
        // of how many channels are firing. No more clipping when 4-8 sounds
        // collide (gunshots + monster pain + door slams). Single sound stays
        // full volume because target == 1.0.
        const int32_t target_q8 = (active_count > 0)
                                ? (active_count << 8)
                                : one_q8;
        s_norm_q8 += ((target_q8 - s_norm_q8) * alpha) >> 8;
        if (s_norm_q8 < one_q8) s_norm_q8 = one_q8;
        acc_l = (acc_l << 8) / s_norm_q8;
        acc_r = (acc_r << 8) / s_norm_q8;

        acc_l = (acc_l * master) / 255;
        acc_r = (acc_r * master) / 255;

        // Belt-and-suspenders. With normalization above, only volume slider
        // pushes past int16 range, and even that's bounded.
        dst[f * 2 + 0] = clamp_int16(acc_l);
        dst[f * 2 + 1] = clamp_int16(acc_r);
    }
}
