// audio_backend_libnx.c — production audio backend for sx-doom-overlay.
//
// Implements audio_backend.h against libnx audout. Submitted PCM is enqueued
// into a small buffer pool; a dedicated thread drains the pool by calling
// audoutAppendAudioOutBuffer + audoutWaitPlayFinish (blocking pattern).
//
// **Skeleton — Task 9 fleshes this out on hardware.** This file exists now so
// the production Makefile has all symbols available when cross-compiling
// (Task 5). Init returns AUDIO_BACKEND_INIT_FAILED until Task 9 wires up
// audoutInitialize/audoutStartAudioOut and the drain thread. That keeps the
// silent-fallback path active by default — same behavior as if audoutInitialize
// had genuinely failed on the user's HOS+game combo.
//
// Why a stub now:
//   1. The cross-build smoke (Task 5) must link without missing symbols.
//   2. Both architectural paths (doomgeneric vs chocolate-doom-nx) share this
//      same audio_backend.h interface — this code survives the fork in the
//      road.
//
// Licensed under GPLv2.

#include "audio_backend.h"

#ifdef __SWITCH__
// Real Switch build — Task 9 will fill this out with libnx audout calls.
// Until then, return failure so doomgeneric / chocolate-doom-nx falls into
// the silent path automatically.
#include <stdlib.h>

struct audio_backend_s {
    int placeholder;
};

audio_backend_status_t audio_backend_init(const char* path_hint,
                                          audio_backend_t** out) {
    (void)path_hint;
    if (out) *out = NULL;
    // TASK 9: replace with audoutInitialize + audoutStartAudioOut + thread spawn.
    return AUDIO_BACKEND_INIT_FAILED;
}

bool audio_backend_submit(audio_backend_t* be, const int16_t* pcm, size_t frames) {
    (void)be; (void)pcm; (void)frames;
    return false;
}

void audio_backend_shutdown(audio_backend_t* be) {
    (void)be;
}

#else  // !__SWITCH__
// Non-Switch builds (e.g., desktop tests) use audio_backend_wav.c instead.
// This file only contributes to the production .ovl build.
#endif  // __SWITCH__
