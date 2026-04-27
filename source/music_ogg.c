// music_ogg.c — music_module_t implementation backed by stb_vorbis.
//
// Phase 2: hardcoded test path. RegisterSong opens
// /switch/sx-doom-overlay/music/test.ogg, music_ogg_render pumps decoded
// stereo PCM into the audio submit thread. No lump-name mapping yet —
// every song registration plays the same file. Phase 3 hooks the real
// lookup into musicinfo_t / lump-name resolution.
//
// Threading:
//   - RegisterSong / PlaySong / Stop / SetVolume run on the engine thread
//     (mostly D_DoomLoop tic context). They mutate g_music under
//     g_music_mtx.
//   - music_ogg_render runs on the audio submit thread (the one driving
//     audoutAppendAudioOutBuffer in audio_backend_libnx.c). It also
//     locks g_music_mtx so engine-side state changes don't race a decode.
//   - The lock is held only for the brief stb_vorbis call; never held
//     across an audout submit or libtesla mutex.
//
// Licensed under GPLv2.

#include "music_ogg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __SWITCH__
#include <switch.h>
#endif

#include "doomtype.h"
#include "i_sound.h"
#include "sounds.h"   // S_music[], NUMMUSIC, musicinfo_t

// Pulldata file API only — we don't need pushdata for streaming from disk.
// Keep integer conversion path on (we use stb_vorbis_get_samples_short).
//
// stb_vorbis is vendored as `stb_vorbis_impl.h` rather than `.c` so the
// Makefile's `source/*.c` wildcard doesn't try to compile it as its own
// translation unit (which would multiply-define every API symbol against
// our #include here). It's still pure C — the file extension is just to
// keep the build system from picking it up twice.
#define STB_VORBIS_NO_PUSHDATA_API
#include "stb_vorbis_impl.h"

// Trace logger lives in main.cpp.
extern void doom_trace(const char* msg);

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

// Music asset directory on SD. Per-lump OGG files live as
//   <MUSIC_DIR>/d_<lumpname>.ogg   (e.g. /.../music/d_e1m1.ogg)
// matching chocolate-doom's lump naming convention. The render script in
// scripts/render-music.sh produces files at exactly this layout.
#define MUSIC_DIR  "sdmc:/switch/sx-doom-overlay/music"

// Fallback path used when we can't resolve the lump to a name. Useful as
// a "drop one OGG and hear it on every track" smoke test.
#define MUSIC_TEST_PATH  MUSIC_DIR "/test.ogg"

// stb_vorbis returns floats internally; the int16 helper clamps to int16
// range. We get stereo by passing channels=2; mono OGGs will upmix as
// stb_vorbis duplicates the single channel.
#define MUSIC_CHANNELS   2

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

static struct {
#ifdef __SWITCH__
    Mutex          mtx;
#endif
    stb_vorbis*    decoder;          // NULL when no song loaded
    int            playing;
    int            paused;
    int            looping;
    int            volume;           // 0..127 (Doom's range)
    int            sample_rate;      // OGG native — informational only
    int            stream_channels;  // OGG native — usually 1 or 2
} g_music;

#ifdef __SWITCH__
#define MUSIC_LOCK()    mutexLock(&g_music.mtx)
#define MUSIC_UNLOCK()  mutexUnlock(&g_music.mtx)
#else
#define MUSIC_LOCK()    ((void)0)
#define MUSIC_UNLOCK()  ((void)0)
#endif

// -----------------------------------------------------------------------------
// Internal helpers
// -----------------------------------------------------------------------------

static void close_decoder_locked(void) {
    if (g_music.decoder) {
        stb_vorbis_close(g_music.decoder);
        g_music.decoder = NULL;
    }
    g_music.playing = 0;
    g_music.paused  = 0;
}

// Walk the engine's S_music[] table to find the entry whose `data` pointer
// matches the buffer chocolate-doom just handed us. The matching entry's
// `name` field ("e1m1", "intro", "bunny", …) is what we use to resolve
// the OGG path. Returns NULL if no entry matches (shouldn't happen in
// practice — every RegisterSong call comes from S_ChangeMusic which sets
// `music->data` immediately before calling us).
static const char* lookup_music_name(const void* buf) {
    if (!buf) return NULL;
    for (int i = 1; i < NUMMUSIC; ++i) {
        if (S_music[i].data == buf) {
            return S_music[i].name;   // points into engine static memory
        }
    }
    return NULL;
}

// Try a single candidate path. Caller must hold the music mutex.
// Returns 1 on success and populates g_music.decoder, 0 otherwise.
static int try_open_path_locked(const char* path) {
    int err = 0;
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, NULL);
    if (!v) {
        char tbuf[200];
        snprintf(tbuf, sizeof(tbuf),
                 "music_ogg: open miss err=%d path=%s", err, path);
        doom_trace(tbuf);
        return 0;
    }
    stb_vorbis_info info = stb_vorbis_get_info(v);
    g_music.decoder         = v;
    g_music.sample_rate     = (int)info.sample_rate;
    g_music.stream_channels = info.channels;
    char tbuf[200];
    snprintf(tbuf, sizeof(tbuf),
             "music_ogg: opened %s — rate=%d channels=%d",
             path, g_music.sample_rate, g_music.stream_channels);
    doom_trace(tbuf);
    return 1;
}

// Resolve the music buffer to a filename and open it. Tries
//   <MUSIC_DIR>/d_<name>.ogg
// then falls back to
//   <MUSIC_DIR>/test.ogg
// so a user without a full pack can still verify the audio chain.
// Caller must hold the music mutex.
static int open_song_for_buffer_locked(const void* buf) {
    const char* name = lookup_music_name(buf);
    if (name && name[0]) {
        char path[256];
        snprintf(path, sizeof(path), "%s/d_%s.ogg", MUSIC_DIR, name);
        if (try_open_path_locked(path)) return 1;
    }
    // Fallback for first-launch smoke tests / users without a full pack.
    return try_open_path_locked(MUSIC_TEST_PATH);
}

// -----------------------------------------------------------------------------
// music_module_t implementation
// -----------------------------------------------------------------------------

static boolean MusicInit(void) {
#ifdef __SWITCH__
    mutexInit(&g_music.mtx);
#endif
    g_music.decoder         = NULL;
    g_music.playing         = 0;
    g_music.paused          = 0;
    g_music.looping         = 0;
    g_music.volume          = 127;
    g_music.sample_rate     = 0;
    g_music.stream_channels = 0;
    doom_trace("music_ogg: Init");
    return true;
}

static void MusicShutdown(void) {
    MUSIC_LOCK();
    close_decoder_locked();
    MUSIC_UNLOCK();
    doom_trace("music_ogg: Shutdown");
}

static void MusicSetVolume(int volume) {
    if (volume < 0)   volume = 0;
    if (volume > 127) volume = 127;
    MUSIC_LOCK();
    g_music.volume = volume;
    MUSIC_UNLOCK();
}

static void MusicPause(void) {
    MUSIC_LOCK();
    g_music.paused = 1;
    MUSIC_UNLOCK();
}

static void MusicResume(void) {
    MUSIC_LOCK();
    g_music.paused = 0;
    MUSIC_UNLOCK();
}

static void* MusicRegister(void* data, int len) {
    char tbuf[120];
    snprintf(tbuf, sizeof(tbuf), "music_ogg: RegisterSong len=%d data=%p", len, data);
    doom_trace(tbuf);

    MUSIC_LOCK();
    close_decoder_locked();                       // tear down any previous song
    int ok = open_song_for_buffer_locked(data);   // resolve lump → OGG path
    MUSIC_UNLOCK();

    return ok ? (void*)0x1 : NULL;
}

static void MusicUnRegister(void* handle) {
    (void)handle;
    MUSIC_LOCK();
    close_decoder_locked();
    MUSIC_UNLOCK();
}

static void MusicPlay(void* handle, boolean looping) {
    (void)handle;
    MUSIC_LOCK();
    g_music.playing = (g_music.decoder != NULL);
    g_music.paused  = 0;
    g_music.looping = looping ? 1 : 0;
    MUSIC_UNLOCK();
    char tbuf[80];
    snprintf(tbuf, sizeof(tbuf),
             "music_ogg: Play loop=%d (decoder=%s)",
             (int)looping, g_music.decoder ? "yes" : "no");
    doom_trace(tbuf);
}

static void MusicStop(void) {
    MUSIC_LOCK();
    g_music.playing = 0;
    if (g_music.decoder) stb_vorbis_seek_start(g_music.decoder);
    MUSIC_UNLOCK();
}

static boolean MusicIsPlaying(void) {
    MUSIC_LOCK();
    int p = g_music.playing && !g_music.paused;
    MUSIC_UNLOCK();
    return p ? true : false;
}

static void MusicPoll(void) {
    // No-op. Decode is driven from music_ogg_render on the audio thread.
}

static const snddevice_t music_ogg_devices[] = {
    SNDDEVICE_GENMIDI,
};

music_module_t music_ogg_module = {
    (snddevice_t*)music_ogg_devices,
    sizeof(music_ogg_devices) / sizeof(*music_ogg_devices),
    MusicInit,
    MusicShutdown,
    MusicSetVolume,
    MusicPause,
    MusicResume,
    MusicRegister,
    MusicUnRegister,
    MusicPlay,
    MusicStop,
    MusicIsPlaying,
    MusicPoll,
};

// -----------------------------------------------------------------------------
// Audio thread render entry point
// -----------------------------------------------------------------------------
//
// Called once per audio submit buffer. Pulls up to `frames` stereo int16
// samples from the OGG decoder into `dst`. Pads with silence past EOF (or
// when no song is playing). Applies music volume scaling on the fly.
//
// stb_vorbis returns the number of *samples per channel* it actually
// produced; we treat any short read as "EOF reached" and either loop
// (re-seek to start) or stop.

void music_ogg_render(int16_t* dst, size_t frames) {
    MUSIC_LOCK();

    if (!g_music.playing || g_music.paused || !g_music.decoder) {
        memset(dst, 0, frames * MUSIC_CHANNELS * sizeof(int16_t));
        MUSIC_UNLOCK();
        return;
    }

    size_t produced = 0;
    while (produced < frames) {
        const int want = (int)(frames - produced);
        int got = stb_vorbis_get_samples_short_interleaved(
            g_music.decoder,
            MUSIC_CHANNELS,
            dst + produced * MUSIC_CHANNELS,
            want * MUSIC_CHANNELS);
        if (got <= 0) {
            if (g_music.looping) {
                stb_vorbis_seek_start(g_music.decoder);
                continue;  // try again from the top
            } else {
                // Pad remainder with silence and stop.
                memset(dst + produced * MUSIC_CHANNELS, 0,
                       (frames - produced) * MUSIC_CHANNELS * sizeof(int16_t));
                g_music.playing = 0;
                break;
            }
        }
        produced += (size_t)got;
    }

    // Volume scale (0..127 → 0..256). Skip the multiply when at full
    // volume to save cycles in the common case.
    if (g_music.volume < 127) {
        const int32_t v = (g_music.volume * 256) / 127;
        const size_t total = produced * MUSIC_CHANNELS;
        for (size_t i = 0; i < total; ++i) {
            dst[i] = (int16_t)(((int32_t)dst[i] * v) >> 8);
        }
    }

    MUSIC_UNLOCK();
}
