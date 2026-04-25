# UltraDoom Overlay — Adapted Implementation Plan

Created: 2026-04-25
Author: ereid129@gmail.com
Status: PENDING
Approved: No
Iterations: 0
Worktree: No
Type: Feature

---

## Context

Adapted from Chase's `sx-doom-overlay` plan (gober.chase@gmail.com). Same scope, same 12 tasks, same truths. Adapted for:
- **Engine**: chocolate-doom-nx-master (already present) instead of doomgeneric submodule
- **Threading**: D_DoomMain on dedicated thread (already in main.cpp) instead of single-threaded tick accumulator
- **Build env**: Windows + devkitPro at `C:\devkitPro`; build via devkitPro MSYS2 shell
- **Paths**: `sdmc:/roms/doom/doom.wad`, `sdmc:/config/ultradoom/`, output `ultradoom.ovl`
- **Project dir**: `D:\Users\ereid\Documents\Ethans-Claude-Projects\UltraDoom\UltraDoom-Overlay\`
- **WADs**: user already has `doom/doom.wad` + `doom/doom1.wad`; Freedoom still supported as free alternative
- **lib/libultrahand**: Windows junction (`mklink /J`), already exists — not a git submodule

---

## Summary

**Goal:** Doom running end-to-end as a Nintendo Switch overlay (`ultradoom.ovl`). Chocolate-doom engine + libtesla surface. Single-player playable with commercial DOOM.WAD or free Freedoom 1; audio via libnx audout; save/load to 8 slots; runtime render scale; BYO-WAD; heap-too-small error screen.

**Architecture:**
- `tsl::Overlay → tsl::Gui → tsl::elm::Element` (DoomElement draws, DoomGui handles input)
- `D_DoomMain()` runs on a dedicated libnx Thread (`s_doom_stack`, priority 0x2c)
- `I_FinishUpdate()` in `i_video_nx.c` calls `doom_submit_frame()` → copies 320×200 indexed buffer into `g_doom_shared_fb` under `g_doom_fb_mutex`
- `DoomElement::draw()` reads shared buffer → palette LUT → RGBA4444 → direct write to `renderer->getCurrentFramebuffer()`
- `I_SetPalette()` in `i_video_nx.c` calls `doom_set_palette()` → rebuilds `g_doom_palette_rgba4444[256]`
- `I_GetEvent()` in `i_input_nx.c` reads `g_hid_keys_held/down/up` atomics written by `DoomGui::handleInput()`
- Audio thread separate from engine thread (libnx audout submit-and-wait is blocking)

**Tech stack:** C99 (chocolate-doom engine), C++26 (overlay shim), devkitA64/libnx, libultrahand (libtesla + libultra), chocolate-doom-nx-master (vendored, already present).

---

## Key Differences from Chase's Plan

| Topic | Chase (doomgeneric) | Ethan (chocolate-doom-nx) |
|---|---|---|
| Engine | doomgeneric submodule + DG_* shims | chocolate-doom-nx-master (already present) |
| Threading | Single-threaded tick in update() | D_DoomMain on dedicated thread |
| Platform shim hook | 6 DG_* functions | I_video_nx.c / I_input_nx.c / I_sound_nx.c / I_timer_nx.c |
| Palette callback | extern palette_changed + colors[256] | doom_set_palette(rgb_pal*) called from I_SetPalette() |
| MIN_RAM patch | patches/0001-lower-min-ram.patch | Not needed (chocolate-doom memory model differs) |
| WAD bundling | Freedoom 1 bundled in release zip | User already has doom.wad + doom1.wad; Freedoom optional |
| Output name | sx-doom-overlay.ovl | ultradoom.ovl |
| Build env | WSL2 / Linux | Windows + devkitPro MSYS2 |
| lib/libultrahand | git submodule | Windows junction (already exists) |

---

## Current State (as of plan creation)

- **Task 1** (~80% done): project bootstrapped, Makefile functional, lib/libultrahand junction exists, `ultradoom.ovl` already built, source/{main.cpp, doom_globals.hpp, doom_input.hpp} present. Remaining: `.gitignore`, verify clean build.
- **Task 2** (~20% done): engine compiles for Switch; no desktop test suite yet.
- **Task 3** (~50% done): blit loop + palette callback implemented in main.cpp; not yet extracted to blit.cpp, no unit tests.
- **Task 4** (~30% done): `i_sound_nx.o` in build/ (source exists in chocolate-doom-nx-master/src/); audout integration unverified.
- **Task 5** (~90% done): `ultradoom.ovl` exists; size/symbol checks pending.
- **Tasks 6–12**: Not started on hardware.

---

## SD Card Paths

- `/switch/.overlays/ultradoom.ovl` — overlay binary
- `sdmc:/roms/doom/doom.wad` — primary IWAD (commercial)
- `sdmc:/roms/doom/doom1.wad` — alternate IWAD
- `sdmc:/roms/doom/freedoom1.wad` — free IWAD (BYO)
- `sdmc:/config/ultradoom/config.ini` — settings
- `sdmc:/config/ultradoom/savegames/doomsavN.dsg` — saves
- `sdmc:/config/ultradoom/perflog.txt` — frame timing log
- `sdmc:/config/ultradoom/error.log` — crash log

---

## Goal Verification Truths (unchanged from Chase's plan)

1. **T1** — Plays Doom end-to-end. Cold launch → Doom Overlay in Ultrahand → E1M1 → exit door. Input responsive throughout.
2. **T2** — Audio works or fails gracefully. Shotgun/monsters/music audible, OR audio-init-failed toast appeared once and game continues silent.
3. **T3** — Save/load round-trips. Slot 3 save reloads to ±1 tick (position, weapons, ammo, monsters).
4. **T4** — Settings persist. After changing scale/volume/WAD, values survive overlay close+reopen.
5. **T5** — Heap error screen guards entry. At 4 MB heap: error screen within 2 s, engine never inits, no crash report. At 8 MB: engine inits normally.
6. **T6** — BYO-WAD detected. `freedoom1.wad` at `sdmc:/roms/doom/` → appears in settings WAD dropdown.
7. **T7** — Pause-on-dismiss preserves state. Mid-level dismiss → re-summon → same tick, same position, projectiles in-flight.
8. **T8** — 35 Hz holds at scale=2. 60 s of E1M1 gameplay, no more than 5 consecutive frames > 30 ms delta in perflog.txt.

---

## Critical Files

| File | Role |
|---|---|
| `UltraDoom-Overlay/source/main.cpp` | DoomOverlay, DoomGui, DoomElement, tsl::loop entry |
| `UltraDoom-Overlay/source/doom_globals.hpp` | Shared state: framebuffer, palette LUT, atomics, paths |
| `UltraDoom-Overlay/source/doom_input.hpp` | HID → doom_key mapping table |
| `UltraDoom-Overlay/source/blit.cpp` (to create) | Extracted blit + palette-LUT logic |
| `UltraDoom-Overlay/source/i_video_nx.c` (in choc-doom-nx) | doom_submit_frame + doom_set_palette calls |
| `UltraDoom-Overlay/source/i_input_nx.c` (in choc-doom-nx) | I_GetEvent reads g_hid_keys_* atomics |
| `UltraDoom-Overlay/source/i_sound_nx.c` (in choc-doom-nx) | libnx audout backend |
| `UltraDoom-Overlay/source/i_timer_nx.c` (in choc-doom-nx) | armGetSystemTick clock |
| `UltraDoom-Overlay/source/settings_gui.hpp` (to create) | tsl::Gui for scale/volume/WAD/vibration |
| `UltraDoom-Overlay/source/error_heap_gui.hpp` (to create) | Heap-too-small error screen |
| `UltraDoom-Overlay/source/atomic_save.c` (to create) | fwrite → tmp → fsync → rename save wrapper |
| `UltraDoom-Overlay/Makefile` | Build; includes lib/libultrahand/ultrahand.mk |
| `chocolate-doom-nx-master/src/i_video_nx.c` | Upstream nx platform shim (video + palette) |
| `chocolate-doom-nx-master/src/i_input_nx.c` | Upstream nx platform shim (input) |
| `chocolate-doom-nx-master/src/i_sound_nx.c` | Upstream nx platform shim (audio) |
| `libultrahand/libtesla/include/tesla.hpp` | libtesla API; Renderer::getCurrentFramebuffer() |

---

## Build Commands (Windows devkitPro MSYS2)

Launch via: `C:\devkitPro\msys2\msys2.exe`

```bash
# Inside MSYS2 shell (devkitPro environment already configured):
export DEVKITPRO=/opt/devkitpro
cd /d/Users/ereid/Documents/Ethans-Claude-Projects/UltraDoom/UltraDoom-Overlay
make clean && make
```

Desktop tests (chocolate-doom CMake, native MSYS2 + SDL2):
```bash
cd /d/Users/ereid/Documents/Ethans-Claude-Projects/UltraDoom/chocolate-doom
mkdir build-desktop && cd build-desktop
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
make chocolate-doom -j4
./chocolate-doom -iwad /d/Users/ereid/Documents/Ethans-Claude-Projects/UltraDoom/doom/doom.wad
```

---

## Progress Tracking

- [ ] Task 1: Project bootstrap (clean state verification)
- [ ] Task 2: Desktop engine smoke (chocolate-doom CMake native build)
- [ ] Task 3: Extract blit module to blit.cpp + unit tests
- [ ] Task 4: Verify i_sound_nx.c audout integration + desktop audio tests
- [ ] Task 5: Cross-build verification (clean build, size/symbol checks)
- [ ] Task 6: Overlay skeleton on hardware (test pattern + lifecycle confirmation)
- [ ] Task 7: Engine integration on hardware (Doom runs in overlay, no input yet)
- [ ] Task 8: Input mapping verified on hardware (E1M1 completable)
- [ ] Task 9: Audio integration on hardware (sfx + music; coexistence fallback)
- [ ] Task 10: Heap-too-small error screen + first-launch self-check
- [ ] Task 11: Settings menu + INI persistence + BYO-WAD detection
- [ ] Task 12: Save/load wiring + release packaging

**Total Tasks:** 12 | **Completed:** 0 | **Remaining:** 12

---

## Implementation Tasks

### Task 1: Project bootstrap — clean state verification

**Objective:** Verify the project is in a clean, reproducible state. Add missing scaffolding. Confirm `make clean && make` produces `ultradoom.ovl` from scratch.
**Dependencies:** None
**Mapped Truths:** None (foundation)

**Files:**
- Create: `UltraDoom-Overlay/.gitignore` (cover `build/`, `*.ovl`, `*.elf`, `*.nacp`, `*.nro`, `*.o`, `*.d`, `*.map`, `error.log`)
- Verify: `lib/libultrahand` junction points to `../../libultrahand` (or `..\..\libultrahand` on Windows)
- Verify: `Makefile` has correct `DOOM_SRC`, `SDL_EXCLUDE`, and all required `CFLAGS`

**Key Decisions:**
- `lib/libultrahand` is a Windows junction (`mklink /J`), not a git submodule. The Makefile comment documents the `mklink /J` command for fresh checkouts. Do NOT add it to `.gitmodules`.
- WAD path stays `sdmc:/roms/doom/doom.wad` (user has commercial WADs in `doom/`). Freedoom will be an additional supported path in Task 11.
- No `patches/` directory or patch sentinel needed — chocolate-doom-nx-master is used as-is (no MIN_RAM patch required).

**Definition of Done:**
- [ ] `make clean && make` (in devkitPro MSYS2) produces `ultradoom.ovl` without errors
- [ ] `.gitignore` covers all build artifacts; `git status` (once initialized) shows only source files
- [ ] `lib/libultrahand` junction resolves: `ls lib/libultrahand/ultrahand.mk` succeeds
- [ ] `file ultradoom.ovl` reports ARM64 NRO with ULTR suffix (Ultrahand signature)
- [ ] `ultradoom.ovl` > 1 MB (has engine code linked in)

**Verify:**
```bash
make clean && make 2>&1 | tail -5
file ultradoom.ovl
wc -c ultradoom.ovl
```

---

### Task 2: Desktop engine smoke (chocolate-doom CMake)

**Objective:** Build chocolate-doom natively (MSYS2 + SDL2) and confirm it boots, renders frames, and accepts input with the user's `doom.wad`. Validates engine health before integrating into the overlay.
**Dependencies:** Task 1
**Mapped Truths:** None (engine smoke; supports T1)

**Files:**
- No overlay source files modified — this is a desktop validation step using the upstream `chocolate-doom-nx-master/CMakeLists.txt` or the plain `chocolate-doom/CMakeLists.txt`.

**Key Decisions:**
- Use `chocolate-doom/` (the cross-platform build, not the nx port) for desktop smoke — it has SDL2 and runs on Windows/MSYS2.
- IWAD: `doom/doom.wad` (already present, 10.6 MB — full Doom 1).
- Smoke test: launch, watch title screen + attract demo for ~10 seconds. Visual confirmation: red DOOM logo, E1M1 demo loop.
- No PPM frame dumping needed (unlike Chase's doomgeneric plan). SDL window is sufficient.
- If SDL2 not in MSYS2, install via `pacman -S mingw-w64-x86_64-SDL2` inside devkitPro MSYS2.

**Definition of Done:**
- [ ] `cmake .. && make chocolate-doom` succeeds without errors in devkitPro MSYS2
- [ ] `./chocolate-doom -iwad <path>/doom/doom.wad` shows title screen and attract demo for ≥10 seconds without crash
- [ ] Memory: no `malloc` failures or `I_Error` calls during desktop run
- [ ] Engine handles save directory init gracefully (will use default `~/.local/share/chocolate-doom/`)

**Verify:**
```bash
cd chocolate-doom && mkdir -p build-desktop && cd build-desktop
cmake .. -G "Unix Makefiles" -DCMAKE_BUILD_TYPE=Debug
make chocolate-doom -j4
./chocolate-doom -iwad /d/.../doom/doom.wad
```

---

### Task 3: Extract blit module + unit tests

**Objective:** Extract the inline blit + palette logic from `main.cpp` into `source/blit.cpp` / `source/blit.hpp`. Add desktop unit tests confirming correct palette conversion and integer-scale blit at 1×/2×/3×.
**Dependencies:** Task 2
**Mapped Truths:** Supports T1

**Files:**
- Create: `source/blit.hpp` — `void blit_doom_frame(const uint8_t* src_indexed, const uint16_t* lut, uint16_t* dst_fb, int dst_stride, int vp_x, int vp_y, int vp_w, int vp_h)`
- Create: `source/blit.cpp` — implementation (integer nearest-neighbor scale; inner loop reads indexed byte → LUT → write RGBA4444)
- Modify: `source/main.cpp` — replace inline blit in `DoomElement::draw()` with call to `blit_doom_frame()`; keep `doom_set_palette()` in main.cpp (it writes `g_doom_palette_rgba4444` which blit reads)
- Create: `tests/desktop/test_blit.cpp` — hand-rolled assertion harness; tests: palette 0 round-trip at scale 1/2/3, palette-switch correctness

**Key Decisions:**
- `blit_doom_frame` takes explicit `vp_w/vp_h` so the same code works for scale=1 (320×200 viewport), scale=2 (640×400), scale=3 (960×600) — vp dimensions computed from scale in the caller.
- The existing blit in `DoomElement::draw()` uses `VP_W = 448, VP_H = 336` (aspect-corrected) — keep this aspect-correct mapping as the default. Task 11's settings menu will let user switch between aspect-correct and square-pixel modes.
- Palette callback (`doom_set_palette`) stays in main.cpp because it's called from C linkage. `g_doom_palette_rgba4444` is the shared LUT.
- Desktop test: allocate a 320×200 uint8_t buffer with known index pattern, build a simple LUT (index → index * 0x1111 in RGBA4444), call blit, verify output dimensions and a sample of pixel values.

**Definition of Done:**
- [ ] `blit_doom_frame` compiles cleanly as part of the main build
- [ ] `main.cpp` `DoomElement::draw()` calls `blit_doom_frame()` — no inline blit loop remaining
- [ ] Desktop test builds standalone: `g++ -std=c++17 tests/desktop/test_blit.cpp source/blit.cpp -o test_blit && ./test_blit`
- [ ] Tests: scale 1 output is 320×200, scale 2 is 640×400, scale 3 is 960×600 (pixel counts verified)
- [ ] Tests: palette substitution verified — different LUT → different output bytes
- [ ] No sanitizer errors: `-fsanitize=address,undefined ./test_blit`

**Verify:**
```bash
g++ -std=c++17 -fsanitize=address,undefined tests/desktop/test_blit.cpp source/blit.cpp -o test_blit && ./test_blit
```

---

### Task 4: Verify audio integration (i_sound_nx.c + audout)

**Objective:** Inspect and complete `chocolate-doom-nx-master/src/i_sound_nx.c`. Confirm it implements chocolate-doom's `I_Sound` interface against libnx audout. Add a desktop stub test. Wire up the dedicated audio thread.
**Dependencies:** Task 2
**Mapped Truths:** Supports T2

**Files:**
- Inspect: `chocolate-doom-nx-master/src/i_sound_nx.c` — verify all required `I_Sound_*` / `I_Music_*` functions are present
- Modify if needed: `i_sound_nx.c` — add `g_audio_failed` atomic, submit-time error handling, silent fallback path
- Create: `source/audio_ring.h` / `source/audio_ring.c` — lock-free single-producer/single-consumer ring (4 × 1024-frame chunks of int16 stereo at 22050 Hz)
- Modify: `source/main.cpp` — `Overlay::initServices()` starts audio thread; `exitServices()` joins it; `onShow()/onHide()` reinit audout on resume

**Key Decisions / Notes:**
- chocolate-doom's `I_Sound` interface is in `src/i_sound.h`. The nx port implements it in `i_sound_nx.c` (already compiled into build). Read the existing source before modifying.
- Audio thread: libnx `Thread` at priority 0x2c, 4 KB stack. Drains ring buffer via `audoutAppendAudioOutBuffer` + `audoutWaitPlayFinish` (blocking). Separate from engine thread.
- `g_audio_failed` atomic: set when `audoutInitialize` OR `audoutAppendAudioOutBuffer` fails. On set: drain ring, exit thread, show one-time `tsl::notification` toast. Engine continues at 35 Hz unaffected.
- Sleep/wake: register `AppletHookCookie`; on `AppletMessage_Resume`, drop ring and reinit `audoutStartAudioOut`. If reinit fails → `g_audio_failed` path.
- Sample rate: 22050 Hz 16-bit stereo. Confirm audout sample rate negotiation during implementation (may need to upsample to 48 kHz).
- Desktop stub test: implement a WAV-file audio backend that writes `tests/desktop/out/audio_capture.wav`. Not wired to hardware; just validates mixer math.

**Definition of Done:**
- [ ] All `I_Sound_*` and `I_Music_*` functions exist in `i_sound_nx.c` (grep confirms no `__attribute__((unused))` stubs returning 0 without body)
- [ ] `g_audio_failed` path exists and compiles
- [ ] Desktop WAV test: `make -C tests/desktop test_sound` runs without error; produces a non-empty WAV
- [ ] `audoutExit()` called on `exitServices()` — no leaked service handles
- [ ] T2 fallback path covered: when `audoutInitialize` returns non-zero, toast fires once, game continues

**Verify:**
- Read `i_sound_nx.c` fully before modifying. Check function signatures against `src/i_sound.h`.
- Desktop: `gcc -std=c11 tests/desktop/audio_backend_wav.c tests/desktop/test_sound.c -o test_sound && ./test_sound`

---

### Task 5: Cross-build smoke (clean build + verification)

**Objective:** `make clean && make` produces a correct, fully-linked `ultradoom.ovl`. Verify size, symbols, and no undefined references.
**Dependencies:** Tasks 3, 4
**Mapped Truths:** None (build gate)

**Files:**
- Modify: `Makefile` — add any new sources from Tasks 3 + 4 to `SOURCES`; confirm `SDL_EXCLUDE` still covers all SDL files that conflict with nx builds
- Create: `scripts/check-ovl-size.sh` — warns if `.ovl` outside 1–4 MB range

**Key Decisions:**
- `DOOM_ENGINE_LINKED` flag already in Makefile CFLAGS — this enables the extern C linkage block in main.cpp and the real D_DoomMain() thread path.
- Expected `.ovl` size: 1–4 MB (chocolate-doom is larger than doomgeneric; libultrahand adds overhead).
- `-Wl,--gc-sections` + LTO should eliminate unreachable SDL stubs.

**Definition of Done:**
- [ ] `make clean && make 2>&1 | grep -E "^(error:|warning:)" | wc -l` = 0 (no errors or warnings; `-Werror` catches regressions)
- [ ] `nm ultradoom.elf | grep -c "D_DoomMain"` ≥ 1 (engine entry point linked)
- [ ] `nm ultradoom.elf | grep -c "doom_set_palette"` ≥ 1 (palette callback present)
- [ ] `nm ultradoom.elf | grep -c "doom_submit_frame"` ≥ 1 (frame submit callback present)
- [ ] No undefined symbol linker warnings
- [ ] `scripts/check-ovl-size.sh ultradoom.ovl` reports OK
- [ ] `nm ultradoom.elf | grep -E "SDL_|__cxa_throw|_ZTI" | wc -l` = 0 (no SDL leakage, no RTTI)

**Verify:**
```bash
make clean && make 2>&1 | tee build.log
grep -E "(error|warning):" build.log
nm ultradoom.elf | grep "D_DoomMain"
bash scripts/check-ovl-size.sh ultradoom.ovl
```

---

### Task 6: Overlay skeleton on hardware (test pattern + lifecycle)

**Objective:** Deploy to real Switch. Render color-bar test pattern at scales 1×/2×/3×. Confirm framebuffer dimensions, dismiss/resume lifecycle, and per-frame timing.
**Dependencies:** Task 5
**Mapped Truths:** Pre-condition for T7 (lifecycle verification)

**Pre-task verification (~30 min, no code):**
Read `libultrahand/libtesla/include/tesla.hpp` around `tsl::loop`, `mainLoop`, the dismiss/show paths. Confirm:
1. `tsl::Overlay::onHide()` default is a no-op (does not destroy active Gui).
2. Active Gui object is NOT destroyed on overlay dismiss (only on process exit).
3. `Gui::update()` simply stops being called while hidden; resumes on re-summon with instance state intact.

Document confirmed behavior in `LIFECYCLE_NOTES.md` at project root (one paragraph + tesla.hpp line numbers). If behavior differs, flag deviation and revise T7 + Task 7 pause-on-dismiss design.

**Files:**
- Create: `LIFECYCLE_NOTES.md` — confirmed lifecycle behavior
- Modify: `source/main.cpp` — replace stub rainbow pattern with explicit color-bar test (no engine thread started); hardcode scale=2 for this task; log per-frame draw time to `sdmc:/config/ultradoom/perflog.txt`
- Create: `source/test_pattern.cpp` — color bars + frame counter renderer

**Key Decisions:**
- `DOOM_ENGINE_LINKED` should be temporarily disabled (`#undef` or comment flag) for this task so the test pattern runs without the engine thread. Re-enable for Task 7.
- Use `renderer->getCurrentFramebuffer()` (fast path) for pixel writes — same path the real blit will use. NOT `setPixelAtOffset`.
- Frame timer: `armGetSystemTick()` before and after draw; log delta in µs to perflog.txt.
- Target: draw cost < 16 ms at scale=2 (640×400 fill). If over budget, investigate before Task 7.

**Definition of Done:**
- [ ] `.ovl` deploys, summons cleanly from Ultrahand
- [ ] Test pattern displays at scale=1 (320×200), scale=2 (640×400), scale=3 (960×600) — no artifacts
- [ ] B button exits cleanly to Ultrahand
- [ ] Per-frame draw time < 16 ms at scale=2 (from perflog.txt)
- [ ] Dismiss overlay → re-summon → test pattern resumes at same frame counter (state preserved)
- [ ] `LIFECYCLE_NOTES.md` written with tesla.hpp line references

**Verify:**
- Manual hardware test. Capture screenshot at each scale. Attach perflog.txt excerpt.
- `cat sdmc:/config/ultradoom/perflog.txt | awk '{print $2}' | sort -n | tail -5`

---

### Task 7: Engine integration on hardware

**Objective:** Wire `D_DoomMain` into the overlay. Doom boots, runs attract demo from `doom.wad`, renders gameplay at 35 Hz. No input control yet.
**Dependencies:** Task 6
**Mapped Truths:** T1 (initial path), T8 (35 Hz hold)

**Files:**
- Modify: `source/main.cpp` — re-enable `DOOM_ENGINE_LINKED`; `Overlay::initServices()` starts `g_doom_thread` running `D_DoomMain`; remove test-pattern call
- Verify/Modify: `chocolate-doom-nx-master/src/i_video_nx.c` — `I_FinishUpdate()` calls `doom_submit_frame(I_VideoBuffer)`; `I_SetPalette()` calls `doom_set_palette(pal_rgb)`; `I_InitGraphics()` calls `doom_notify_ready()`
- Verify/Modify: `chocolate-doom-nx-master/src/i_timer_nx.c` — `I_GetTime()` returns `armGetSystemTick() / 19200` (19.2 MHz Tegra X1 timer → ms)

**Key Decisions / Notes:**
- `myargc/myargv` already set up in `Overlay::initServices()` as `{"ultradoom", "-iwad", "sdmc:/roms/doom/doom.wad", "-mb", "4", nullptr}`. Keep `-mb 4` (4 MB zone alloc is appropriate for chocolate-doom).
- `DoomElement::draw()` calls `blit_doom_frame()` from Task 3. When `g_doom_initialized` is false, shows "Loading Doom..." text.
- 35 Hz hold via the engine's own `D_DoomLoop` / `I_GetTime()` — chocolate-doom handles its own tick rate. Our job is to not stall the libtesla composite. The engine thread runs independently.
- Framebuffer race: `doom_submit_frame` holds `g_doom_fb_mutex` during memcpy; `DoomElement::draw()` holds it during blit. Both are brief (≤1 ms). Acceptable.
- Log heap usage after `D_DoomMain` init via `mallinfo()` equivalent; target < 7.5 MB.

**Definition of Done:**
- [ ] Freedoom title screen (or DOOM title screen from doom.wad) appears on Switch hardware within 5 s of summon
- [ ] Attract demo plays for ≥30 s without crash
- [ ] Frame timing: 35 Hz simulation stable to ±1 tick over 1000 ticks at scale=2
- [ ] Peak heap < 7.5 MB (logged)
- [ ] No `Z_Malloc` or `I_Error` failures during 5 min attract demo
- [ ] `error.log` empty after successful run

**Verify:**
- Manual hardware test. Phone video of attract demo as artifact.
- Attach perflog.txt and heaplog from 5 min soak.

---

### Task 8: Input mapping verified on hardware

**Objective:** Player controls Doom. D-pad/sticks/buttons map to Doom keys. Freedoom / DOOM E1M1 completable to exit.
**Dependencies:** Task 7
**Mapped Truths:** T1

**Files:**
- Verify/Modify: `source/doom_input.hpp` — confirm mapping table matches plan (already mostly in place)
- Verify/Modify: `chocolate-doom-nx-master/src/i_input_nx.c` — `I_GetEvent()` reads `g_hid_keys_held/down/up` atomics, synthesizes `ev_keydown/ev_keyup` events for `D_ProcessEvents()`
- Modify: `source/main.cpp` — `DoomGui::handleInput()` writes key atomics; right-stick → turn (threshold 10000); stick deadzone handling

**Default mapping (from doom_input.hpp — confirm on hardware):**
- Left stick up/down/left/right → forward/back/turn-left/turn-right
- A → fire (RCTRL)
- ZR → fire (RCTRL)
- B → use/open (SPACE)
- Y → run (RSHIFT)
- L / ZL → strafe left (COMMA)
- R → strafe right (PERIOD)
- Plus → Doom menu (ESCAPE)
- Minus → Enter / confirm (will become settings in Task 11)
- X → automap (TAB)

**Key Decisions:**
- Both press AND release events must be queued. `I_GetEvent` synthesizes `ev_keydown` on new bits in `keys_down`, `ev_keyup` on bits in `keys_up` (= `prev_held & ~held`). This is already computed in `handleInput()`.
- Stick deadzone: threshold 10000 (~30% of 32767). Already in `handleInput()`.

**Definition of Done:**
- [ ] Player walks, strafes, turns, fires, opens doors
- [ ] Releasing A stops chaingun (key-up registered correctly)
- [ ] Plus → Doom in-game menu opens
- [ ] Stick deadzone comfortable; no stuck movement on release
- [ ] E1M1 completable to exit door (kills all enemies, reaches exit)

**Verify:**
- Manual hardware test. Verifier completes E1M1, records completion time and any control issues.

---

### Task 9: Audio integration on hardware

**Objective:** sfx + music via libnx audout. Graceful fallback at init AND submit time.
**Dependencies:** Task 8
**Mapped Truths:** T2

> **Threading:** Engine thread (D_DoomMain) calls `I_Sound_*` / `I_Music_*`. Those enqueue PCM into the ring. Dedicated audio thread drains ring via `audoutAppendAudioOutBuffer` + `audoutWaitPlayFinish` (blocking — must NOT be on engine thread).

**Files:**
- Complete: `chocolate-doom-nx-master/src/i_sound_nx.c` — libnx audout backend; `g_audio_failed` atomic; both failure paths
- Complete: `source/audio_ring.c` — ring buffer (from Task 4 stub)
- Modify: `source/main.cpp` — start audio thread in `initServices()`; join in `exitServices()`; `AppletHookCookie` for sleep/wake resume

**Key Decisions:**
- Same ring/thread architecture as described in Task 4.
- On `AppletMessage_Resume`: drop ring, call `audoutStartAudioOut`. Failure → `g_audio_failed` path.
- One-time toast text: "Audio unavailable — game continues silent" (same for init and submit failures).
- Coexistence test: with a game running in background (e.g., holding a cartridge game paused), summon overlay. Expected: either audio plays alongside OR init fails gracefully. Document which case occurs.

**Definition of Done:**
- [ ] Pistol shot, monster grunts, level music audible in normal gameplay (or silent-fallback toast)
- [ ] Volume settings (Task 11 slider) affect audio immediately
- [ ] Sleep/wake: audio resumes cleanly, no buffer artifacts
- [ ] Init-time failure (forced): toast appears once, gameplay continues at 35 Hz
- [ ] Submit-time failure (forced): `g_audio_failed` set, thread exits, toast, engine unaffected
- [ ] `audoutExit()` called on overlay exit
- [ ] T8 perflog unchanged with audio silent-fallback vs audio on

**Verify:**
- Manual hardware test over foreground game and Ultrahand-only (no game).

---

### Task 10: Heap-too-small error screen

**Objective:** Self-check heap at overlay init. Below 8 MB → show `ErrorHeapGui` with remediation. Never auto-write `heap_size.bin`.
**Dependencies:** Task 7 (engine init path to gate)
**Mapped Truths:** T5

**Files:**
- Create: `source/error_heap_gui.hpp` — `tsl::Gui` subclass rendering error screen
- Modify: `source/main.cpp` — `Overlay::initServices()` reads heap size via `svcGetInfo(InfoType_HeapRegionSize, ...)` BEFORE starting engine thread; if < 8 MB, instantiate `ErrorHeapGui` instead of `DoomGui`

**Error screen content:**
- Title: "Overlay Heap Too Small"
- Body: "Open Ultrahand → Settings → Overlay Heap Size → set to 8 MB → close & reopen"
- Sub-text: "What this means: Switch overlay memory pool too small to run Doom engine."
- B button exits to Ultrahand

**Key Decisions:**
- Threshold: `< 8 * 1024 * 1024` → show error. Exactly 8 MB → proceed.
- **Never write `heap_size.bin`** — overshoot triggers `fatalThrow` on some HOS versions.
- Error screen uses default libtesla framebuffer size (do NOT set `cfg::FramebufferWidth` before the check — only set it when proceeding to `DoomGui`).

**Definition of Done:**
- [ ] At heap=4 MB: error screen title visible within 2 s; engine never inits; perflog has no new entries; no crash dump
- [ ] At heap=6 MB: same (6 MB < 8 MB threshold)
- [ ] At heap=8 MB: error screen absent; Doom title screen appears; perflog accumulates entries
- [ ] B on error screen → clean exit to Ultrahand
- [ ] No `heap_size.bin` written under any condition
- [ ] No crash reports at any tested heap size

**Verify:**
- Manual hardware test at heap=4/6/8 MB. `ls /atmosphere/crash_reports/` count unchanged before/after each test.

---

### Task 11: Settings menu + INI persistence + BYO-WAD detection

**Objective:** Minus button → settings page. Render scale, master/music volume, active WAD, vibration toggle. Persists to `sdmc:/config/ultradoom/config.ini`.
**Dependencies:** Tasks 8 (input), 9 (audio)
**Mapped Truths:** T4, T6

**Files:**
- Create: `source/settings_gui.hpp` — `tsl::Gui` subclass with rows per setting
- Create: `source/config_ini.cpp` / `.hpp` — INI read/write under `[ultradoom]` section; wraps libultra's INI parser if available
- Modify: `source/main.cpp` — Minus button transitions from `DoomGui` to `SettingsGui` and back; on close, `config_ini_save()` then `config_ini_apply()`
- Modify: `chocolate-doom-nx-master/src/i_sound_nx.c` — read volume globals updated by SettingsGui

**INI keys under `[ultradoom]`:**
- `render_scale=2` (1/2/3)
- `master_volume=80` (0–100)
- `music_volume=80` (0–100)
- `active_wad=doom.wad`
- `vibration=0`

**BYO-WAD detection:** Scan `sdmc:/roms/doom/` for `*.wad` at SettingsGui construction. Show in "Active WAD" dropdown. (User already has `doom.wad`, `doom1.wad`; `freedoom1.wad` will appear if placed there.)

**Key Decisions:**
- Scale change: requires overlay close+reopen (libtesla layer recreate). Show toast "Restart overlay to apply". Do NOT attempt live re-resize.
- WAD change: also requires restart. Same toast.
- Volume changes: apply immediately (audio thread reads globals each submit cycle).
- Vibration: libnx HID rumble on weapon-fire events; off by default.
- INI write: atomic (write to `config.ini.tmp` → `fsync` → `rename` to `config.ini`).

**Definition of Done:**
- [ ] Minus → settings page; Minus or B → back to game
- [ ] All 4 settings persist across close+reopen
- [ ] Volume sliders apply in real time (audible change without restart)
- [ ] Scale change shows "restart to apply" toast
- [ ] `doom1.wad` at `sdmc:/roms/doom/` → appears in WAD dropdown alongside `doom.wad`
- [ ] Vibration on: pistol fire rumbles controller
- [ ] No INI corruption on partial write (atomic write verified)

**Verify:**
- Manual hardware test. Change each setting, close/reopen, confirm values persist.

---

### Task 12: Save/load wiring + release packaging

**Objective:** Confirm Doom's native save/load works. Saves to `sdmc:/config/ultradoom/savegames/`. Atomic write protects against sleep-mid-save. Build release zip.
**Dependencies:** Task 11
**Mapped Truths:** T3

**Files:**
- Modify: `chocolate-doom-nx-master/src/` — set Doom save directory to `sdmc:/config/ultradoom/savegames/` via `M_SetConfigDir()` equivalent; ensure directory created on first save
- Create: `source/atomic_save.c` / `.h` — wraps save write with tmp+fsync+rename
- Create: `tests/desktop/test_atomic_save.c` — simulate mid-write kill; verify existing .dsg untouched
- Create: `README.md` — install, heap-bump, controls, settings, BYO-WAD, save location, license
- Create: `scripts/dist.sh` — assembles release zip

**Release zip layout:**
```
ultradoom.ovl          → /switch/.overlays/ultradoom.ovl
README.md
LICENSE                (GPLv2 — chocolate-doom)
```
(No WAD included — user provides their own. README instructs placement at `sdmc:/roms/doom/`.)

**Key Decisions:**
- Atomic save: `fwrite` to `doomsavN.dsg.tmp` → `fsync` → `rename` to `doomsavN.dsg`. Prevents truncated save on sleep-mid-write.
- chocolate-doom's save code uses `M_StringJoin` to build save paths — patch or redirect via `M_SetConfigDir` rather than modifying every save call site.
- No WAD bundling (unlike Chase's Freedoom bundle) — user already has commercial WADs; README documents Freedoom as a free alternative with download URL.
- `make dist` target in Makefile.

**Definition of Done:**
- [ ] Save in slot 3 mid-level → exit overlay → reopen → load slot 3 → state restored within ±1 tick
- [ ] `.dsg` files appear under `sdmc:/config/ultradoom/savegames/`
- [ ] Save directory auto-created on first save
- [ ] Desktop atomic-write test: mid-save kill simulation leaves existing `.dsg` intact
- [ ] `make dist` produces `dist/ultradoom-v0.1.0.zip`
- [ ] Extracting zip to SD card produces working install (cold launch test)
- [ ] README covers install, heap-bump, controls, save location, BYO-WAD, Freedoom download, license

**Verify:**
- Manual hardware test (save round-trip).
- `make dist && unzip -l dist/ultradoom-*.zip`

---

## Risks and Mitigations

| Risk | Likelihood | Mitigation |
|---|---|---|
| chocolate-doom-nx-master i_sound_nx.c is a stub returning 0 | Medium | Read source before Task 4; if stub, implement audout backend from scratch |
| `audoutInitialize` fails on user's HOS+game combo | Medium | Silent fallback path in Task 4 covers this; one-time toast |
| D_DoomMain never returns cleanly (infinite loop) | Medium | `g_doom_quit` atomic checked in `D_DoomLoop`; `doom_nx_quit` flag for I_Quit(); SIGABRT handler for longjmp recovery |
| Heap < 8 MB on user's HOS version | Low | Task 10 error screen is unmissable and blocks engine init |
| `mklink /J` breaks on fresh Windows checkout | Low | Document command in Makefile comment; fresh checkout step in README |
| cholocalate-doom save paths not redirectable via M_SetConfigDir | Low | Patch save-path construction in 1–2 call sites if M_SetConfigDir insufficient |
| 35 Hz hold misses at scale=2 due to composite overhead | Low | Task 6 frame timer catches this early; fallback to scale=1 default |

---

## Deferred Ideas (unchanged from Chase's plan)

- NEON-accelerated blit (scalar reference sufficient for v1)
- OPL2/OPL3 music synth (software MIDI fallback acceptable for v1)
- Per-weapon haptic profiles (binary vibration toggle in v1)
- Mid-overlay scale change without restart
- Docked-mode optimization
