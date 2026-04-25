// audio_backend_wav.c — desktop WAV-file backend for sx-doom-overlay tests.
//
// Implements the audio_backend.h contract. Submitted PCM is appended to a
// .wav file. On shutdown, the WAV header is patched up with the final size.
//
// Format: RIFF/WAVE, 16-bit signed PCM, stereo, 22050 Hz.
//
// Licensed under GPLv2.

#include "../../source/audio_backend.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct audio_backend_s {
    FILE*    fp;
    long     header_pos;
    uint32_t bytes_written;
};

static void write_le32(FILE* f, uint32_t v) {
    uint8_t b[4] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
        (uint8_t)((v >> 16) & 0xFF),
        (uint8_t)((v >> 24) & 0xFF),
    };
    fwrite(b, 1, 4, f);
}

static void write_le16(FILE* f, uint16_t v) {
    uint8_t b[2] = {
        (uint8_t)(v & 0xFF),
        (uint8_t)((v >> 8) & 0xFF),
    };
    fwrite(b, 1, 2, f);
}

audio_backend_status_t audio_backend_init(const char* path_hint,
                                          audio_backend_t** out) {
    if (!out) return AUDIO_BACKEND_INIT_FAILED;
    *out = NULL;

    audio_backend_t* be = (audio_backend_t*)calloc(1, sizeof(*be));
    if (!be) return AUDIO_BACKEND_BUFFER_ALLOC_FAILED;

    be->fp = fopen(path_hint ? path_hint : "out.wav", "wb");
    if (!be->fp) {
        free(be);
        return AUDIO_BACKEND_INIT_FAILED;
    }

    // Write a placeholder WAV header. We patch up sizes in shutdown().
    fwrite("RIFF", 1, 4, be->fp);
    write_le32(be->fp, 0);             // file_size - 8 (patched later)
    fwrite("WAVE", 1, 4, be->fp);
    fwrite("fmt ", 1, 4, be->fp);
    write_le32(be->fp, 16);            // fmt chunk size
    write_le16(be->fp, 1);             // PCM format
    write_le16(be->fp, AUDIO_BACKEND_CHANNELS);
    write_le32(be->fp, AUDIO_BACKEND_SAMPLE_RATE);
    write_le32(be->fp, AUDIO_BACKEND_SAMPLE_RATE *
                       AUDIO_BACKEND_CHANNELS *
                       (AUDIO_BACKEND_BITS / 8));   // byte rate
    write_le16(be->fp, AUDIO_BACKEND_CHANNELS * (AUDIO_BACKEND_BITS / 8));  // block align
    write_le16(be->fp, AUDIO_BACKEND_BITS);
    fwrite("data", 1, 4, be->fp);
    be->header_pos = ftell(be->fp);
    write_le32(be->fp, 0);             // data chunk size (patched later)
    be->bytes_written = 0;

    *out = be;
    return AUDIO_BACKEND_OK;
}

bool audio_backend_submit(audio_backend_t* be, const int16_t* pcm, size_t frames) {
    if (!be || !be->fp || !pcm) return false;
    size_t bytes = frames * AUDIO_BACKEND_CHANNELS * (AUDIO_BACKEND_BITS / 8);
    size_t wrote = fwrite(pcm, 1, bytes, be->fp);
    if (wrote != bytes) return false;
    be->bytes_written += (uint32_t)bytes;
    return true;
}

void audio_backend_shutdown(audio_backend_t* be) {
    if (!be) return;
    if (be->fp) {
        // Patch up sizes.
        // RIFF size = file_size - 8 = (header_after_RIFF_size = 4 + 'WAVE'+'fmt '+16+'data'+4 = 36) + bytes_written
        // We wrote: 'RIFF'(4) + size(4) + 'WAVE'(4) + 'fmt '(4) + 16(4) + fmt(16) + 'data'(4) + size(4) = 44 bytes header
        // So RIFF size value = 44 - 8 + bytes_written = 36 + bytes_written
        fseek(be->fp, 4, SEEK_SET);
        write_le32(be->fp, 36 + be->bytes_written);
        // data chunk size
        fseek(be->fp, be->header_pos, SEEK_SET);
        write_le32(be->fp, be->bytes_written);
        fclose(be->fp);
    }
    free(be);
}
