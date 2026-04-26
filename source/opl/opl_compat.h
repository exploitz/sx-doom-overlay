// opl_compat.h — chocolate-doom shims for doomgeneric.
//
// The chocolate-doom files we vendor (i_oplmusic.c, midifile.c) reference
// symbols that don't exist in doomgeneric (older fork). Rather than fork
// doomgeneric for these few helpers, define them here and include from
// both source files at the top.
//
// Licensed under GPLv2.

#ifndef OPL_COMPAT_H
#define OPL_COMPAT_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// PACKED_STRUCT — doomgeneric defines PACKEDATTR but not the wrapper macro
// that chocolate-doom's doomtype.h provides.
#ifndef PACKED_STRUCT
#define PACKED_STRUCT(...)  struct __VA_ARGS__ __attribute__((packed))
#endif

// SDL byte-swap macros — used by midifile.c. We only build for ARM
// little-endian on Switch, so these are straight bswap.
#ifndef SDL_SwapBE16
#define SDL_SwapBE16(x)  ((uint16_t)(((uint16_t)(x) >> 8) | ((uint16_t)(x) << 8)))
#endif
#ifndef SDL_SwapBE32
#define SDL_SwapBE32(x)                                  \
    ((uint32_t)(                                          \
        (((uint32_t)(x) & 0x000000FFu) << 24) |           \
        (((uint32_t)(x) & 0x0000FF00u) <<  8) |           \
        (((uint32_t)(x) & 0x00FF0000u) >>  8) |           \
        (((uint32_t)(x) & 0xFF000000u) >> 24)))
#endif

// I_Realloc — chocolate-doom's safe realloc, originally abort()-on-fail.
// sx-doom-overlay: changed to return NULL on failure so callers can
// recover. abort() in our address-constrained overlay = full process
// death and a system freeze (observed during MIDI track loading on
// level transitions). Callers must check the return value.
static inline void* I_Realloc(void* ptr, size_t size) {
    return realloc(ptr, size);
}

// M_fopen / M_remove — chocolate-doom Windows wide-char wrappers. On Switch
// (and our doomgeneric build) plain fopen/remove are correct.
static inline FILE* M_fopen(const char* path, const char* mode) {
    return fopen(path, mode);
}
#ifndef M_REMOVE_DEFINED
#define M_REMOVE_DEFINED 1
static inline int M_remove_compat(const char* path) { return remove(path); }
// i_oplmusic.c defines its own static M_remove. midifile.c doesn't use it.
#endif

#endif  // OPL_COMPAT_H
