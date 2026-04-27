// audio_backend_libnx.c — libnx audout backend for sx-doom-overlay
//
// We own audout lifecycle ourselves: audoutInitialize + audoutStartAudioOut
// in init, audoutStopAudioOut + audoutExit in shutdown.
//
// Earlier (working-but-fragile) versions of this file relied on libtesla's
// ult::Audio::initialize() to open audout for us. That broke on real
// hardware: libtesla only calls Audio::initialize() when its useSoundEffects
// config flag is true, and Audio::initialize() can fail silently
// (audoutInitialize returns nonzero → m_initialized stays false, audout is
// never opened). Trace from a real run showed audoutAppendAudioOutBuffer
// returning 0xE401 (KernelError_InvalidHandle, "2001-0114") — the audout
// service handle had never been initialized at all.
//
// libnx audout uses NX_GENERATE_SERVICE_GUARD (reference-counted internally),
// so paired audoutInitialize / audoutExit calls stack safely with libtesla's
// own usage. UltraGB and other libtesla-based emulators follow this same
// "own your own audout" pattern.
//
// Memory budget on Switch overlay: ~70 KB free at audio init. We use:
//   - 4 audio buffers × 4 KB each   = 16 KB
//   - drain thread stack             =  4 KB
//   - ring buffer (between submit + drain) = 8 KB
//   total                            = 28 KB — fits.
//
// Coexistence with libtesla's UI sounds: libtesla's backgroundSoundThread
// also submits to audout for menu clicks etc. audout serializes submissions
// internally; both threads can submit safely. Worst case is brief audio
// interleaving when libtesla plays a UI sound during gameplay.
//
// On submit-time failure (audout returns error mid-stream), we mark the
// backend dead and drain. The drain thread exits cleanly. Subsequent
// audio_backend_submit calls return false; engine continues silently.
//
// Licensed under GPLv2.

#include "audio_backend.h"
#include "audio_glue.h"
#include "music_ogg.h"

#ifdef __SWITCH__

#include <stdlib.h>
#include <string.h>
#include <malloc.h>      // memalign
#include <switch.h>

#include "opl/opl_libnx.h"

// Buffer geometry. Latency = (BUFS * FRAMES_PER_BUF) / SAMPLE_RATE.
// 4 × 1100 frames @ 22050 Hz ≈ 200 ms — generous so the drain thread has
// plenty of slack against libtesla's variable composite cadence.
#define BUFS              4
#define FRAMES_PER_BUF    1100
#define BYTES_PER_FRAME   (AUDIO_BACKEND_CHANNELS * (AUDIO_BACKEND_BITS / 8))   // 4
#define BYTES_PER_BUF     (FRAMES_PER_BUF * BYTES_PER_FRAME)                    // 4400
#define BUF_ALIGN         0x1000  // libnx audout requires page-aligned buffers

// Ring buffer between submit + drain — 8 KB holds 2048 frames of headroom.
#define RING_FRAMES       2048
#define RING_BYTES        (RING_FRAMES * BYTES_PER_FRAME)

struct audio_backend_s {
    AudioOutBuffer  buffers[BUFS];
    void*           buf_data[BUFS];      // backing memory for each AudioOutBuffer
    Thread          drain_thread;
    Mutex           ring_mtx;
    int16_t*        ring;                // RING_FRAMES stereo samples (RING_BYTES total)
    size_t          ring_read;           // index in stereo-frame units
    size_t          ring_write;
    bool            running;             // set false to stop drain thread
    bool            dead;                // submit-time failure latch
    bool            own_session;         // we called audoutInitialize ourselves
    int             next_slot;           // next buffer index to submit (round-robin)
    uint32_t        last_error;          // libnx Result of last failed audout call
    int             last_error_step;     // -1 = N/A; 0..BUFS-1 = prime slot; BUFS = submit loop
    int             primed_count;        // how many buffers successfully reached audout
};

// Pop up to `frames` frames from the ring into out (interleaved L/R int16).
// Pads with silence if the ring is short. Returns how many frames were real.
static size_t ring_drain(struct audio_backend_s* be, int16_t* out, size_t frames) {
    mutexLock(&be->ring_mtx);
    size_t real = 0;
    for (size_t i = 0; i < frames; ++i) {
        if (be->ring_read == be->ring_write) break;
        out[i*2 + 0] = be->ring[be->ring_read*2 + 0];
        out[i*2 + 1] = be->ring[be->ring_read*2 + 1];
        be->ring_read = (be->ring_read + 1) % RING_FRAMES;
        ++real;
    }
    mutexUnlock(&be->ring_mtx);
    if (real < frames) {
        memset(out + real*2, 0, (frames - real) * BYTES_PER_FRAME);
    }
    return real;
}

// Drain thread loop. Two key correctness points vs the earlier (crashing) design:
//
//   1. FINITE timeout on audoutWaitPlayFinish (100 ms instead of UINT64_MAX) so
//      the thread periodically returns and can check `be->running`. On infinite
//      wait the shutdown path was hanging — by the time __appExit ran ult::Audio::exit,
//      our thread was still in the kernel wait, audout got torn out from under it.
//
//   2. FILTER for our own buffers. libtesla's backgroundSoundThread also submits
//      to audout (UI sounds via ult::Audio). audoutWaitPlayFinish returns ANY
//      freed buffer — if we get one of libtesla's, we must NOT touch it.
//
// Submission loop — UltraGB pattern (lib/UltraGB/source/gb_audio.h:1207+).
//
// Each iteration:
//   1. Lock libtesla's audio mutex (audio_lock_acquire).
//   2. audoutGetReleasedAudioOutBuffer (NON-BLOCKING) drains finished bufs.
//      We do NOT use audoutWaitPlayFinish — it crashes in our overlay context
//      and only dequeues one buffer at a time anyway.
//   3. Refill `next_slot` from our ring (silence-pads if engine starved) and
//      audoutAppendAudioOutBuffer.
//   4. Unlock.
//   5. svcSleepThread for one buffer's worth of time. With BUFS=4 buffers and
//      1100 frames @ 22050 Hz = 50 ms each, sleeping 50 ms keeps the queue
//      ~3 deep — same headroom UltraGB targets.
//
// On audout error we record it but DO NOT mark dead from here. The submit
// path keeps trying — transient errors during libtesla state changes have
// recovered in practice in UltraGB (resync block).
static void submit_thread_main(void* arg) {
    struct audio_backend_s* be = (struct audio_backend_s*)arg;

    // Buffer duration in nanoseconds: frames / sample_rate * 1e9.
    // 1100 / 22050 = ~49.886 ms.
    const uint64_t BUF_DURATION_NS =
        ((uint64_t)FRAMES_PER_BUF * 1000000000ULL) / (uint64_t)AUDIO_BACKEND_SAMPLE_RATE;

    // Music render scratch (stereo int16). music_ogg_render fills this
    // each iteration; we then mix it additively into the SFX-filled
    // output buffer with int16 clamping. Lives in BSS to avoid per-
    // iteration allocation. Sized for FRAMES_PER_BUF stereo frames.
    // (OPL stack is also compiled in but unused while DG_music_module
    // points at music_ogg_module — see source/i_sound_switch.c.)
    static int16_t music_scratch[FRAMES_PER_BUF * AUDIO_BACKEND_CHANNELS];

    while (be->running) {
        AudioOutBuffer* buf = &be->buffers[be->next_slot];
        int16_t* const out = (int16_t*)buf->buffer;

        // SFX path: ring_drain pulls SFX-only PCM from the engine.
        ring_drain(be, out, FRAMES_PER_BUF);

        // Music path: stream OGG into music_scratch on this thread, then
        // mix additively. SFX already pre-boosts via compute_lr in the
        // i_sound_switch.c path; music starts ~half-scale so the additive
        // sum stays under int16 unless both peak simultaneously, in which
        // case the saturator below catches it.
        music_ogg_render(music_scratch, FRAMES_PER_BUF);
        const int total_samples = FRAMES_PER_BUF * AUDIO_BACKEND_CHANNELS;
        const int32_t SFX_SCALE   = 160;   // 62%
        const int32_t MUSIC_SCALE = 160;   // 62%
        for (int i = 0; i < total_samples; ++i) {
            int32_t s = ((int32_t)out[i] * SFX_SCALE
                       + (int32_t)music_scratch[i] * MUSIC_SCALE) >> 8;
            if (s >  32767) s =  32767;
            if (s < -32768) s = -32768;
            out[i] = (int16_t)s;
        }

        buf->data_size   = BYTES_PER_BUF;
        buf->data_offset = 0;
        buf->next        = NULL;

        // Audout submit + cleanup of released buffers — under libtesla's
        // mutex so we never race its UI sound thread. Try-acquire with a
        // short timeout: if libtesla's audio thread is wedged holding the
        // mutex (real failure mode seen on hardware), skip this iteration
        // and try again next cycle. Missing one submit means audout
        // underflows ~22 ms; a permanent hang would freeze the system.
        Result rc = 0;
        if (audio_lock_try_acquire_ms(10)) {
            AudioOutBuffer* released = NULL;
            u32 released_count = 0;
            audoutGetReleasedAudioOutBuffer(&released, &released_count);  // non-blocking
            rc = audoutAppendAudioOutBuffer(buf);
            audio_lock_release();
        } else {
            // Treat as a transient failure — record but don't latch dead.
            rc = MAKERESULT(345 /*Module_Libnx*/, 116 /*Cancelled*/);
        }

        if (R_FAILED(rc)) {
            if (be->last_error == 0) {
                be->last_error = rc;
                be->last_error_step = be->next_slot;
            }
        } else {
            if (be->primed_count < BUFS) be->primed_count++;
        }

        be->next_slot = (be->next_slot + 1) % BUFS;
        svcSleepThread(BUF_DURATION_NS);
    }
}

audio_backend_status_t audio_backend_init(const char* path_hint,
                                          audio_backend_t** out) {
    (void)path_hint;
    if (!out) return AUDIO_BACKEND_INIT_FAILED;
    *out = NULL;

    // UltraGB-style ownership: only call audoutInitialize ourselves if
    // libtesla didn't (useSoundEffects=false, or its init failed silently).
    // Calling audoutInitialize when libtesla already opened audout is a
    // no-op refcount bump, but going through the m_initialized check matches
    // UltraGB's tested pattern and avoids the InvalidHandle crash we hit when
    // we always called it ourselves.
    bool own_session = false;
    if (!audio_libtesla_initialized()) {
        if (R_FAILED(audoutInitialize())) {
            return AUDIO_BACKEND_INIT_FAILED;
        }
        own_session = true;
    }

    // Stop+Start under libtesla's mutex for clean stream ownership. Stop
    // flushes any libtesla-queued buffers so our silence pre-roll plays first.
    audio_lock_acquire();
    audoutStopAudioOut();
    const Result start_rc = audoutStartAudioOut();
    audio_lock_release();
    if (R_FAILED(start_rc)) {
        if (own_session) {
            audoutExit();
        } else {
            // Best-effort restore libtesla's stream.
            audio_lock_acquire();
            audoutStartAudioOut();
            audio_lock_release();
        }
        return AUDIO_BACKEND_INIT_FAILED;
    }

    struct audio_backend_s* be = (struct audio_backend_s*)calloc(1, sizeof(*be));
    if (!be) {
        audio_lock_acquire();
        audoutStopAudioOut();
        audio_lock_release();
        if (own_session) audoutExit();
        return AUDIO_BACKEND_BUFFER_ALLOC_FAILED;
    }
    be->own_session = own_session;

    mutexInit(&be->ring_mtx);
    be->ring = (int16_t*)calloc(RING_FRAMES * AUDIO_BACKEND_CHANNELS, sizeof(int16_t));
    if (!be->ring) goto fail_free_be;

    const size_t aligned_sz = (BYTES_PER_BUF + (BUF_ALIGN - 1)) & ~(BUF_ALIGN - 1);
    for (int i = 0; i < BUFS; ++i) {
        be->buf_data[i] = memalign(BUF_ALIGN, aligned_sz);
        if (!be->buf_data[i]) goto fail_free_bufs;
        memset(be->buf_data[i], 0, aligned_sz);
        be->buffers[i].next        = NULL;
        be->buffers[i].buffer      = be->buf_data[i];
        be->buffers[i].buffer_size = aligned_sz;
        be->buffers[i].data_size   = BYTES_PER_BUF;
        be->buffers[i].data_offset = 0;
    }

    be->running         = true;
    be->dead            = false;
    be->last_error      = 0;
    be->last_error_step = -1;
    be->primed_count    = 0;
    be->next_slot       = 0;

    // 8 KB stack (was 4 KB). The submit thread runs OPL2 emulation +
    // i_oplmusic MIDI callback chain on top of audio submit + libtesla
    // mutex acquire — the original 4 KB was within margin and a real
    // failure mode (HID-thread abort during stage transition) suggested
    // memory corruption, possibly stack-adjacent. 8 KB has comfortable
    // headroom; cost is +4 KB BSS, trivial.
    if (R_FAILED(threadCreate(&be->drain_thread, submit_thread_main, be,
                              NULL, 0x2000, 0x2C, -2))) {
        goto fail_free_bufs;
    }
    if (R_FAILED(threadStart(&be->drain_thread))) {
        threadClose(&be->drain_thread);
        goto fail_free_bufs;
    }

    *out = be;
    return AUDIO_BACKEND_OK;

fail_free_bufs:
    for (int i = 0; i < BUFS; ++i) {
        if (be->buf_data[i]) free(be->buf_data[i]);
    }
    free(be->ring);
fail_free_be: {
    const bool was_own = be->own_session;
    free(be);
    audio_lock_acquire();
    audoutStopAudioOut();
    audio_lock_release();
    if (was_own) {
        audoutExit();
    } else {
        audio_lock_acquire();
        audoutStartAudioOut();
        audio_lock_release();
    }
    return AUDIO_BACKEND_BUFFER_ALLOC_FAILED;
}
}

bool audio_backend_submit(audio_backend_t* be, const int16_t* pcm, size_t frames) {
    if (!be || be->dead) return false;
    mutexLock(&be->ring_mtx);
    for (size_t i = 0; i < frames; ++i) {
        size_t next = (be->ring_write + 1) % RING_FRAMES;
        if (next == be->ring_read) break;  // ring full; drop newest
        be->ring[be->ring_write*2 + 0] = pcm[i*2 + 0];
        be->ring[be->ring_write*2 + 1] = pcm[i*2 + 1];
        be->ring_write = next;
    }
    mutexUnlock(&be->ring_mtx);
    return true;
}

void audio_backend_debug(const audio_backend_t* be, audio_backend_debug_t* out) {
    if (!out) return;
    if (!be) {
        out->last_error = 0;
        out->last_error_step = -1;
        out->primed_count = 0;
        out->dead = true;
        return;
    }
    out->last_error      = be->last_error;
    out->last_error_step = be->last_error_step;
    out->primed_count    = be->primed_count;
    out->dead            = be->dead;
}

void audio_backend_shutdown(audio_backend_t* be) {
    if (!be) return;
    extern void doom_trace(const char* msg);
    doom_trace("audio_backend_shutdown: enter");

    be->running = false;
    threadWaitForExit(&be->drain_thread);
    threadClose(&be->drain_thread);
    doom_trace("audio_backend_shutdown: submit thread stopped");

    // exitServices is the LAST user code to run before libtesla's __appExit
    // calls ult::Audio::exit (which does its own audoutStop+audoutExit).
    // We don't restart the audout stream here — libtesla doesn't need it
    // restored, the process is one syscall away from termination.
    //
    // try_acquire keeps us from blocking forever if libtesla's
    // backgroundSoundThread is mid-IPC; observed-on-hardware deadlock that
    // froze the whole Switch UI until sleep/wake reset state.
    if (audio_lock_try_acquire_ms(50)) {
        bool flush_dummy = false;
        audoutFlushAudioOutBuffers(&flush_dummy);
        audoutStopAudioOut();
        if (be->own_session) {
            audoutExit();
        }
        // (libtesla case: do nothing — its __appExit teardown handles audout.)
        audio_lock_release();
        doom_trace("audio_backend_shutdown: audout teardown done");
    } else {
        doom_trace("audio_backend_shutdown: skipped audout teardown (lock timeout)");
    }

    for (int i = 0; i < BUFS; ++i) {
        if (be->buf_data[i]) free(be->buf_data[i]);
    }
    free(be->ring);
    free(be);
    doom_trace("audio_backend_shutdown: exit");
}

#else  // !__SWITCH__
// Non-Switch builds (e.g., desktop tests) use audio_backend_wav.c instead.
#endif  // __SWITCH__
