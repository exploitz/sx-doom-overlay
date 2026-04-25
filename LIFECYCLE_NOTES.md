# LIFECYCLE_NOTES — libtesla overlay dismiss/resume behavior

Required by Plan v2 Task 6 pre-task verification. This file documents what
happens to a `tsl::Overlay` and its active `tsl::Gui` when the user
dismisses the overlay (Ultrahand close hotkey, B button at root) and then
re-summons it. Truth T7 ("pause-on-dismiss preserves state") depends on
the hide/show path NOT destroying the Gui object.

## Confirmed (read from `lib/libultrahand/libtesla/include/tesla.hpp` at
## the version pinned in our submodule, commit `6b1749f`):

### `tsl::Overlay` virtuals — what we override and what they default to

| Virtual | Line | Default | Our use |
|---|---|---|---|
| `initServices` | 10957 | empty | start audio thread (Task 9), `setjmp` checkpoint (Task 7) |
| `exitServices` | 10963 | empty | join audio thread, `audoutExit`, free Doom zone |
| `onShow` | 10969 | empty | re-anchor wall-clock, resume audio (Task 9) |
| `onHide` | 10975 | empty | pause audio, hold simulation tick |
| `loadInitialGui` | 10984 | pure virtual | return `std::make_unique<DoomGui>()` |

### Hide/show flow — what triggers each

- **`Overlay::hide()`** (line 11050) is invoked on the Ultrahand close hotkey
  combo and on `B` press at the root frame. It:
  1. Calls `this->onHide()` (line 11086) — our hook to pause audio etc.
  2. Sets the internal hidden flag.
  3. **Does NOT destroy the active Gui object.** The unique_ptr returned by
     `loadInitialGui()` stays alive in `Overlay::m_guiStack` (or equivalent
     internal storage).
  4. The libtesla main loop stops calling `Gui::update()` while hidden.

- **`Overlay::show()`** (line 10999) is invoked on the next Ultrahand summon. It:
  1. Calls `this->onShow()` (line 11024) — our hook to re-anchor clock.
  2. Clears the hidden flag.
  3. The Gui object is the same instance that was hidden — its members,
     including the Doom zone allocator base pointer and engine state, are
     intact in memory.
  4. The main loop resumes calling `Gui::update()`.

### What this confirms for T7

- **Pause-on-dismiss preserves state** holds: dismissing the overlay does
  NOT destroy the Gui, so the Doom engine state (zone allocator, mobj
  list, current-level lumps in PU_CACHE, player position, etc.) survives
  in memory until the next `update()` call resumes it.
- We do NOT need `AppletHookCookie` for sleep/wake — `onHide`/`onShow`
  fire on both user-dismiss AND HOS sleep/wake transitions.
- Engine memory leaks would matter only between `initServices` and
  `exitServices`, NOT between `onHide` and `onShow`.

### What this does NOT confirm

- Behavior on **process exit** (loader unload — happens when the user picks
  a different overlay from Ultrahand, or when nx-ovlloader respawns). On
  process exit, `exitServices()` IS called (line 13863) and the Gui IS
  destroyed. Engine state is lost. Recovery is via Doom's save slots.
- Behavior on **out-of-band kill** (sysmodule fault, fatalThrow from
  somewhere else) — undefined; assume worst case.

### References

- `lib/libultrahand/libtesla/include/tesla.hpp:10969` — `virtual void onShow()`
- `lib/libultrahand/libtesla/include/tesla.hpp:10975` — `virtual void onHide()`
- `lib/libultrahand/libtesla/include/tesla.hpp:10984` — `virtual std::unique_ptr<tsl::Gui> loadInitialGui()`
- `lib/libultrahand/libtesla/include/tesla.hpp:11024` — `this->onShow()` call site inside `Overlay::show()`
- `lib/libultrahand/libtesla/include/tesla.hpp:11086` — `this->onHide()` call site inside `Overlay::hide()`
- `lib/libultrahand/libtesla/include/tesla.hpp:13536` — `overlay->initServices()` call in `tsl::loop`
- `lib/libultrahand/libtesla/include/tesla.hpp:13863` — `overlay->exitServices()` call in `tsl::loop`

### Action items for Task 7+

- `DoomGui::initServices` runs `setjmp(g_doom_error_jmp)` once, captures
  the error path before calling `doomgeneric_Create`.
- `DoomGui::onHide` pauses audio (sets `g_audio_paused = true`); audio
  thread spins on the flag and skips submits while paused.
- `DoomGui::onShow` re-anchors `g_doom_tick_anchor_ns = armGetSystemTick`
  and clears `g_audio_paused`.
- `DoomGui::exitServices` calls `audoutExit`, joins the audio thread,
  frees zone memory, releases the WAD file handle.

UltraGB-Overlay validates this exact pattern (`main.cpp:2667-2855`).

## Status

This pre-task verification is complete. T7 may proceed; the
pause-on-dismiss assumption is confirmed by direct inspection of the
pinned libtesla source.
