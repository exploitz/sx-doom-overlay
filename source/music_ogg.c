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
#include "z_zone.h"   // Z_Malloc

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

// IWAD basename (lowercase, no extension) — set by main.cpp before engine
// init via music_ogg_set_iwad. Used to scope music lookup to a per-WAD
// subdirectory (e.g. /music/chex/d_e1m1.ogg vs /music/doom/d_e1m1.ogg) so
// users can keep multiple OGG packs cached side-by-side and the runtime
// picks the right one for whichever WAD is loaded.
static char g_iwad_name[64] = "";

void music_ogg_set_iwad(const char* iwad_basename) {
    if (!iwad_basename) { g_iwad_name[0] = '\0'; return; }
    size_t i = 0;
    for (; i < sizeof(g_iwad_name) - 1 && iwad_basename[i]; ++i) {
        char c = iwad_basename[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);  // lowercase
        g_iwad_name[i] = c;
    }
    g_iwad_name[i] = '\0';
}

// OPL fallback for songs that have no matching OGG file (e.g. CHEX, Freedoom,
// custom WADs). When MusicRegister can't open an OGG, we hand the buffer to
// music_opl_module and music_ogg_render forwards to OPL_LIBNX_Render until
// the next song change. Keeps custom WADs from going silent.
extern music_module_t music_opl_module;            // source/opl/i_oplmusic.c
extern void           OPL_LIBNX_Render(int16_t* dst, size_t frames);
static int  g_opl_active     = 0;   // 1 when current song is on OPL fallback
static void* g_opl_handle    = NULL; // handle returned by OPL RegisterSong

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

// stb_vorbis allocation buffer.
//
// stb_vorbis defaults to malloc() from newlib heap; on Switch overlays
// that pool is heavily pressured (SFX cache, libnx services) and the
// first-decode allocation burst (~150-200 KB) can crash mid-alloc.
//
// We allocate from Doom's Z_Malloc zone instead. Trade-offs vs BSS:
//   - BSS: 256 KB cost is paid in binary size + loader image footprint,
//     coming out of the 8 MB overlay slider before our heap pool is
//     even sized. Pure addition to total memory use.
//   - Z_Malloc: borrows from the 4 MiB Doom zone where we currently use
//     ~3.5 MiB at peak. 256 KB there is invisible. Zero binary growth,
//     zero impact on newlib heap, zero impact on the 8 MB ceiling beyond
//     what the engine already commits. Only constraint is timing —
//     Z_Init must have run, which it has by the time S_Init →
//     InitMusicModule → DGMusic_Init runs.
// stb_vorbis's documented "comfortable" minimum is ~150 KB but real-world
// usage during decode can spike higher (codebook decoders, frame state
// per active book). 256 KB previously reached decode but crashed mid-call,
// strongly suggesting buffer overrun. Doubling to 512 KB. Cost is invisible
// in the 4 MiB Doom zone — engine peak is still ~3.5 MiB, leaves margin.
#define MUSIC_ALLOC_BYTES (512 * 1024)
static char* music_alloc_buffer = NULL;  // Z_Malloc'd in MusicInit

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
    int            sample_rate;      // OGG native (44100 or 48000 typical)
    int            stream_channels;  // OGG native — usually 1 or 2

    // Linear-interp resampler state, only used when sample_rate != 48000.
    // 16.16 fixed-point sub-sample position into a virtual stream where
    // sample -1 is `carry` (the last source frame from the prior render).
    uint32_t       resample_phase_q16;
    int16_t        resample_carry_l;
    int16_t        resample_carry_r;

    // Diagnostic: set to 1 after the first decode_with_loop_locked call
    // following a song change. Lets trace.log distinguish "audio thread
    // never reached decode" from "decode ran but produced nothing." Reset
    // by close_decoder_locked.
    int            first_decode_logged;

    // Counts every music_ogg_render entry. Logged every 50 calls so we
    // can see in trace.log how far the audio thread got past the first
    // decode without spamming the log.
    uint32_t       render_call_count;
} g_music;

#define MUSIC_OUTPUT_RATE 48000

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

// Tear down any active OPL fallback song. Called before swapping songs so the
// OPL player doesn't keep rendering a stale lump.
static void close_opl_fallback_locked(void) {
    if (!g_opl_active) return;
    music_opl_module.StopSong();
    if (g_opl_handle) {
        music_opl_module.UnRegisterSong(g_opl_handle);
        g_opl_handle = NULL;
    }
    g_opl_active = 0;
}

static void close_decoder_locked(void) {
    if (g_music.decoder) {
        stb_vorbis_close(g_music.decoder);
        g_music.decoder = NULL;
    }
    close_opl_fallback_locked();
    g_music.playing = 0;
    g_music.paused  = 0;
    // Reset resampler so a new song doesn't pick up the previous song's
    // tail-end sample as its interp partner.
    g_music.resample_phase_q16 = 0;
    g_music.resample_carry_l   = 0;
    g_music.resample_carry_r   = 0;
    g_music.first_decode_logged = 0;
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
    stb_vorbis_alloc alloc;
    stb_vorbis_alloc* alloc_ptr = NULL;
    if (music_alloc_buffer) {
        alloc.alloc_buffer            = music_alloc_buffer;
        alloc.alloc_buffer_length_in_bytes = MUSIC_ALLOC_BYTES;
        alloc_ptr = &alloc;
    }
    stb_vorbis* v = stb_vorbis_open_filename(path, &err, alloc_ptr);
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

// Aliases for engine-side lump remaps that depend on snd_musicdevice.
// Vanilla chocolate-doom's S_ChangeMusic turns mus_intro into mus_introa
// when snd_musicdevice is ADLIB or SB (the default). Most third-party OGG
// packs (including the canonical archive.org SC-55 set) only ship the
// non-aliased "intro" track. If the aliased file is missing, fall back to
// the canonical one before going to the silent test fallback.
static const char* music_name_alias(const char* name) {
    if (!name) return NULL;
    if (strcmp(name, "introa") == 0) return "intro";    // OPL alias
    return NULL;
}

// Resolve the music buffer to a filename and open it. Tries, in order:
//   <MUSIC_DIR>/<iwad>/d_<name>.ogg     (per-WAD pack — preferred)
//   <MUSIC_DIR>/<iwad>/d_<alias>.ogg
//   <MUSIC_DIR>/d_<name>.ogg            (flat — legacy users + single-WAD)
//   <MUSIC_DIR>/d_<alias>.ogg
//   <MUSIC_DIR>/test.ogg                (final smoke-test fallback)
// Caller must hold the music mutex.
static int open_song_for_buffer_locked(const void* buf) {
    const char* name = lookup_music_name(buf);
    if (name && name[0]) {
        char path[256];
        const char* alias = music_name_alias(name);

        // Per-WAD subdir lookup first. Lets users keep e.g. /music/doom/ +
        // /music/chex/ cached side-by-side and the right pack auto-selects.
        if (g_iwad_name[0]) {
            snprintf(path, sizeof(path), "%s/%s/d_%s.ogg",
                     MUSIC_DIR, g_iwad_name, name);
            if (try_open_path_locked(path)) return 1;
            if (alias) {
                snprintf(path, sizeof(path), "%s/%s/d_%s.ogg",
                         MUSIC_DIR, g_iwad_name, alias);
                if (try_open_path_locked(path)) return 1;
            }
        }

        // Flat fallback — original layout, still valid for users who only
        // play one WAD or who installed before the per-WAD layout existed.
        snprintf(path, sizeof(path), "%s/d_%s.ogg", MUSIC_DIR, name);
        if (try_open_path_locked(path)) return 1;
        if (alias) {
            snprintf(path, sizeof(path), "%s/d_%s.ogg", MUSIC_DIR, alias);
            if (try_open_path_locked(path)) return 1;
        }
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

    // Allocate stb_vorbis scratch from the Doom zone. PU_STATIC keeps it
    // pinned for the engine's lifetime so subsequent W_Read / Z_Malloc
    // churn never relocates or evicts our buffer.
    if (!music_alloc_buffer) {
        music_alloc_buffer = Z_Malloc(MUSIC_ALLOC_BYTES, PU_STATIC, NULL);
    }
    char tbuf[120];
    snprintf(tbuf, sizeof(tbuf),
             "music_ogg: Init (alloc_buffer=%p, %d KB)",
             (void*)music_alloc_buffer, MUSIC_ALLOC_BYTES / 1024);
    doom_trace(tbuf);

    // Bring up OPL too so it's ready as a per-song fallback when the OGG pack
    // doesn't have the requested track (CHEX, Freedoom, custom WADs).
    if (!music_opl_module.Init()) {
        doom_trace("music_ogg: OPL fallback init FAILED — silent for missing OGGs");
    } else {
        doom_trace("music_ogg: OPL fallback ready");
    }
    return true;
}

static void MusicShutdown(void) {
    MUSIC_LOCK();
    close_decoder_locked();
    MUSIC_UNLOCK();
    music_opl_module.Shutdown();
    doom_trace("music_ogg: Shutdown");
}

static void MusicSetVolume(int volume) {
    if (volume < 0)   volume = 0;
    if (volume > 127) volume = 127;
    MUSIC_LOCK();
    g_music.volume = volume;
    int opl_active = g_opl_active;
    MUSIC_UNLOCK();
    if (opl_active) music_opl_module.SetMusicVolume(volume);
}

static void MusicPause(void) {
    MUSIC_LOCK();
    g_music.paused = 1;
    int opl_active = g_opl_active;
    MUSIC_UNLOCK();
    if (opl_active) music_opl_module.PauseMusic();
}

static void MusicResume(void) {
    MUSIC_LOCK();
    g_music.paused = 0;
    int opl_active = g_opl_active;
    MUSIC_UNLOCK();
    if (opl_active) music_opl_module.ResumeMusic();
}

static void* MusicRegister(void* data, int len) {
    char tbuf[120];
    snprintf(tbuf, sizeof(tbuf), "music_ogg: RegisterSong len=%d data=%p", len, data);
    doom_trace(tbuf);

    MUSIC_LOCK();
    close_decoder_locked();                       // tear down any previous song
    int ok = open_song_for_buffer_locked(data);   // resolve lump → OGG path
    MUSIC_UNLOCK();

    if (ok) return (void*)0x1;

    // OGG miss — try OPL fallback so the WAD still gets music.
    void* h = music_opl_module.RegisterSong(data, len);
    MUSIC_LOCK();
    g_opl_active = (h != NULL) ? 1 : 0;
    g_opl_handle = h;
    MUSIC_UNLOCK();
    snprintf(tbuf, sizeof(tbuf),
             "music_ogg: OPL fallback %s handle=%p",
             h ? "engaged" : "FAILED", h);
    doom_trace(tbuf);
    return h;  // chocolate-doom calls PlaySong with this same handle
}

static void MusicUnRegister(void* handle) {
    MUSIC_LOCK();
    int   opl_active = g_opl_active;
    void* opl_h      = g_opl_handle;
    close_decoder_locked();   // also tears down any OPL fallback
    MUSIC_UNLOCK();
    (void)handle;             // close_decoder_locked already handled OPL
    (void)opl_active;
    (void)opl_h;
}

static void MusicPlay(void* handle, boolean looping) {
    MUSIC_LOCK();
    int opl_active = g_opl_active;
    MUSIC_UNLOCK();

    if (opl_active) {
        music_opl_module.PlaySong(handle, looping);
        char tbuf[80];
        snprintf(tbuf, sizeof(tbuf),
                 "music_ogg: Play loop=%d (OPL fallback)", (int)looping);
        doom_trace(tbuf);
        return;
    }

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
    int opl_active = g_opl_active;
    g_music.playing = 0;
    if (g_music.decoder) stb_vorbis_seek_start(g_music.decoder);
    MUSIC_UNLOCK();
    if (opl_active) music_opl_module.StopSong();
}

static boolean MusicIsPlaying(void) {
    MUSIC_LOCK();
    int opl_active = g_opl_active;
    int p = g_music.playing && !g_music.paused;
    MUSIC_UNLOCK();
    if (opl_active) return music_opl_module.MusicIsPlaying();
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

// Decode `frames` stereo frames into `out`, looping or stopping on EOF
// per g_music.looping. Always returns `frames` (pads with silence past
// EOF when not looping). Caller must hold the music mutex.
static void decode_with_loop_locked(int16_t* out, int frames) {
    // Diagnostic: log the very first decode after a song change so we can
    // tell from trace.log whether the audio thread is reaching the decoder
    // at all (vs crashing on the first call). Two markers — entry and a
    // post-call complete — bracket the stb_vorbis_get_samples call so we
    // can localize crashes inside vs outside the decoder.
    const int log_first = !g_music.first_decode_logged;
    if (log_first) {
        g_music.first_decode_logged = 1;
        char tbuf[120];
        snprintf(tbuf, sizeof(tbuf),
                 "music_ogg: first decode ENTER want=%d decoder=%p",
                 frames, (void*)g_music.decoder);
        doom_trace(tbuf);
    }
    int produced = 0;
    while (produced < frames) {
        const int want = frames - produced;
        int got = stb_vorbis_get_samples_short_interleaved(
            g_music.decoder, MUSIC_CHANNELS,
            out + produced * MUSIC_CHANNELS,
            want * MUSIC_CHANNELS);
        if (log_first) {
            // Only log the very first call's return so we don't spam.
            char tbuf[120];
            snprintf(tbuf, sizeof(tbuf),
                     "music_ogg: first decode RETURN got=%d", got);
            doom_trace(tbuf);
        }
        if (got <= 0) {
            if (g_music.looping) {
                stb_vorbis_seek_start(g_music.decoder);
                continue;
            }
            memset(out + produced * MUSIC_CHANNELS, 0,
                   (frames - produced) * MUSIC_CHANNELS * sizeof(int16_t));
            g_music.playing = 0;
            return;
        }
        produced += got;
    }
}

void music_ogg_render(int16_t* dst, size_t frames) {
    MUSIC_LOCK();
    int opl_active = g_opl_active;
    MUSIC_UNLOCK();

    // OPL fallback path — current song has no OGG, drive the FM synth into
    // the music bus directly. OPL_LIBNX_Render writes stereo int16 frames at
    // the same MUSIC_OUTPUT_RATE the bus mixer expects.
    if (opl_active) {
        OPL_LIBNX_Render(dst, frames);
        return;
    }

    MUSIC_LOCK();

    g_music.render_call_count++;
    // Log every 50th render so we can see in trace.log how far past the
    // first decode the audio thread gets before any crash. Spaced sparsely
    // so it doesn't dominate the log.
    if ((g_music.render_call_count % 50) == 1 && g_music.decoder) {
        char tbuf[140];
        snprintf(tbuf, sizeof(tbuf),
                 "music_ogg: render #%u (playing=%d looping=%d in_rate=%d phase=%u)",
                 g_music.render_call_count, g_music.playing, g_music.looping,
                 g_music.sample_rate, g_music.resample_phase_q16);
        doom_trace(tbuf);
    }

    if (!g_music.playing || g_music.paused || !g_music.decoder) {
        memset(dst, 0, frames * MUSIC_CHANNELS * sizeof(int16_t));
        MUSIC_UNLOCK();
        return;
    }

    const int in_rate = g_music.sample_rate;

    if (in_rate == MUSIC_OUTPUT_RATE) {
        // Native rate, no resampling.
        decode_with_loop_locked(dst, (int)frames);
    } else {
        // Linear-interp resample, 16.16 fixed-point phase.
        // Sized for FRAMES_PER_BUF=1100 output frames at any input rate up
        // to 2× output (i.e. 96 kHz source → 48 kHz output). Doom OST OGGs
        // are 44.1 or 48 kHz; this is generous slop.
        static int16_t scratch[2400 * MUSIC_CHANNELS];
        const int max_in = sizeof(scratch) / (MUSIC_CHANNELS * sizeof(int16_t));

        const uint32_t step = (uint32_t)(((uint64_t)in_rate << 16) / MUSIC_OUTPUT_RATE);
        const uint64_t end_phase = (uint64_t)g_music.resample_phase_q16
                                 + (uint64_t)step * (uint64_t)frames;
        // ceil(end_phase / 2^16) — number of source frames we need to
        // decode this call. Always ≥ 1 so the carry update below is sane.
        int needed = (int)((end_phase + 0xFFFFu) >> 16);
        if (needed < 1)        needed = 1;
        if (needed > max_in)   needed = max_in;

        decode_with_loop_locked(scratch, needed);

        uint32_t       phase  = g_music.resample_phase_q16;
        const int16_t  prev_l = g_music.resample_carry_l;
        const int16_t  prev_r = g_music.resample_carry_r;

        for (size_t i = 0; i < frames; ++i) {
            const uint32_t idx  = phase >> 16;
            const uint32_t frac = phase & 0xFFFFu;
            int16_t l0, r0, l1, r1;
            if (idx == 0) {
                l0 = prev_l; r0 = prev_r;
            } else {
                l0 = scratch[(idx - 1) * MUSIC_CHANNELS + 0];
                r0 = scratch[(idx - 1) * MUSIC_CHANNELS + 1];
            }
            if ((int)idx >= needed) {
                // End-of-buffer guard; in normal operation idx < needed.
                l1 = l0; r1 = r0;
            } else {
                l1 = scratch[idx * MUSIC_CHANNELS + 0];
                r1 = scratch[idx * MUSIC_CHANNELS + 1];
            }
            const int32_t l = (int32_t)l0
                + (((int32_t)(l1 - l0) * (int32_t)frac) >> 16);
            const int32_t r = (int32_t)r0
                + (((int32_t)(r1 - r0) * (int32_t)frac) >> 16);
            dst[i * MUSIC_CHANNELS + 0] = (int16_t)l;
            dst[i * MUSIC_CHANNELS + 1] = (int16_t)r;
            phase += step;
        }

        g_music.resample_phase_q16 = (uint32_t)(end_phase & 0xFFFFu);
        g_music.resample_carry_l   = scratch[(needed - 1) * MUSIC_CHANNELS + 0];
        g_music.resample_carry_r   = scratch[(needed - 1) * MUSIC_CHANNELS + 1];
    }

    // Volume scale (0..127 → 0..256). Skip the multiply at full volume.
    if (g_music.volume < 127) {
        const int32_t v = (g_music.volume * 256) / 127;
        const size_t total = frames * MUSIC_CHANNELS;
        for (size_t i = 0; i < total; ++i) {
            dst[i] = (int16_t)(((int32_t)dst[i] * v) >> 8);
        }
    }

    MUSIC_UNLOCK();
}
