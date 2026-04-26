// audio_glue.h — bridge between i_sound_switch.c (C) and main.cpp (C++).
//
// main.cpp owns the audio_backend lifetime via DoomOverlay::initServices /
// exitServices. i_sound_switch.c needs to push mixed PCM into that backend
// every tic from inside the engine. Rather than thread the backend pointer
// through doomgeneric's sound_module_t (which has no engine-private context),
// main.cpp publishes the handle here and i_sound_switch.c reads it.
//
// Licensed under GPLv2.

#ifndef AUDIO_GLUE_H
#define AUDIO_GLUE_H

#include "audio_backend.h"

#ifdef __cplusplus
extern "C" {
#endif

void switch_audio_set_backend(audio_backend_t* be);

bool switch_audio_submit(const int16_t* pcm, size_t frames);

// libtesla audio mutex bridge — defined in audio_lock.cpp.
// audio_backend_libnx.c locks this around audout calls so we never race
// libtesla's UI sound thread (which submits to the same audout stream).
void audio_lock_acquire(void);
void audio_lock_release(void);
bool audio_libtesla_initialized(void);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_GLUE_H
