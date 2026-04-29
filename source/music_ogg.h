// music_ogg.h — OGG-streamed music backend for sx-doom-overlay.
//
// Replaces the OPL3 FM synthesizer with a streaming OGG decoder. Doom's
// MIDI lumps are pre-rendered offline (FluidSynth + GM soundfont) into
// OGG files that live at /switch/sx-doom-overlay/music/<lumpname>.ogg.
//
// The engine still calls the music_module_t function table; we just
// satisfy those calls by streaming a file instead of synthesizing FM.
//
// Phase 1: stubs — silent music, build clean, no decode.
// Phase 2+: actual stb_vorbis-backed decode. See
//           docs/plans/2026-04-26-ogg-music.md for the full plan.
//
// Licensed under GPLv2.

#ifndef SX_MUSIC_OGG_H
#define SX_MUSIC_OGG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

// Render up to `frames` stereo int16 frames into `dst` from the currently-
// playing OGG. Pads with silence past EOF (or when no song is playing).
// Called from the audio submit thread, replacing OPL_LIBNX_Render.
void music_ogg_render(int16_t* dst, size_t frames);

// Tell the music backend which IWAD is loaded so it can scope music lookup
// to /music/<iwad>/d_*.ogg (per-WAD packs cached side-by-side). Pass the
// stem only — no path, no .wad extension. Case-insensitive (we lowercase).
// Pass NULL or "" to disable per-WAD lookup and use the flat /music/ path.
void music_ogg_set_iwad(const char* iwad_basename);

#ifdef __cplusplus
}
#endif

#endif  // SX_MUSIC_OGG_H
