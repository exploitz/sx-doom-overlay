// audio_lock.cpp — extern "C" wrapper around libtesla's audio mutex.
//
// audio_backend_libnx.c needs to lock ult::Audio::m_audioMutex around audout
// calls to prevent races with libtesla's own UI sound submissions. UltraGB
// follows the same pattern (see lib/UltraGB/source/gb_audio.h:1212). C can't
// directly use std::mutex / std::lock_guard, so we expose lock/unlock as
// C functions.
//
// Also exposes whether libtesla has already initialized audout — when it
// has, we DO NOT call audoutInitialize ourselves (refcount is already 1
// from libtesla; we just borrow). When it hasn't, we open audout ourselves
// and remember to close it.
//
// Licensed under GPLv2.

#include <atomic>
#include <mutex>

#include "../lib/libultrahand/libultra/include/audio.hpp"

extern "C" {

void audio_lock_acquire(void) {
    ult::Audio::m_audioMutex.lock();
}

void audio_lock_release(void) {
    ult::Audio::m_audioMutex.unlock();
}

bool audio_libtesla_initialized(void) {
    return ult::Audio::m_initialized;
}

}  // extern "C"
