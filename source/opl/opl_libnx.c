// opl_libnx.c — libnx OPL2/3 driver for sx-doom-overlay.
//
// Replaces opl_sdl.c (chocolate-doom's reference driver). Same opl_driver_t
// interface so i_oplmusic.c is unchanged. Difference: we do NOT spawn an
// audio thread — our submit thread (audio_backend_libnx.c::submit_thread_main)
// drives rendering by calling OPL_LIBNX_Render() once per buffer.
//
// Rendering model (port of OPL_Mix_Callback in opl_sdl.c):
//   For each output buffer:
//     while not full:
//       next_callback_time = peek queue
//       nsamples = min(remaining, samples_until_next_callback)
//       OPL3_GenerateStream(&opl_chip, dst+filled*2, nsamples)
//       AdvanceTime(nsamples)   ← fires queued callbacks whose expire <= now
//     submit
//
// Locking matches chocolate-doom's split:
//   callback_queue_mutex — short-lived, around queue ops only
//   callback_mutex       — held while invoking a callback (so OPL_Lock from
//                          another context can block callbacks for a
//                          critical section)
// The mutexes are separate so OPL_SetCallback (called from the callback
// itself to reschedule) doesn't recurse-deadlock.
//
// Licensed under GPLv2.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

#include "opl.h"
#include "opl_internal.h"
#include "opl_queue.h"
#include "opl3.h"

// One microsecond expressed as a count toward 1 second.
#ifndef OPL_SECOND
#define OPL_SECOND 1000000ULL
#endif

// Sample rate the OPL emulator runs at. Must match what i_oplmusic and the
// submit thread expect (see opl.c — opl_sample_rate). Default to 48 kHz to
// match Switch audout's device rate so we can blit the OPL output directly
// without resampling.
#ifndef OPL_LIBNX_SAMPLE_RATE
#define OPL_LIBNX_SAMPLE_RATE 48000
#endif

// ---- State ------------------------------------------------------------------

static opl3_chip                  opl_chip;
static int                        opl_chip_inited = 0;

static opl_callback_queue_t*      callback_queue;
static Mutex                      callback_mutex;
static Mutex                      callback_queue_mutex;

static uint64_t                   current_time;     // µs since init
static uint64_t                   pause_offset;     // µs spent paused
static int                        opl_paused;

static int                        register_num;     // last selected OPL register

// Timer status emulation — minimum needed to make OPL_Detect succeed.
// Detection writes a sequence of timer-control commands and polls the
// status port. We model just enough state for those polls to return what
// Doom expects. See OPL_LIBNX_PortRead for the full protocol.
#define OPL_REG_TIMER_CTRL   0x04
static int timer1_running = 0;

// Output scratch — Nuked-OPL3 writes 2 int16 (stereo) per sample. We render
// in chunks of up to MAX_CHUNK frames between callback firings. 128 frames
// covers the worst-case sub-millisecond MIDI tick spacing without forcing a
// per-sample function call.
#define MAX_CHUNK 256
static int16_t scratch_chunk[MAX_CHUNK * 2];

// Diagnostic counters (zeroed in Init). Inspected via OPL_LIBNX_DebugSnapshot.
static uint32_t g_total_writes        = 0;
static uint32_t g_total_callbacks     = 0;
static uint32_t g_set_callback_calls  = 0;

extern void doom_trace(const char* msg);

// ---- Time advance + callback firing ----------------------------------------

static void AdvanceTime(unsigned int nsamples) {
    mutexLock(&callback_queue_mutex);

    const uint64_t us = ((uint64_t)nsamples * OPL_SECOND)
                        / (uint64_t)OPL_LIBNX_SAMPLE_RATE;
    current_time += us;
    if (opl_paused) pause_offset += us;

    while (!OPL_Queue_IsEmpty(callback_queue)
           && current_time >= OPL_Queue_Peek(callback_queue) + pause_offset) {
        opl_callback_t cb;
        void*          data;
        if (!OPL_Queue_Pop(callback_queue, &cb, &data)) break;

        // Drop queue mutex BEFORE calling the callback; the callback may
        // call OPL_SetCallback (which grabs queue mutex) to reschedule.
        // Hold callback_mutex during the call so OPL_Lock-style critical
        // sections from other threads block until the callback returns.
        mutexUnlock(&callback_queue_mutex);
        mutexLock(&callback_mutex);
        cb(data);
        mutexUnlock(&callback_mutex);
        ++g_total_callbacks;
        mutexLock(&callback_queue_mutex);
    }

    mutexUnlock(&callback_queue_mutex);
}

// ---- Public render entry point ---------------------------------------------
//
// Fill `frames` stereo int16 frames into `dst`. Called from the audio submit
// thread once per buffer. Drives both OPL emulation and MIDI event playback.

void OPL_LIBNX_Render(int16_t* dst, size_t frames) {
    if (!opl_chip_inited) {
        memset(dst, 0, frames * 2 * sizeof(int16_t));
        return;
    }

    size_t filled = 0;
    while (filled < frames) {
        uint64_t until_next;

        mutexLock(&callback_queue_mutex);
        if (opl_paused || OPL_Queue_IsEmpty(callback_queue)) {
            until_next = (uint64_t)(frames - filled);
        } else {
            const uint64_t next_us =
                OPL_Queue_Peek(callback_queue) + pause_offset;
            uint64_t dt_us = (next_us > current_time) ? (next_us - current_time)
                                                      : 0;
            // Convert µs → samples: ceil(dt_us * rate / 1e6).
            until_next = (dt_us * (uint64_t)OPL_LIBNX_SAMPLE_RATE
                          + OPL_SECOND - 1) / OPL_SECOND;
            if (until_next > frames - filled) until_next = frames - filled;
            if (until_next == 0) until_next = 1;  // make forward progress
        }
        mutexUnlock(&callback_queue_mutex);

        // Render in chunks to fit our scratch buffer.
        size_t remaining = until_next;
        while (remaining > 0) {
            size_t chunk = remaining > MAX_CHUNK ? MAX_CHUNK : remaining;
            OPL3_GenerateStream(&opl_chip, scratch_chunk, (Bit32u)chunk);
            // Copy into dst at the right offset.
            memcpy(dst + (filled * 2), scratch_chunk, chunk * 2 * sizeof(int16_t));
            filled    += chunk;
            remaining -= chunk;
        }

        // Time advances for the whole `until_next`, not per chunk — keeps
        // callbacks firing at the right point even when we slice the render.
        AdvanceTime((unsigned int)until_next);
    }
}

// ---- opl_driver_t implementation -------------------------------------------

static int OPL_LIBNX_Init(unsigned int port_base) {
    (void)port_base;

    callback_queue = OPL_Queue_Create();
    if (!callback_queue) return 0;

    mutexInit(&callback_mutex);
    mutexInit(&callback_queue_mutex);

    OPL3_Reset(&opl_chip, OPL_LIBNX_SAMPLE_RATE);
    opl_chip_inited = 1;

    current_time = 0;
    pause_offset = 0;
    opl_paused   = 0;
    register_num = 0;

    return 1;
}

static void OPL_LIBNX_Shutdown(void) {
    if (callback_queue) {
        OPL_Queue_Destroy(callback_queue);
        callback_queue = NULL;
    }
    opl_chip_inited = 0;
}

static unsigned int OPL_LIBNX_PortRead(opl_port_t port) {
    (void)port;
    // Fake the OPL2/3 status register so OPL_Detect succeeds. The detection
    // sequence:
    //   1. Reset/enable interrupts (timer1_running stays 0) → status reads 0 ✓
    //   2. Start timer 1 with TIMER_CTRL bit 0x01 → timer1_running = 1
    //   3. Read status many times — return 0xc0 (interrupt + T1-expired) ✓
    // Doom only inspects two reads (before/after start), so a constant 0xc0
    // while the timer is "running" is sufficient. We don't need to model
    // real elapsed time — current_time only advances inside submit_thread's
    // AdvanceTime, while OPL_Delay (called between writes) is a real-time
    // sleep that doesn't tick our virtual OPL clock.
    return timer1_running ? 0xc0 : 0;
}

static void OPL_LIBNX_PortWrite(opl_port_t port, unsigned int value) {
    if (port == OPL_REGISTER_PORT) {
        register_num = (int)(value & 0xff);
    } else if (port == OPL_REGISTER_PORT_OPL3) {
        register_num = (int)((value & 0xff) | 0x100);
    } else if (port == OPL_DATA_PORT) {
        const Bit8u v = (Bit8u)(value & 0xff);
        const int   r = register_num;

        // Track timer-control writes so OPL_Detect's status-bit poll resolves.
        // (Music playback never starts the timers — i_oplmusic uses the
        // OPL_SetCallback mechanism instead — so this state matters only
        // during the init detection sequence.)
        if (r == OPL_REG_TIMER_CTRL) {
            // Doom's detection sequence (OPL_Detect):
            //   write 0x60 → reset both timers
            //   write 0x80 → enable interrupts (no start bits)
            //   write 0x21 → start timer 1 (bit 0)
            //   ... read status ...
            // We treat any control write WITHOUT bit 0x01 as resetting; bit
            // 0x01 starts T1.
            if (v & 0x01) {
                timer1_running = 1;
            } else {
                timer1_running = 0;
            }
        }

        OPL3_WriteRegBuffered(&opl_chip, (Bit16u)r, v);
        ++g_total_writes;
    }
}

static void OPL_LIBNX_SetCallback(uint64_t us,
                                  opl_callback_t callback,
                                  void* data) {
    mutexLock(&callback_queue_mutex);
    OPL_Queue_Push(callback_queue, callback, data, current_time + us);
    mutexUnlock(&callback_queue_mutex);
    ++g_set_callback_calls;
}

void OPL_LIBNX_DebugSnapshot(int* inited, uint32_t* writes,
                             uint32_t* callbacks, uint32_t* sched) {
    if (inited)    *inited    = opl_chip_inited;
    if (writes)    *writes    = g_total_writes;
    if (callbacks) *callbacks = g_total_callbacks;
    if (sched)     *sched     = g_set_callback_calls;
}

static void OPL_LIBNX_ClearCallbacks(void) {
    mutexLock(&callback_queue_mutex);
    OPL_Queue_Clear(callback_queue);
    mutexUnlock(&callback_queue_mutex);
}

static void OPL_LIBNX_Lock(void) {
    mutexLock(&callback_mutex);
}

static void OPL_LIBNX_Unlock(void) {
    mutexUnlock(&callback_mutex);
}

static void OPL_LIBNX_SetPaused(int paused) {
    opl_paused = paused;
}

static void OPL_LIBNX_AdjustCallbacks(float factor) {
    mutexLock(&callback_queue_mutex);
    OPL_Queue_AdjustCallbacks(callback_queue, current_time, factor);
    mutexUnlock(&callback_queue_mutex);
}

opl_driver_t opl_libnx_driver = {
    "libnx",
    OPL_LIBNX_Init,
    OPL_LIBNX_Shutdown,
    OPL_LIBNX_PortRead,
    OPL_LIBNX_PortWrite,
    OPL_LIBNX_SetCallback,
    OPL_LIBNX_ClearCallbacks,
    OPL_LIBNX_Lock,
    OPL_LIBNX_Unlock,
    OPL_LIBNX_SetPaused,
    OPL_LIBNX_AdjustCallbacks,
};
