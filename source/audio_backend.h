// audio_backend.h — abstract audio sink for sx-doom-overlay
//
// The mixer (i_sound_switch.c) writes 16-bit signed stereo PCM at 22050 Hz
// through this interface. Two implementations:
//
//   - audio_backend_libnx.c   — production. Wraps libnx audout; runs on a
//                                dedicated thread (priority 0x2c, 4 KB stack).
//                                Init- and submit-time failures are caught
//                                and surfaced via the callback.
//
//   - audio_backend_wav.c     — desktop tests. Writes the PCM stream into a
//                                .wav file so unit tests can compare against
//                                expected output.
//
// The interface is C (not C++) because doomgeneric is C and i_sound_switch.c
// has to interop with it. -fno-exceptions on the C++ side stays compatible.
//
// Licensed under GPLv2.

#ifndef AUDIO_BACKEND_H
#define AUDIO_BACKEND_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Format constants. Both backends use the same values.
#define AUDIO_BACKEND_SAMPLE_RATE  22050
#define AUDIO_BACKEND_CHANNELS     2          // stereo
#define AUDIO_BACKEND_BITS         16         // signed PCM

// Status codes returned by audio_backend_init.
typedef enum {
    AUDIO_BACKEND_OK = 0,
    AUDIO_BACKEND_INIT_FAILED = 1,         // device unavailable
    AUDIO_BACKEND_BUFFER_ALLOC_FAILED = 2, // out of memory
} audio_backend_status_t;

// Opaque handle. Each backend implementation defines its own concrete type
// behind this typedef.
typedef struct audio_backend_s audio_backend_t;

// --- Lifecycle ---------------------------------------------------------------

// Open the audio device and start an output stream. Caller passes a path
// hint (used by the WAV backend; ignored by libnx). On success, *out is
// non-NULL.
//
// On Switch hardware this may fail at init time if the foreground game
// holds audout exclusively. On desktop with the WAV backend it can fail
// only on filesystem errors.
audio_backend_status_t audio_backend_init(const char* path_hint,
                                          audio_backend_t** out);

// Submit a PCM chunk. `frames` is in interleaved-stereo samples; each frame
// is two int16_t (left, right).
//
// Returns true on success. Returns false on submit-time failure (e.g.,
// foreground game grabbed audout after init succeeded). The caller should
// treat false as a permanent backend failure — drain the ring, set
// `g_audio_failed`, and stop submitting.
bool audio_backend_submit(audio_backend_t* be, const int16_t* pcm, size_t frames);

// Drain any pending output, close the device, free resources.
void audio_backend_shutdown(audio_backend_t* be);

#ifdef __cplusplus
}
#endif

#endif  // AUDIO_BACKEND_H
