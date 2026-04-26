// opl_libnx.h — render entry point for our libnx OPL driver.
//
// audio_backend_libnx.c::submit_thread_main calls OPL_LIBNX_Render once per
// output buffer to generate `frames` of stereo int16 PCM. The driver
// internally fires queued MIDI callbacks (i_oplmusic event timing) at the
// right sample offsets while rendering.

#ifndef OPL_LIBNX_H
#define OPL_LIBNX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void OPL_LIBNX_Render(int16_t* dst, size_t frames);

// Diagnostic snapshot — non-blocking peek. Useful for the audio submit
// thread's heartbeat trace to confirm OPL is alive and ticking.
void OPL_LIBNX_DebugSnapshot(int* inited, uint32_t* writes,
                             uint32_t* callbacks, uint32_t* sched);

#ifdef __cplusplus
}
#endif

#endif  // OPL_LIBNX_H
