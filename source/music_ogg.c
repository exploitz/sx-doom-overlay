// music_ogg.c — music_module_t implementation backed by stb_vorbis.
//
// Phase 1: stubs. The function table exists, the engine can call it without
// crashing, but no decode happens — music_ogg_render fills silence.
//
// Phase 2 will fill in:
//   - file open / decoder init in MusicRegister
//   - stb_vorbis pushdata pump in music_ogg_render
//   - clean shutdown in MusicShutdown / MusicStop
//
// Licensed under GPLv2.

#include "music_ogg.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doomtype.h"
#include "i_sound.h"

// Trace logger lives in main.cpp.
extern void doom_trace(const char* msg);

// -----------------------------------------------------------------------------
// State
// -----------------------------------------------------------------------------

// Phase 1 state is intentionally minimal: just a flag for "playing" so the
// engine's IsPlaying poll behaves coherently. Phase 2 adds:
//   - FILE* (or fd) for the open OGG
//   - stb_vorbis* decoder handle
//   - loop start / end sample positions
//   - current_volume (0..127) for per-sample gain
//   - looping flag
static struct {
    int playing;     // 1 between PlaySong and StopSong
    int paused;
    int volume;      // 0..127 from engine; unused in Phase 1
    int looping;     // unused in Phase 1
} g_music = { 0, 0, 127, 0 };

// -----------------------------------------------------------------------------
// music_module_t implementation
// -----------------------------------------------------------------------------

static boolean MusicInit(void) {
    doom_trace("music_ogg: Init (Phase 1 stub)");
    g_music.playing = 0;
    g_music.paused  = 0;
    g_music.volume  = 127;
    g_music.looping = 0;
    return true;
}

static void MusicShutdown(void) {
    doom_trace("music_ogg: Shutdown");
    g_music.playing = 0;
}

static void MusicSetVolume(int volume) {
    g_music.volume = volume;
}

static void MusicPause(void) {
    g_music.paused = 1;
}

static void MusicResume(void) {
    g_music.paused = 0;
}

static void* MusicRegister(void* data, int len) {
    // Phase 1: pretend to register. Real impl will open the OGG file
    // matching the lump's name and init the stb_vorbis decoder. Return a
    // non-NULL handle so the engine treats registration as successful.
    (void)data; (void)len;
    char buf[64];
    snprintf(buf, sizeof(buf), "music_ogg: RegisterSong stub len=%d", len);
    doom_trace(buf);
    return (void*)0x1;  // placeholder handle — engine only checks NULL/non-NULL
}

static void MusicUnRegister(void* handle) {
    (void)handle;
    // Phase 2: close the file, free decoder state.
}

static void MusicPlay(void* handle, boolean looping) {
    (void)handle;
    g_music.playing = 1;
    g_music.paused  = 0;
    g_music.looping = looping ? 1 : 0;
    char buf[48];
    snprintf(buf, sizeof(buf), "music_ogg: Play stub loop=%d", (int)looping);
    doom_trace(buf);
}

static void MusicStop(void) {
    g_music.playing = 0;
}

static boolean MusicIsPlaying(void) {
    return g_music.playing != 0;
}

static void MusicPoll(void) {
    // Engine poll hook (called per tic). The real OGG decode is driven on
    // the audio submit thread via music_ogg_render — not here.
}

static const snddevice_t music_ogg_devices[] = {
    SNDDEVICE_GENMIDI,   // any plausible value; engine only uses this for
                         // device-list filtering, which we bypass anyway
                         // (InitMusicModule binds unconditionally).
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

void music_ogg_render(int16_t* dst, size_t frames) {
    // Phase 1: silence. Phase 2 will:
    //   - if !playing or paused: memset silence, return
    //   - call stb_vorbis_get_samples_short_interleaved into dst
    //   - on EOF: if looping, seek to LOOPSTART and continue; else stop
    //   - apply volume scale ((s * g_music.volume) / 127) per sample
    memset(dst, 0, frames * 2 * sizeof(int16_t));
}
