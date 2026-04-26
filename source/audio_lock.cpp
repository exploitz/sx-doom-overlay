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
#include <chrono>
#include <mutex>

#include "../lib/libultrahand/libultra/include/audio.hpp"

extern "C" {

void audio_lock_acquire(void) {
    ult::Audio::m_audioMutex.lock();
}

void audio_lock_release(void) {
    ult::Audio::m_audioMutex.unlock();
}

// Try to acquire the libtesla audio mutex with a deadline. Returns true on
// success (caller MUST call audio_lock_release), false on timeout (caller
// MUST NOT release). Used during shutdown so we never hang when libtesla's
// backgroundSoundThread is mid-IPC — better to skip audout teardown than
// freeze the system. On Switch, std::mutex is backed by a libnx Mutex; the
// std::timed_mutex is heavier (extra fields) so we simulate try_lock_for
// with a small spin instead.
bool audio_lock_try_acquire_ms(unsigned timeout_ms) {
    using clock = std::chrono::steady_clock;
    const auto deadline = clock::now() + std::chrono::milliseconds(timeout_ms);
    do {
        if (ult::Audio::m_audioMutex.try_lock()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    } while (clock::now() < deadline);
    return false;
}

bool audio_libtesla_initialized(void) {
    return ult::Audio::m_initialized;
}

}  // extern "C"
