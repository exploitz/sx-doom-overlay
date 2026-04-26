// memio_malloc.c — drop-in replacement for doomgeneric's memio.c that
// uses plain malloc/free instead of Z_Malloc/Z_Free.
//
// Why: chocolate-doom's memio is wired into Doom's zone allocator. That
// works fine for the original engine-only use, but on Switch we use memio
// from i_oplmusic + midifile during in-memory MIDI loading. mus2mid grows
// its output buffer through several Z_Malloc/Z_Free cycles, fragmenting
// the zone — observed to crash libtesla's HID poll thread on level
// transitions when the zone got tight.
//
// Same API surface as memio.h. Linked instead of doomgeneric's memio.c
// (excluded in Makefile DG_EXCLUDE). mus2mid.c gets it transparently.
//
// Licensed under GPLv2.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "memio.h"

typedef enum { MODE_READ, MODE_WRITE } memfile_mode_t;

struct _MEMFILE {
    unsigned char* buf;
    size_t         buflen;
    size_t         alloced;
    unsigned int   position;
    memfile_mode_t mode;
};

MEMFILE* mem_fopen_read(void* buf, size_t buflen) {
    MEMFILE* file = (MEMFILE*)malloc(sizeof(MEMFILE));
    if (!file) return NULL;
    file->buf      = (unsigned char*)buf;
    file->buflen   = buflen;
    file->alloced  = 0;             // unused for READ
    file->position = 0;
    file->mode     = MODE_READ;
    return file;
}

size_t mem_fread(void* buf, size_t size, size_t nmemb, MEMFILE* stream) {
    if (stream->mode != MODE_READ) return (size_t)-1;

    size_t items = nmemb;
    if (items * size > stream->buflen - stream->position) {
        items = (stream->buflen - stream->position) / size;
    }
    memcpy(buf, stream->buf + stream->position, items * size);
    stream->position += items * size;
    return items;
}

MEMFILE* mem_fopen_write(void) {
    MEMFILE* file = (MEMFILE*)malloc(sizeof(MEMFILE));
    if (!file) return NULL;
    // sx-doom-overlay: chocolate-doom's original 1024-byte initial caused
    // 5-6 realloc-doublings to fit a typical Doom MIDI output (30-50 KB),
    // each leaving a fragmentation hole. Start at 64 KB — covers ~95% of
    // Doom songs without a single realloc, dramatically lowering peak
    // heap pressure during song-change.
    file->alloced  = 64 * 1024;
    file->buf      = (unsigned char*)malloc(file->alloced);
    if (!file->buf) { free(file); return NULL; }
    file->buflen   = 0;
    file->position = 0;
    file->mode     = MODE_WRITE;
    return file;
}

size_t mem_fwrite(const void* ptr, size_t size, size_t nmemb, MEMFILE* stream) {
    if (stream->mode != MODE_WRITE) return (size_t)-1;
    size_t bytes = size * nmemb;
    while (bytes > stream->alloced - stream->position) {
        size_t newsz = stream->alloced * 2;
        unsigned char* newbuf = (unsigned char*)realloc(stream->buf, newsz);
        if (!newbuf) return 0;
        stream->buf     = newbuf;
        stream->alloced = newsz;
    }
    memcpy(stream->buf + stream->position, ptr, bytes);
    stream->position += bytes;
    if (stream->position > stream->buflen) stream->buflen = stream->position;
    return nmemb;
}

void mem_get_buf(MEMFILE* stream, void** buf, size_t* buflen) {
    *buf    = stream->buf;
    *buflen = stream->buflen;
}

void mem_fclose(MEMFILE* stream) {
    if (!stream) return;
    if (stream->mode == MODE_WRITE) free(stream->buf);
    free(stream);
}

long mem_ftell(MEMFILE* stream) {
    return (long)stream->position;
}

int mem_fseek(MEMFILE* stream, signed long offset, mem_rel_t whence) {
    long newpos;
    switch (whence) {
        case MEM_SEEK_SET: newpos = offset;                          break;
        case MEM_SEEK_CUR: newpos = (long)stream->position + offset; break;
        case MEM_SEEK_END: newpos = (long)stream->buflen + offset;   break;
        default:           return -1;
    }
    if (newpos < 0 || newpos > (long)stream->buflen) return -1;
    stream->position = (unsigned int)newpos;
    return 0;
}
