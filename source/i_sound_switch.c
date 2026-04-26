// i_sound_switch.c — DG_sound_module + DG_music_module for sx-doom-overlay.
//
// Engine integration:
//   - Doom's i_sound.c calls our sound_module_t function table once
//     i_sound.c has chosen us via snd_sfxdevice (default SNDDEVICE_SB).
//   - Music is force-disabled at startup via -nomusic; DG_music_module is
//     present only so the linker resolves InitMusicModule's reference.
//
// Memory budget — TIGHT (~42 KB free after audio backend):
//   - Lazy load: each StartSound that hits an uncached lump decodes from
//     the WAD into an int16 mono buffer at OUTPUT_RATE (22050 Hz).
//   - LRU cache: total cached PCM bytes capped at SFX_CACHE_CAP_BYTES.
//     When a load would exceed the cap, evict from the tail (oldest
//     unlocked sound) until it fits. Sounds currently being mixed are
//     locked and never evicted.
//   - If the entire cache can't free enough room (all locked, or sound
//     bigger than the cap), StartSound returns -1 (silent fail). Engine
//     continues with no audible effect for that sound.
//
// Output path:
//   - Each I_UpdateSound tic mixes TIC_FRAMES of stereo audio from active
//     channels (audio_mixer) and pushes it through audio_glue to the
//     libnx audout backend. Backend ring buffer is 8 KB (~93 ms headroom).
//
// Licensed under GPLv2.

#include "audio_backend.h"
#include "audio_glue.h"
#include "audio_mixer.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"
#include "deh_str.h"
#include "m_misc.h"
#include "w_wad.h"
#include "z_zone.h"

// Trace logger lives in main.cpp (writes to sdmc:/config/.../trace.log).
// Declared extern "C" there for cross-language linkage.
extern void doom_trace(const char* msg);

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

#define NUM_CHANNELS         AUDIO_MIXER_CHANNELS         // 8
#define OUTPUT_RATE          AUDIO_BACKEND_SAMPLE_RATE    // 22050

// Total bytes of PCM held across all cached sounds. Sized to comfortably
// hold the larger Doom SFX at 48 kHz — pistol ≈49 KB, pstop ≈56 KB — plus
// 2-3 short sfx (item pickup, footsteps, etc.) coexisting. The earlier
// 32 KB cap was based on a pessimistic "70 KB free heap" reading that
// turned out to be the newlib arena's fordblks, not the overlay pool's
// real ceiling (~8 MB on the slider).
#define SFX_CACHE_CAP_BYTES  (192 * 1024)

// One tic at TICRATE=35 ≈ 28.6 ms. 48000 * 28.6 ms ≈ 1371 frames; round up
// to give the ring a touch of slack against scheduler jitter.
#define TIC_FRAMES           1400

// -----------------------------------------------------------------------------
// LRU cache
// -----------------------------------------------------------------------------

typedef struct cached_sound_s {
    int16_t*               pcm;          // mono int16 @ OUTPUT_RATE
    size_t                 length;       // frames in pcm
    size_t                 bytes;        // length * sizeof(int16_t)
    sfxinfo_t*             sfx;          // back-pointer to clear driver_data
    int                    lock_count;   // > 0 while currently being mixed
    struct cached_sound_s* prev;         // doubly-linked, head=MRU, tail=LRU
    struct cached_sound_s* next;
} cached_sound_t;

static cached_sound_t* g_cache_head = NULL;
static cached_sound_t* g_cache_tail = NULL;
static size_t          g_cache_bytes = 0;

static audio_mixer_t   g_mixer;
static sfxinfo_t*      g_channel_sfx[NUM_CHANNELS];
static boolean         g_initialized = false;
static boolean         g_use_sfx_prefix = true;

// Per-tic scratch — stereo, owns its storage in BSS so we don't malloc per tic.
static int16_t         g_tic_buf[TIC_FRAMES * 2];

// audio_backend handle — set by main.cpp via switch_audio_set_backend.
static audio_backend_t* g_backend = NULL;

// -----------------------------------------------------------------------------
// audio_glue.h impl
// -----------------------------------------------------------------------------

void switch_audio_set_backend(audio_backend_t* be) {
    g_backend = be;
}

bool switch_audio_submit(const int16_t* pcm, size_t frames) {
    if (!g_backend) return false;
    return audio_backend_submit(g_backend, pcm, frames);
}

// -----------------------------------------------------------------------------
// LRU helpers
// -----------------------------------------------------------------------------

static void cache_unlink(cached_sound_t* c) {
    if (c->prev) c->prev->next = c->next;
    else         g_cache_head  = c->next;
    if (c->next) c->next->prev = c->prev;
    else         g_cache_tail  = c->prev;
    c->prev = c->next = NULL;
}

static void cache_push_head(cached_sound_t* c) {
    c->prev = NULL;
    c->next = g_cache_head;
    if (g_cache_head) g_cache_head->prev = c;
    g_cache_head = c;
    if (!g_cache_tail) g_cache_tail = c;
}

static void cache_drop(cached_sound_t* c) {
    cache_unlink(c);
    if (c->sfx) c->sfx->driver_data = NULL;
    g_cache_bytes -= c->bytes;
    free(c->pcm);
    free(c);
}

// Evict from tail until adding `need` more bytes would still fit under cap.
// Skips locked entries (currently being mixed). Returns false if nothing
// more is evictable.
static boolean cache_make_room(size_t need) {
    while (g_cache_bytes + need > SFX_CACHE_CAP_BYTES) {
        cached_sound_t* victim = NULL;
        for (cached_sound_t* c = g_cache_tail; c; c = c->prev) {
            if (c->lock_count == 0) { victim = c; break; }
        }
        if (!victim) return false;
        cache_drop(victim);
    }
    return true;
}

// -----------------------------------------------------------------------------
// DMX decode (Doom DSXXXX format) → mono int16 @ OUTPUT_RATE
// -----------------------------------------------------------------------------
//
// DMX layout (from i_sdlsound.c / vanilla Doom):
//   bytes 0..1 : format magic 0x03 0x00
//   bytes 2..3 : little-endian u16 sample rate
//   bytes 4..7 : little-endian u32 sample count
//   bytes 8..23: 16-byte lead pad (skipped)
//   ... samples ...
//   last 16 bytes: trail pad (skipped)
// Samples are 8-bit unsigned PCM.

static boolean decode_dmx(const uint8_t* data, size_t len,
                          int16_t** out_pcm, size_t* out_frames) {
    if (len < 8 || data[0] != 0x03 || data[1] != 0x00) return false;

    uint32_t src_rate = (uint32_t)data[2] | ((uint32_t)data[3] << 8);
    uint32_t length   = (uint32_t)data[4] | ((uint32_t)data[5] << 8)
                      | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    if (length > len - 8 || length <= 48) return false;
    if (src_rate < 4000 || src_rate > 96000) return false;

    int32_t real_length = (int32_t)length - 32;
    if (real_length <= 0) return false;
    const uint8_t* samples = data + 8 + 16;

    uint64_t out_n64 = ((uint64_t)real_length * (uint64_t)OUTPUT_RATE)
                       / (uint64_t)src_rate;
    if (out_n64 < 1 || out_n64 > 0x100000ULL) return false;  // 1M-frame sanity cap

    size_t out_n = (size_t)out_n64;
    int16_t* pcm = (int16_t*)malloc(out_n * sizeof(int16_t));
    if (!pcm) return false;

    // Linear-interpolation resample. step = real_length / out_n in 16.16
    // fixed point. For each output sample, pos>>16 selects the source pair
    // (s0, s1) and (pos & 0xFFFF) is the fractional weight in [0..65535].
    // Nearest-neighbor (the prior version) introduced harsh aliasing on the
    // typical 11025 → 48000 ratio; linear interp removes most of it for
    // negligible CPU cost. Vanilla Doom's 8-bit DSXXXX content has limited
    // bandwidth so linear is plenty.
    const uint64_t step = ((uint64_t)real_length << 16) / (uint64_t)out_n;
    uint64_t       pos  = 0;
    const int32_t  last_idx = real_length - 1;
    for (size_t i = 0; i < out_n; ++i) {
        int32_t idx0 = (int32_t)(pos >> 16);
        int32_t idx1 = idx0 + 1;
        if (idx0 > last_idx) idx0 = last_idx;
        if (idx1 > last_idx) idx1 = last_idx;
        // 8-bit unsigned → centered around 0 (range -128..127).
        const int32_t s0 = (int32_t)samples[idx0] - 128;
        const int32_t s1 = (int32_t)samples[idx1] - 128;
        const int32_t frac = (int32_t)(pos & 0xFFFF);  // 0..65535
        // Linear blend in the centered 8-bit domain, then expand to 16-bit
        // by left-shift 8. Range: -128..127 → -32768..32512 (fits int16
        // exactly; *257 would overflow at the negative extreme).
        const int32_t blended = s0 + (((s1 - s0) * frac) >> 16);  // -128..127
        pcm[i] = (int16_t)(blended << 8);
        pos += step;
    }

    *out_pcm = pcm;
    *out_frames = out_n;
    return true;
}

// -----------------------------------------------------------------------------
// Lump load + cache insertion
// -----------------------------------------------------------------------------

static void switch_get_lump_name(sfxinfo_t* sfx, char* buf, size_t buf_len) {
    if (sfx->link != NULL) sfx = sfx->link;
    if (g_use_sfx_prefix) {
        M_snprintf(buf, buf_len, "ds%s", DEH_String(sfx->name));
    } else {
        M_StringCopy(buf, DEH_String(sfx->name), buf_len);
    }
}

// Returns the cached_sound_t for sfx, loading + caching it if necessary.
// NULL on any failure (lump missing, decode failure, OOM, doesn't fit).
static cached_sound_t* cache_get_or_load(sfxinfo_t* sfx) {
    if (sfx->driver_data) return (cached_sound_t*)sfx->driver_data;

    char name[16];
    switch_get_lump_name(sfx, name, sizeof(name));
    int lumpnum = W_CheckNumForName(name);
    if (lumpnum < 0) return NULL;
    int len = W_LumpLength(lumpnum);
    if (len < 8) return NULL;

    void* lump = W_CacheLumpNum(lumpnum, PU_STATIC);
    if (!lump) return NULL;

    int16_t* pcm = NULL;
    size_t   frames = 0;
    boolean  ok = decode_dmx((const uint8_t*)lump, (size_t)len, &pcm, &frames);
    W_ReleaseLumpNum(lumpnum);
    if (!ok) return NULL;

    size_t bytes = frames * sizeof(int16_t);
    if (bytes > SFX_CACHE_CAP_BYTES) {
        // Sound exceeds the entire cache cap; refuse rather than thrashing.
        char dbg[96];
        snprintf(dbg, sizeof(dbg),
                 "cache_load: %s too big (%zuB > cap %dB) — dropped",
                 sfx->name, bytes, SFX_CACHE_CAP_BYTES);
        doom_trace(dbg);
        free(pcm);
        return NULL;
    }
    if (!cache_make_room(bytes)) {
        free(pcm);
        return NULL;
    }

    cached_sound_t* c = (cached_sound_t*)calloc(1, sizeof(*c));
    if (!c) {
        free(pcm);
        return NULL;
    }
    c->pcm    = pcm;
    c->length = frames;
    c->bytes  = bytes;
    c->sfx    = sfx;
    c->lock_count = 0;
    cache_push_head(c);
    g_cache_bytes += bytes;
    sfx->driver_data = c;
    return c;
}

// -----------------------------------------------------------------------------
// sound_module_t implementations
// -----------------------------------------------------------------------------

static snddevice_t switch_sound_devices[] = {
    SNDDEVICE_SB,
    SNDDEVICE_PAS,
    SNDDEVICE_GENMIDI,
    SNDDEVICE_WAVEBLASTER,
    SNDDEVICE_SOUNDCANVAS,
    SNDDEVICE_AWE32,
};

static boolean I_Switch_Init(boolean use_sfx_prefix) {
    g_use_sfx_prefix = use_sfx_prefix ? true : false;
    audio_mixer_init(&g_mixer);
    for (int i = 0; i < NUM_CHANNELS; ++i) g_channel_sfx[i] = NULL;
    g_initialized = true;
    char buf[64];
    snprintf(buf, sizeof(buf), "I_Switch_Init: prefix=%d backend=%p",
             (int)use_sfx_prefix, (void*)g_backend);
    doom_trace(buf);
    return true;
}

static void I_Switch_Shutdown(void) {
    if (!g_initialized) return;
    while (g_cache_head) cache_drop(g_cache_head);
    g_cache_bytes = 0;
    g_initialized = false;
}

static int I_Switch_GetSfxLumpNum(sfxinfo_t* sfx) {
    char name[16];
    switch_get_lump_name(sfx, name, sizeof(name));
    return W_GetNumForName(name);
}

// Convert Doom's (vol 0..127, sep 0..254) into mixer's (vol_l, vol_r) 0..255.
// Same formula as i_sdlsound.
static void compute_lr(int vol, int sep, int* left, int* right) {
    int l = ((254 - sep) * vol) / 127;
    int r = (sep * vol) / 127;
    if (l < 0) l = 0; else if (l > 255) l = 255;
    if (r < 0) r = 0; else if (r > 255) r = 255;
    *left = l;
    *right = r;
}

static void I_Switch_Update(void) {
    if (!g_initialized) return;

    // Mix one tic's worth of stereo PCM and push to backend.
    audio_mixer_mix(&g_mixer, g_tic_buf, TIC_FRAMES);
    bool submit_ok = switch_audio_submit(g_tic_buf, TIC_FRAMES);

    // First-call trace + periodic heartbeat. Confirms tick wiring + backend
    // health without spamming the log every 28 ms.
    static uint32_t s_update_calls = 0;
    static bool     s_first_traced = false;
    s_update_calls++;
    if (!s_first_traced) {
        char buf[160];
        audio_backend_debug_t dbg;
        audio_backend_debug(g_backend, &dbg);
        snprintf(buf, sizeof(buf),
                 "I_Switch_Update: first call, backend=%p submit=%d "
                 "dead=%d primed=%d last_err=0x%08x step=%d",
                 (void*)g_backend, (int)submit_ok,
                 (int)dbg.dead, dbg.primed_count,
                 (unsigned)dbg.last_error, dbg.last_error_step);
        doom_trace(buf);
        s_first_traced = true;
    } else if ((s_update_calls % 350) == 0) {
        // ~10s @ 35 Hz
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "I_Switch_Update: tick=%u submit=%d cache=%zuB",
                 (unsigned)s_update_calls, (int)submit_ok, g_cache_bytes);
        doom_trace(buf);
    }

    // Reap finished channels: drop the lock so the cache can evict.
    for (int i = 0; i < NUM_CHANNELS; ++i) {
        sfxinfo_t* sfx = g_channel_sfx[i];
        if (!sfx) continue;
        if (!audio_mixer_is_playing(&g_mixer, i)) {
            cached_sound_t* c = (cached_sound_t*)sfx->driver_data;
            if (c && c->lock_count > 0) c->lock_count--;
            g_channel_sfx[i] = NULL;
        }
    }
}

static void I_Switch_UpdateSoundParams(int channel, int vol, int sep) {
    if (!g_initialized || channel < 0 || channel >= NUM_CHANNELS) return;
    if (!audio_mixer_is_playing(&g_mixer, channel)) return;
    int left, right;
    compute_lr(vol, sep, &left, &right);
    audio_mixer_channel_t* c = &g_mixer.chans[channel];
    c->vol_l = (uint8_t)left;
    c->vol_r = (uint8_t)right;
}

static int I_Switch_StartSound(sfxinfo_t* sfx, int channel, int vol, int sep) {
    if (!g_initialized || channel < 0 || channel >= NUM_CHANNELS) return -1;

    cached_sound_t* c = cache_get_or_load(sfx);

    // Trace first 8 StartSound calls so we can see name + load outcome.
    static uint32_t s_start_calls = 0;
    if (s_start_calls < 8) {
        s_start_calls++;
        char buf[96];
        snprintf(buf, sizeof(buf),
                 "I_Switch_StartSound[%u]: name=%s ch=%d vol=%d sep=%d cached=%p len=%zu",
                 (unsigned)s_start_calls,
                 (sfx && sfx->name[0]) ? sfx->name : "?",
                 channel, vol, sep, (void*)c, c ? c->length : 0);
        doom_trace(buf);
    }

    if (!c) return -1;

    // Release any previous sound on this channel.
    if (g_channel_sfx[channel]) {
        cached_sound_t* prev = (cached_sound_t*)g_channel_sfx[channel]->driver_data;
        if (prev && prev->lock_count > 0) prev->lock_count--;
    }

    int left, right;
    compute_lr(vol, sep, &left, &right);
    if (!audio_mixer_play(&g_mixer, channel, c->pcm, c->length,
                          (uint8_t)left, (uint8_t)right)) {
        return -1;
    }

    g_channel_sfx[channel] = sfx;
    c->lock_count++;
    // Promote to MRU.
    cache_unlink(c);
    cache_push_head(c);
    return channel;
}

static void I_Switch_StopSound(int channel) {
    if (!g_initialized || channel < 0 || channel >= NUM_CHANNELS) return;
    audio_mixer_stop(&g_mixer, channel);
    sfxinfo_t* sfx = g_channel_sfx[channel];
    if (sfx) {
        cached_sound_t* c = (cached_sound_t*)sfx->driver_data;
        if (c && c->lock_count > 0) c->lock_count--;
        g_channel_sfx[channel] = NULL;
    }
}

static boolean I_Switch_SoundIsPlaying(int channel) {
    if (!g_initialized || channel < 0 || channel >= NUM_CHANNELS) return false;
    return audio_mixer_is_playing(&g_mixer, channel) ? true : false;
}

sound_module_t DG_sound_module = {
    switch_sound_devices,
    sizeof(switch_sound_devices) / sizeof(switch_sound_devices[0]),
    I_Switch_Init,
    I_Switch_Shutdown,
    I_Switch_GetSfxLumpNum,
    I_Switch_Update,
    I_Switch_UpdateSoundParams,
    I_Switch_StartSound,
    I_Switch_StopSound,
    I_Switch_SoundIsPlaying,
    NULL,  // CacheSounds: NULL → engine skips precache, we lazy-load.
};

// -----------------------------------------------------------------------------
// DG_music_module — stub. Engine is launched with -nomusic, so InitMusicModule
// is never called and none of these run. The symbol exists only to satisfy
// i_sound.c's link reference.
// -----------------------------------------------------------------------------

static boolean I_Switch_MusicInit(void)            { return false; }
static void    I_Switch_MusicShutdown(void)        {}
static void    I_Switch_MusicSetVolume(int v)      { (void)v; }
static void    I_Switch_MusicPause(void)           {}
static void    I_Switch_MusicResume(void)          {}
static void*   I_Switch_MusicRegister(void* d, int l) { (void)d; (void)l; return NULL; }
static void    I_Switch_MusicUnRegister(void* h)   { (void)h; }
static void    I_Switch_MusicPlay(void* h, boolean l) { (void)h; (void)l; }
static void    I_Switch_MusicStop(void)            {}
static boolean I_Switch_MusicIsPlaying(void)       { return false; }
static void    I_Switch_MusicPoll(void)            {}

music_module_t DG_music_module = {
    NULL, 0,
    I_Switch_MusicInit,
    I_Switch_MusicShutdown,
    I_Switch_MusicSetVolume,
    I_Switch_MusicPause,
    I_Switch_MusicResume,
    I_Switch_MusicRegister,
    I_Switch_MusicUnRegister,
    I_Switch_MusicPlay,
    I_Switch_MusicStop,
    I_Switch_MusicIsPlaying,
    I_Switch_MusicPoll,
};
