//
// Copyright(C) 2005-2014 Simon Howard
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// DESCRIPTION:
//    Reading of MIDI files.
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "doomtype.h"
#include "i_swap.h"
#include "i_system.h"
#include "m_misc.h"

#include "z_zone.h"       // sx-doom-overlay: Doom zone for music arena
#include "memio.h"        // sx-doom-overlay: in-memory MIDI loading
#include "opl_compat.h"   // PACKED_STRUCT, SDL_Swap*, M_fopen, I_Realloc

#include "midifile.h"

// sx-doom-overlay: midifile.c was originally FILE*-based. We've converted all
// reads to memio so song change can be done entirely in heap (no SD card
// round-trip per song — eliminates a heap-fragmentation source that was
// crashing libtesla's HID poll thread on level transitions).
//
// MIDI_LoadFile(filename) reads the file into a temporary buffer then defers
// to MIDI_LoadBuffer. MIDI_LoadBuffer is the hot path used by I_OPL_RegisterSong.

// -----------------------------------------------------------------------------
// MIDI event arena (heap-allocated at music init)
// -----------------------------------------------------------------------------
//
// Per-track event arrays would otherwise need realloc growth up to 500+ KB
// of contiguous heap (E1M2's "On the Hunt" has ~12k MIDI events × 32 bytes
// per midi_event_t = ~400 KB single track). Doubling realloc to that size
// requires 768 KB peak (old + new buffer simultaneously), which never fits
// on our 6.8 MB pool after the engine zone (4 MB) and libtesla framebuffer
// (1.3 MB) are claimed.
//
// Solution: malloc one fixed arena at music init, when heap is fresh and
// least fragmented. Bump-allocate from it per song; reset on every new
// MIDI_LoadBuffer. If the init malloc fails, music is disabled for the
// session — consistent (no intermittent silent songs) rather than the
// previous heap-state-dependent failures.
//
// The 512 KB allocation comes out of newlib heap, so it counts against the
// pool just like any malloc. Net cost vs the previous BSS arena: same. Net
// vs the dynamic realloc approach: fragmentation eliminated, peak heap
// pressure during play substantially lower.

#define MIDI_ARENA_BYTES  (512 * 1024)
static uint8_t* g_midi_arena      = NULL;
static size_t   g_midi_arena_used = 0;

extern void doom_trace(const char* msg);

// Allocate the arena from the Doom ZONE (PU_STATIC), not newlib heap.
// The 4 MiB zone has its own allocator that can COMPACT PU_CACHE entries
// to make room for static allocations — strictly more reliable than
// newlib malloc, which can't move existing allocations. Called from
// I_OPL_InitMusic. Returns false only if the zone genuinely can't fit
// 512 KB (very rare; would need >3.5 MB of pinned static zone state).
boolean MIDI_InitArena(void)
{
    if (g_midi_arena != NULL) return true;  // already allocated
    g_midi_arena = (uint8_t*)Z_Malloc(MIDI_ARENA_BYTES, PU_STATIC, NULL);
    if (g_midi_arena == NULL)
    {
        doom_trace("MIDI_InitArena: Z_Malloc failed");
        return false;
    }
    g_midi_arena_used = 0;
    char dbg[80];
    snprintf(dbg, sizeof(dbg),
             "MIDI_InitArena: %d KB Z_Malloc'd at %p",
             MIDI_ARENA_BYTES / 1024, (void*)g_midi_arena);
    doom_trace(dbg);
    return true;
}

void MIDI_ShutdownArena(void)
{
    if (g_midi_arena != NULL)
    {
        Z_Free(g_midi_arena);
        g_midi_arena = NULL;
        g_midi_arena_used = 0;
    }
}

static void midi_arena_reset(void) { g_midi_arena_used = 0; }

static void* midi_arena_alloc(size_t bytes)
{
    if (g_midi_arena == NULL) return NULL;
    bytes = (bytes + 7u) & ~(size_t)7u;  // 8-byte align
    if (g_midi_arena_used + bytes > MIDI_ARENA_BYTES) return NULL;
    void* r = g_midi_arena + g_midi_arena_used;
    g_midi_arena_used += bytes;
    return r;
}

#define HEADER_CHUNK_ID "MThd"
#define TRACK_CHUNK_ID  "MTrk"
#define MAX_BUFFER_SIZE 0x10000

// haleyjd 09/09/10: packing required
#ifdef _MSC_VER
#pragma pack(push, 1)
#endif

typedef PACKED_STRUCT (
{
    byte chunk_id[4];
    unsigned int chunk_size;
}) chunk_header_t;

typedef PACKED_STRUCT (
{
    chunk_header_t chunk_header;
    unsigned short format_type;
    unsigned short num_tracks;
    unsigned short time_division;
}) midi_header_t;

// haleyjd 09/09/10: packing off.
#ifdef _MSC_VER
#pragma pack(pop)
#endif

typedef struct
{
    // Length in bytes:

    unsigned int data_len;

    // Events in this track:

    midi_event_t *events;
    int num_events;
} midi_track_t;

struct midi_track_iter_s
{
    midi_track_t *track;
    unsigned int position;
    unsigned int loop_point;
};

struct midi_file_s
{
    midi_header_t header;

    // All tracks in this file:
    midi_track_t *tracks;
    unsigned int num_tracks;

    // Data buffer used to store data read for SysEx or meta events:
    byte *buffer;
    unsigned int buffer_size;
};

// Check the header of a chunk:

static boolean CheckChunkHeader(chunk_header_t *chunk,
                                const char *expected_id)
{
    boolean result;
    
    result = (memcmp((char *) chunk->chunk_id, expected_id, 4) == 0);

    if (!result)
    {
        fprintf(stderr, "CheckChunkHeader: Expected '%s' chunk header, "
                        "got '%c%c%c%c'\n",
                        expected_id,
                        chunk->chunk_id[0], chunk->chunk_id[1],
                        chunk->chunk_id[2], chunk->chunk_id[3]);
    }

    return result;
}

// Read a single byte.  Returns false on error.
// sx-doom-overlay: memio has no fgetc — use mem_fread of one byte.

static boolean ReadByte(byte *result, MEMFILE *stream)
{
    byte b;
    if (mem_fread(&b, 1, 1, stream) != 1)
    {
        fprintf(stderr, "ReadByte: Unexpected end of file\n");
        return false;
    }
    else
    {
        *result = b;

        return true;
    }
}

// Read a variable-length value.

static boolean ReadVariableLength(unsigned int *result, MEMFILE *stream)
{
    int i;
    byte b = 0;

    *result = 0;

    for (i=0; i<4; ++i)
    {
        if (!ReadByte(&b, stream))
        {
            fprintf(stderr, "ReadVariableLength: Error while reading "
                            "variable-length value\n");
            return false;
        }

        // Insert the bottom seven bits from this byte.

        *result <<= 7;
        *result |= b & 0x7f;

        // If the top bit is not set, this is the end.

        if ((b & 0x80) == 0)
        {
            return true;
        }
    }

    fprintf(stderr, "ReadVariableLength: Variable-length value too "
                    "long: maximum of four bytes\n");
    return false;
}

// Read a byte sequence into the data buffer.

static void *ReadByteSequence(unsigned int num_bytes, MEMFILE *stream)
{
    unsigned int i;
    byte *result;

    // Allocate a buffer. Allocate one extra byte, as malloc(0) is
    // non-portable.

    result = malloc(num_bytes + 1);

    if (result == NULL)
    {
        fprintf(stderr, "ReadByteSequence: Failed to allocate buffer\n");
        return NULL;
    }

    // Read the data:

    for (i=0; i<num_bytes; ++i)
    {
        if (!ReadByte(&result[i], stream))
        {
            fprintf(stderr, "ReadByteSequence: Error while reading byte %u\n",
                            i);
            free(result);
            return NULL;
        }
    }

    return result;
}

// Read a MIDI channel event.
// two_param indicates that the event type takes two parameters
// (three byte) otherwise it is single parameter (two byte)

static boolean ReadChannelEvent(midi_event_t *event,
                                byte event_type, boolean two_param,
                                MEMFILE *stream)
{
    byte b = 0;

    // Set basics:

    event->event_type = event_type & 0xf0;
    event->data.channel.channel = event_type & 0x0f;

    // Read parameters:

    if (!ReadByte(&b, stream))
    {
        fprintf(stderr, "ReadChannelEvent: Error while reading channel "
                        "event parameters\n");
        return false;
    }

    event->data.channel.param1 = b;

    // Second parameter:

    if (two_param)
    {
        if (!ReadByte(&b, stream))
        {
            fprintf(stderr, "ReadChannelEvent: Error while reading channel "
                            "event parameters\n");
            return false;
        }

        event->data.channel.param2 = b;
    }

    return true;
}

// Read sysex event:

static boolean ReadSysExEvent(midi_event_t *event, int event_type,
                              MEMFILE *stream)
{
    event->event_type = event_type;

    if (!ReadVariableLength(&event->data.sysex.length, stream))
    {
        fprintf(stderr, "ReadSysExEvent: Failed to read length of "
                                        "SysEx block\n");
        return false;
    }

    // Read the byte sequence:

    event->data.sysex.data = ReadByteSequence(event->data.sysex.length, stream);

    if (event->data.sysex.data == NULL)
    {
        fprintf(stderr, "ReadSysExEvent: Failed while reading SysEx event\n");
        return false;
    }

    return true;
}

// Read meta event:

static boolean ReadMetaEvent(midi_event_t *event, MEMFILE *stream)
{
    byte b = 0;

    event->event_type = MIDI_EVENT_META;

    // Read meta event type:

    if (!ReadByte(&b, stream))
    {
        fprintf(stderr, "ReadMetaEvent: Failed to read meta event type\n");
        return false;
    }

    event->data.meta.type = b;

    // Read length of meta event data:

    if (!ReadVariableLength(&event->data.meta.length, stream))
    {
        fprintf(stderr, "ReadSysExEvent: Failed to read length of "
                                        "SysEx block\n");
        return false;
    }

    // Read the byte sequence:

    event->data.meta.data = ReadByteSequence(event->data.meta.length, stream);

    if (event->data.meta.data == NULL)
    {
        fprintf(stderr, "ReadSysExEvent: Failed while reading SysEx event\n");
        return false;
    }

    return true;
}

static boolean ReadEvent(midi_event_t *event, unsigned int *last_event_type,
                         MEMFILE *stream)
{
    byte event_type = 0;

    if (!ReadVariableLength(&event->delta_time, stream))
    {
        fprintf(stderr, "ReadEvent: Failed to read event timestamp\n");
        return false;
    }

    if (!ReadByte(&event_type, stream))
    {
        fprintf(stderr, "ReadEvent: Failed to read event type\n");
        return false;
    }

    // All event types have their top bit set.  Therefore, if 
    // the top bit is not set, it is because we are using the "same
    // as previous event type" shortcut to save a byte.  Skip back
    // a byte so that we read this byte again.

    if ((event_type & 0x80) == 0)
    {
        event_type = *last_event_type;

        if (mem_fseek(stream, -1, MEM_SEEK_CUR) < 0)
        {
            fprintf(stderr, "ReadEvent: Unable to seek in stream\n");
            return false;
        }
    }
    else
    {
        *last_event_type = event_type;
    }

    // Check event type:

    switch (event_type & 0xf0)
    {
        // Two parameter channel events:

        case MIDI_EVENT_NOTE_OFF:
        case MIDI_EVENT_NOTE_ON:
        case MIDI_EVENT_AFTERTOUCH:
        case MIDI_EVENT_CONTROLLER:
        case MIDI_EVENT_PITCH_BEND:
            return ReadChannelEvent(event, event_type, true, stream);

        // Single parameter channel events:

        case MIDI_EVENT_PROGRAM_CHANGE:
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            return ReadChannelEvent(event, event_type, false, stream);

        default:
            break;
    }

    // Specific value?

    switch (event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            return ReadSysExEvent(event, event_type, stream);

        case MIDI_EVENT_META:
            return ReadMetaEvent(event, stream);

        default:
            break;
    }

    fprintf(stderr, "ReadEvent: Unknown MIDI event type: 0x%x\n", event_type);
    return false;
}

// Free an event:

static void FreeEvent(midi_event_t *event)
{
    // Some event types have dynamically allocated buffers assigned
    // to them that must be freed.

    switch (event->event_type)
    {
        case MIDI_EVENT_SYSEX:
        case MIDI_EVENT_SYSEX_SPLIT:
            free(event->data.sysex.data);
            break;

        case MIDI_EVENT_META:
            free(event->data.meta.data);
            break;

        default:
            // Nothing to do.
            break;
    }
}

// Read and check the track chunk header

static boolean ReadTrackHeader(midi_track_t *track, MEMFILE *stream)
{
    size_t records_read;
    chunk_header_t chunk_header;

    records_read = mem_fread(&chunk_header, sizeof(chunk_header_t), 1, stream);

    if (records_read < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&chunk_header, TRACK_CHUNK_ID))
    {
        return false;
    }

    track->data_len = SDL_SwapBE32(chunk_header.chunk_size);

    return true;
}

static boolean ReadTrack(midi_track_t *track, MEMFILE *stream)
{
    // sx-doom-overlay: arena-allocated event array. Single bump alloc
    // sized off the chunk header's data_len byte count. Smallest valid
    // MIDI event is ~3 bytes (delta + status with running-status data),
    // so data_len/3 is a generous upper bound on event count. Allocate
    // that capacity once; if exceeded, fail (return false) and the
    // higher layer will fall back to silent music for this song.
    midi_event_t *event;
    unsigned int last_event_type;
    unsigned int capacity;

    track->num_events = 0;
    track->events     = NULL;

    if (!ReadTrackHeader(track, stream))
    {
        return false;
    }

    last_event_type = 0;

    // Single arena allocation — sized off track byte length. MUS-derived
    // MIDI events are ~3-4 bytes each (delta + status + 1-2 data bytes
    // with running status). data_len/3 is a generous upper bound that
    // still fits the 512 KB arena even for E1M2's heaviest track.
    capacity = (track->data_len / 3) + 16;
    track->events = (midi_event_t *)midi_arena_alloc(
        sizeof(midi_event_t) * capacity);
    if (track->events == NULL)
    {
        char dbg[96];
        snprintf(dbg, sizeof(dbg),
                 "ReadTrack: arena alloc fail (data_len=%u want=%u used=%zu/%d KB)",
                 track->data_len, capacity,
                 g_midi_arena_used / 1024, MIDI_ARENA_BYTES / 1024);
        doom_trace(dbg);
        return false;
    }

    for (;;)
    {
        if (track->num_events >= capacity)
        {
            char dbg[80];
            snprintf(dbg, sizeof(dbg),
                     "ReadTrack: capacity exhausted at %u events", capacity);
            doom_trace(dbg);
            return false;
        }

        event = &track->events[track->num_events];
        if (!ReadEvent(event, &last_event_type, stream))
        {
            return false;
        }
        ++track->num_events;

        if (event->event_type == MIDI_EVENT_META
            && event->data.meta.type == MIDI_META_END_OF_TRACK)
        {
            break;
        }
    }

    return true;
}

// Free a track. Per-event FreeEvent releases sysex/meta data buffers that
// ReadByteSequence malloc'd; then we free the events array itself.

static void FreeTrack(midi_track_t *track)
{
    unsigned int i;

    for (i=0; i<track->num_events; ++i)
    {
        FreeEvent(&track->events[i]);
    }
    // track->events lives in g_midi_arena (Z_Malloc'd block, bump-allocated
    // per song). Reset on next MIDI_LoadBuffer; no per-track free here.
    track->events = NULL;
}

static boolean ReadAllTracks(midi_file_t *file, MEMFILE *stream)
{
    unsigned int i;

    // Allocate list of tracks and read each track:

    file->tracks = malloc(sizeof(midi_track_t) * file->num_tracks);

    if (file->tracks == NULL)
    {
        return false;
    }

    memset(file->tracks, 0, sizeof(midi_track_t) * file->num_tracks);

    // Read each track:

    for (i=0; i<file->num_tracks; ++i)
    {
        if (!ReadTrack(&file->tracks[i], stream))
        {
            return false;
        }
    }

    return true;
}

// Read and check the header chunk.

static boolean ReadFileHeader(midi_file_t *file, MEMFILE *stream)
{
    size_t records_read;
    unsigned int format_type;

    records_read = mem_fread(&file->header, sizeof(midi_header_t), 1, stream);

    if (records_read < 1)
    {
        return false;
    }

    if (!CheckChunkHeader(&file->header.chunk_header, HEADER_CHUNK_ID)
     || SDL_SwapBE32(file->header.chunk_header.chunk_size) != 6)
    {
        fprintf(stderr, "ReadFileHeader: Invalid MIDI chunk header! "
                        "chunk_size=%i\n",
                        SDL_SwapBE32(file->header.chunk_header.chunk_size));
        return false;
    }

    format_type = SDL_SwapBE16(file->header.format_type);
    file->num_tracks = SDL_SwapBE16(file->header.num_tracks);

    if ((format_type != 0 && format_type != 1)
     || file->num_tracks < 1)
    {
        fprintf(stderr, "ReadFileHeader: Only type 0/1 "
                                         "MIDI files supported!\n");
        return false;
    }

    return true;
}

void MIDI_FreeFile(midi_file_t *file)
{
    int i;

    if (file->tracks != NULL)
    {
        for (i=0; i<file->num_tracks; ++i)
        {
            FreeTrack(&file->tracks[i]);
        }

        free(file->tracks);
    }

    free(file);
}

// sx-doom-overlay: in-memory MIDI loader. Both MIDI_LoadFile and the new
// MIDI_LoadBuffer route through the same parsing logic — the only difference
// is whether we slurped the file off SD first or were handed a pointer
// directly. Removes the SD round-trip from the engine's song-change hot
// path (was a fragmentation source on level transitions).
midi_file_t *MIDI_LoadBuffer(void *buf, size_t len)
{
    midi_file_t *file;
    MEMFILE *stream;

    midi_arena_reset();
    {
        char dbg[64];
        snprintf(dbg, sizeof(dbg),
                 "MIDI_LoadBuffer: enter len=%zu",
                 len);
        doom_trace(dbg);
    }

    file = malloc(sizeof(midi_file_t));
    if (file == NULL) return NULL;

    file->tracks      = NULL;
    file->num_tracks  = 0;
    file->buffer      = NULL;
    file->buffer_size = 0;

    stream = mem_fopen_read(buf, len);
    if (stream == NULL) { MIDI_FreeFile(file); return NULL; }

    if (!ReadFileHeader(file, stream))
    {
        mem_fclose(stream);
        MIDI_FreeFile(file);
        return NULL;
    }
    if (!ReadAllTracks(file, stream))
    {
        mem_fclose(stream);
        MIDI_FreeFile(file);
        return NULL;
    }
    mem_fclose(stream);
    return file;
}

midi_file_t *MIDI_LoadFile(char *filename)
{
    // Slurp the whole file into a temp buffer, defer to MIDI_LoadBuffer.
    // Engine's hot path uses MIDI_LoadBuffer directly; this is kept so
    // any standalone tool linking midifile.c still works.
    midi_file_t *file;
    FILE *fh;
    long filesize;
    void *buf;
    size_t got;

    fh = M_fopen(filename, "rb");
    if (fh == NULL)
    {
        fprintf(stderr, "MIDI_LoadFile: Failed to open '%s'\n", filename);
        return NULL;
    }

    fseek(fh, 0, SEEK_END);
    filesize = ftell(fh);
    fseek(fh, 0, SEEK_SET);
    if (filesize <= 0) { fclose(fh); return NULL; }

    buf = malloc(filesize);
    if (buf == NULL) { fclose(fh); return NULL; }
    got = fread(buf, 1, filesize, fh);
    fclose(fh);
    if (got != (size_t)filesize) { free(buf); return NULL; }

    file = MIDI_LoadBuffer(buf, (size_t)filesize);
    free(buf);
    return file;
}

// Get the number of tracks in a MIDI file.

unsigned int MIDI_NumTracks(midi_file_t *file)
{
    return file->num_tracks;
}

// Start iterating over the events in a track.

midi_track_iter_t *MIDI_IterateTrack(midi_file_t *file, unsigned int track)
{
    midi_track_iter_t *iter;

    assert(track < file->num_tracks);

    iter = malloc(sizeof(*iter));
    iter->track = &file->tracks[track];
    iter->position = 0;
    iter->loop_point = 0;

    return iter;
}

void MIDI_FreeIterator(midi_track_iter_t *iter)
{
    free(iter);
}

// Get the time until the next MIDI event in a track.

unsigned int MIDI_GetDeltaTime(midi_track_iter_t *iter)
{
    if (iter->position < iter->track->num_events)
    {
        midi_event_t *next_event;

        next_event = &iter->track->events[iter->position];

        return next_event->delta_time;
    }
    else
    {
        return 0;
    }
}

// Get a pointer to the next MIDI event.

int MIDI_GetNextEvent(midi_track_iter_t *iter, midi_event_t **event)
{
    if (iter->position < iter->track->num_events)
    {
        *event = &iter->track->events[iter->position];
        ++iter->position;

        return 1;
    }
    else
    {
        return 0;
    }
}

unsigned int MIDI_GetFileTimeDivision(midi_file_t *file)
{
    short result = SDL_SwapBE16(file->header.time_division);

    // Negative time division indicates SMPTE time and must be handled
    // differently.
    if (result < 0)
    {
        return (signed int)(-(result/256))
             * (signed int)(result & 0xFF);
    }
    else
    {
        return result;
    }
}

void MIDI_RestartIterator(midi_track_iter_t *iter)
{
    iter->position = 0;
    iter->loop_point = 0;
}

void MIDI_SetLoopPoint(midi_track_iter_t *iter)
{
    iter->loop_point = iter->position;
}

void MIDI_RestartAtLoopPoint(midi_track_iter_t *iter)
{
    iter->position = iter->loop_point;
}

#ifdef TEST

static char *MIDI_EventTypeToString(midi_event_type_t event_type)
{
    switch (event_type)
    {
        case MIDI_EVENT_NOTE_OFF:
            return "MIDI_EVENT_NOTE_OFF";
        case MIDI_EVENT_NOTE_ON:
            return "MIDI_EVENT_NOTE_ON";
        case MIDI_EVENT_AFTERTOUCH:
            return "MIDI_EVENT_AFTERTOUCH";
        case MIDI_EVENT_CONTROLLER:
            return "MIDI_EVENT_CONTROLLER";
        case MIDI_EVENT_PROGRAM_CHANGE:
            return "MIDI_EVENT_PROGRAM_CHANGE";
        case MIDI_EVENT_CHAN_AFTERTOUCH:
            return "MIDI_EVENT_CHAN_AFTERTOUCH";
        case MIDI_EVENT_PITCH_BEND:
            return "MIDI_EVENT_PITCH_BEND";
        case MIDI_EVENT_SYSEX:
            return "MIDI_EVENT_SYSEX";
        case MIDI_EVENT_SYSEX_SPLIT:
            return "MIDI_EVENT_SYSEX_SPLIT";
        case MIDI_EVENT_META:
            return "MIDI_EVENT_META";

        default:
            return "(unknown)";
    }
}

void PrintTrack(midi_track_t *track)
{
    midi_event_t *event;
    unsigned int i;

    for (i=0; i<track->num_events; ++i)
    {
        event = &track->events[i];

        if (event->delta_time > 0)
        {
            printf("Delay: %u ticks\n", event->delta_time);
        }

        printf("Event type: %s (%i)\n",
               MIDI_EventTypeToString(event->event_type),
               event->event_type);

        switch(event->event_type)
        {
            case MIDI_EVENT_NOTE_OFF:
            case MIDI_EVENT_NOTE_ON:
            case MIDI_EVENT_AFTERTOUCH:
            case MIDI_EVENT_CONTROLLER:
            case MIDI_EVENT_PROGRAM_CHANGE:
            case MIDI_EVENT_CHAN_AFTERTOUCH:
            case MIDI_EVENT_PITCH_BEND:
                printf("\tChannel: %u\n", event->data.channel.channel);
                printf("\tParameter 1: %u\n", event->data.channel.param1);
                printf("\tParameter 2: %u\n", event->data.channel.param2);
                break;

            case MIDI_EVENT_SYSEX:
            case MIDI_EVENT_SYSEX_SPLIT:
                printf("\tLength: %u\n", event->data.sysex.length);
                break;

            case MIDI_EVENT_META:
                printf("\tMeta type: %u\n", event->data.meta.type);
                printf("\tLength: %u\n", event->data.meta.length);
                break;
        }
    }
}

int main(int argc, char *argv[])
{
    midi_file_t *file;
    unsigned int i;

    if (argc < 2)
    {
        printf("Usage: %s <filename>\n", argv[0]);
        exit(1);
    }

    file = MIDI_LoadFile(argv[1]);

    if (file == NULL)
    {
        fprintf(stderr, "Failed to open %s\n", argv[1]);
        exit(1);
    }

    for (i=0; i<file->num_tracks; ++i)
    {
        printf("\n== Track %u ==\n\n", i);

        PrintTrack(&file->tracks[i]);
    }

    return 0;
}

#endif

