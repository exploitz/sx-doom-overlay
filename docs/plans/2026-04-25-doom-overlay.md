# Doom Overlay Implementation Plan

Created: 2026-04-25
Author: gober.chase@gmail.com
Status: PENDING
Approved: Yes
Iterations: 0
Worktree: No
Type: Feature

## Summary

**Goal:** Implement `sx-doom-overlay`, a libultrahand-based Nintendo Switch overlay that runs Doom (via the doomgeneric engine) inside the libtesla overlay surface, end-to-end playable on the OG Switch with bundled Freedoom 1, audio, save/load, runtime render scale, BYO-WAD support, and a heap-too-small error screen.

**PRD:** `docs/prd/2026-04-25-doom-overlay.md` — read this for full context (problem, scope, constraints, risks).

**Architecture:** `tsl::Overlay` → `tsl::Gui` → `tsl::elm::Element` skeleton (Tetris-Overlay pattern). The **engine + render path is single-threaded** (`DoomGui::update()` runs the 35 Hz Doom tick accumulator and calls `doomgeneric_Tick()` zero-or-more times; doomgeneric writes a 320×200 8-bit indexed buffer (`CMAP256`) into `DG_ScreenBuffer`; `DoomElement::draw()` does palette → RGBA4444 → integer-scale-blit into libtesla's framebuffer). **Audio runs on a separate libnx Thread** (priority 0x2c, 4 KB stack, started in Task 9) because `audoutAppendAudioOutBuffer` + `audoutWaitPlayFinish` is a blocking submit-and-wait pattern that would stall the 35 Hz tick if run on the main thread. The two threads communicate via a lock-free PCM ring buffer. Render scale (1× / 2× / 3×, default 2×) is selected at runtime via `cfg::FramebufferWidth`/`Height` (UltraGB-Overlay's `g_win_scale` pattern). The Doom palette is **pre-cached as 14 RGBA4444 banks at engine init** and switched per-frame via the platform shim's `DG_SetPalette` callback (damage flash, powerup tint, end-of-game fade). Heap budget is 8 MB system memory pool (anchored to nx-ovlloader's documented ceiling); a first-launch self-check refuses to start the engine when below.

**Tech stack:** C99 (engine), C++26 (overlay shim, libtesla convention), devkitA64 / libnx, libultrahand (libtesla + libultra), doomgeneric (vendored as git submodule, with a `patches/` overlay), Freedoom Phase 1 (BSD-3, bundled in release zip).

## Scope

### In Scope

- Single-player Doom playable end-to-end through Freedoom 1 (also accepts user-supplied DOOM1.WAD / DOOM2.WAD).
- Audio (sfx + music) via libnx `audout`, implementing doomgeneric's `DG_sound_module` slot. Silent fallback if `audoutInitialize` fails.
- Save/load to 8 slots persisted under `/config/sx-doom-overlay/savegames/`.
- Settings menu (`tsl::Gui`): render scale, master/music volume, active WAD, vibration toggle. Persists to `/config/sx-doom-overlay/config.ini`.
- BYO-WAD: detect any `*.wad` placed at `/switch/.overlays/doom/`, surface in settings dropdown.
- Heap-too-small first-launch error screen — clear remediation UI. **No** auto-write of `heap_size.bin`.
- Runtime render scale 1× / 2× / 3× (default 2×), modeled on UltraGB-Overlay's pattern.
- Native libnx HID → Doom key mapping; pause-on-overlay-dismiss; resume on re-summon.

### Out of Scope

- Multiplayer / netplay
- Demo recording / playback (`.lmp`)
- PWAD / DEH / mod loading
- GZDoom / ZDoom features (slopes, scripting, freelook beyond vanilla)
- Bundling commercial `DOOM1.WAD`
- Auto-writing `heap_size.bin`
- `.nro` standalone target (overlay-only)
- Switch-2 or Mariko-only optimizations

## Approach

**Chosen:** Vendored doomgeneric submodule + small `patches/` overlay + custom platform shim implementing the 6 `DG_*` functions.

**Why:** doomgeneric's clean platform-shim seam (`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`) was designed for this kind of port. We don't fork the engine — we keep upstream pinned and apply a 5-line patch to lower `MIN_RAM` from 6 MiB to 3 MiB so we fit in the 8 MB system-memory ceiling. doomgeneric also already has the full `i_sound.c` / `s_sound.c` framework — we plug into the existing `DG_sound_module` slot rather than re-porting from upstream Chocolate Doom.

**Alternatives considered:**

- **Re-port Chocolate Doom directly with a custom SDL stub.** Rejected: SDL is deeply woven through Chocolate Doom; the doomgeneric fork already abstracts the platform layer for us.
- **Fork doomgeneric.** Rejected: maintenance burden. Submodule + patches/ keeps upstream tracking clean.
- **Background thread for engine ticks.** Rejected: Tetris-Overlay and UltraGB-Overlay both ship single-threaded; threading inside an overlay adds complexity without clear benefit.
- **Default ARGB engine output instead of CMAP256.** Rejected: ARGB costs 192 KB more heap with no functional benefit.

## Context for Implementer

> Write for an implementer who has never seen the codebase. The `_reference/` directory should already be deleted by Task 1; consult the `lib/libultrahand` and `lib/doomgeneric` submodules instead.

- **doomgeneric platform-shim contract** (`lib/doomgeneric/doomgeneric/doomgeneric.h:38-43`) — six functions to implement, plus `DG_ScreenBuffer` (a `pixel_t*` the engine writes per frame). With `CMAP256`, `pixel_t = uint8_t`; without, `pixel_t = uint32_t`.
- **doomgeneric resolution macros** (`doomgeneric.h:7-13`) — `DOOMGENERIC_RESX` and `DOOMGENERIC_RESY` default to 640×400. Override at compile time to `320×200` for our case.
- **doomgeneric audio framework** (`lib/doomgeneric/doomgeneric/i_sound.c:74-78`) — `sound_modules[]` array. The first slot is `DG_sound_module`; we implement it. The framework above (`I_StartSound`, `I_UpdateSoundParams`, etc.) is already wired through.
- **doomgeneric MIN_RAM** (`lib/doomgeneric/doomgeneric/i_system.c:58-59`) — `DEFAULT_RAM` and `MIN_RAM` are both `6` MiB. Patched to `3` via `patches/0001-lower-min-ram.patch` (created in Task 2).
- **libultrahand makefile entry** — including `lib/libultrahand/ultrahand.mk` from our root Makefile pulls in libtesla + libultra build rules. Pattern from `_reference/Tetris-Overlay/Makefile:66`.
- **Overlay class skeleton** — model on Tetris-Overlay's `class TetrisGui : public tsl::Gui` (source/main.cpp:1194). Override `createUI()` and `update()`. `tsl::loop<DoomOverlay, LaunchFlags::None>` is the entire `main()`.
- **Runtime framebuffer scale pattern** — UltraGB-Overlay sets `ult::DefaultFramebufferWidth = static_cast<u32>(GB_W * g_win_scale)` (source/main.cpp:2978). We do the same with `DOOM_W = 320, DOOM_H = 200`. Read `g_render_scale` from the INI on overlay init, before the libtesla layer is created.
- **Pixel write primitive** — libtesla's `Renderer::setPixelAtOffset` (`lib/libultrahand/libtesla/include/tesla.hpp:1291`) is the fast path; per-pixel `setPixelBlendDst` is too slow for full-screen blit. Do not use `drawRect` for pixel data (Tetris does that for its 10×20 board, not for an N×M framebuffer).
- **Build flags** (from Tetris-Overlay's Makefile) — `-std=c++26 -fno-exceptions -fno-rtti -flto=6 -Wl,-wrap,__cxa_throw`, plus `-DUSE_EXCEPTION_WRAP=1`. doomgeneric is C99 — the shim layer is C++.
- **SD card paths** —
  - `/switch/.overlays/sx-doom-overlay.ovl` (the overlay itself)
  - `/switch/.overlays/doom/freedoom1.wad` (bundled IWAD)
  - `/switch/.overlays/doom/LICENSE.freedoom` (BSD-3 attribution)
  - `/switch/.overlays/doom/{doom1,doom2,*}.wad` (BYO IWAD overrides)
  - `/config/sx-doom-overlay/config.ini` (settings)
  - `/config/sx-doom-overlay/savegames/doomsavN.dsg` (saves)
- **Heap reality** (from PRD): nx-ovlloader allocates from the **system memory pool** (it's a sysmodule, not an applet). The README documents 4 / 6 / 8 MB. We require 8 MB. **Never auto-write `heap_size.bin`** — overshoot triggers `fatalThrow`.
- **Lifecycle gotcha** — overlay close ≠ process exit. When the user dismisses the overlay (Ultrahand close hotkey), libtesla stops calling `update()`; Doom state lives in memory, resumes on re-summon. Process exit (loader unload) loses state — recovery is via Doom's own save slots.

## Runtime Environment

Not applicable — this is a Switch homebrew overlay, not a service. Test paths:

- **Phases 1–3 (desktop):** `make -f Makefile.linux` builds and runs doomgeneric variants against Linux SDL or PPM-frame output for engine smoke and palette/blit/audio unit tests.
- **Phases 4+ (hardware):** `make` builds `out/sx-doom-overlay.ovl`. Copy to SD card at `/switch/.overlays/`, copy `data/freedoom1.wad` to `/switch/.overlays/doom/`, summon Ultrahand, pick "Doom Overlay."

## Assumptions

- **A1 — doomgeneric upstream is reachable and stable.** We pin a specific commit via submodule. If upstream disappears we vendor. — Tasks 1, 2 depend on this.
- **A2 — libnx `audout` accepts a 16-bit stereo 22050 Hz stream while a foreground game is active on at least most HOS+game combos.** Sys-tune precedent (uses `audren`, not identical, but similar coexistence pattern). If wrong, audio degrades to silent — code path already designed for fallback. — Task 9 depends on this for the success path; the fallback path is unconditional.
- **A3 — libtesla composite at 2× scale (640×400 RGBA4444 framebuffer) holds 35 Hz on Tegra X1 in handheld undocked.** RP2040-Doom precedent (35 fps at 320×200 on dual M0+ @ 133 MHz) suggests massive headroom. Confirmed by profiling in Task 7. — Tasks 7, 8 depend on this.
- **A4 — `MIN_RAM = 3 MiB` is sufficient for Freedoom 1 levels via lump caching.** Vanilla Doom shipped on 386s with 4 MB RAM total; our 3 MiB zone is just for working set. Hitching expected on level transitions. Documented as risk. — Task 7 depends on this.
- **A5 — The user's HOS + Atmosphère grants 8 MB heap from `svcSetHeapSize`.** This is the documented ceiling per nx-ovlloader README. If wrong (e.g., kernel pool tighter on some devices), user sees our heap-too-small error screen on first launch. — Task 10 depends on this.
- **A6 — Ultrahand's settings UI 8 MB option does not regress in any future Ultrahand release.** The `4 / 6 / 8 MB` ladder is documented; if it changes, our error-screen UX may become stale text. Low risk. — Task 10 depends.
- **A7 — Switch SD-card random-read latency is < 30 ms per lump.** Standard SD class 10 spec. Lump fetches dominate level-transition latency. — Task 7.

## Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| `audout` exclusive on user's HOS+game combo at *init time*, `audoutInitialize` fails. | Medium | Medium | `DG_sound_module->Init` returns false on `audoutInitialize` failure; doomgeneric falls back to silent automatically. One-time toast informs user. (Task 9 DoD covers this branch.) |
| `audout` *submit-time* failure — init succeeded but a foreground game later acquires the device exclusively, causing `audoutAppendAudioOutBuffer` to fail mid-gameplay. | Medium | Low | Audio thread checks every submit return code; on failure, sets `g_audio_failed`, drains ring, exits cleanly. `DG_sound_module->Update` short-circuits to no-op. Same one-time toast. Engine continues at 35 Hz unaffected. (Task 9 DoD covers this branch.) |
| **Sleep mid-save:** Switch suspends between fwrite calls, truncating the `.dsg` save file. | Low | Medium | Task 12 wraps Doom's save write with atomic write (`fwrite` to `doomsavN.dsg.tmp`, `fsync`, `rename` to `doomsavN.dsg`). Verified by intentionally killing the process mid-save in a desktop test. |
| **SD card pulled mid-lump-read** during level transition or mid-gameplay sprite fetch. | Low | Low | Documented as known limitation; doomgeneric's `W_LumpLength` / `W_ReadLump` short-read produces an `I_Error` — engine shows its own error screen rather than corrupting state. We do not catch or recover. |
| **Multi-overlay audout conflict:** user opens DoomOverlay, then summons Ultrahand and opens a second overlay; on returning to Doom the audio thread state may be indeterminate. | Low | Low | The same `AppletHookCookie` resume path used for sleep/wake also triggers on overlay re-summon — drops the audio ring and reinits `audoutStartAudioOut`. If reinit fails, falls into the silent-fallback path. |
| `MIN_RAM = 3` proves too tight for Freedoom 1 — engine `Z_Malloc` fails mid-level. | Medium | High | Profile zone usage in Task 7 with the heaviest Freedoom levels. If hitting OOM, raise to `MIN_RAM = 4` and shrink something else (drop scale 3× option, smaller audio buffer). Documented as a tunable, not a hardcode. |
| libtesla 2×-scale composite cost > 35 Hz budget on Erista undocked. | Medium | Medium | Task 7 includes a per-frame timer log. If 2× misses 35 Hz, default falls back to 1× (the 320×200 framebuffer is trivially fast). User can manually pick 2× or 3× anyway. |
| Patch on doomgeneric (`MIN_RAM`) doesn't apply cleanly on submodule update. | Low | Low | `make patch` step uses `git apply --check` first; if it fails, build halts with a clear "patches need re-roll" message. Patches are 5 lines — re-rolling takes minutes. |
| Sleep/wake desyncs audio buffers (sys-tune precedent). | Low | Low | On `AppletMessage_Resume`, drop the audio ring and reinit `audout`. (Task 9 DoD.) |
| HOS major version bump breaks libtesla / libultrahand. | Low | High (out of band) | Pin libultrahand submodule. Document the pinned HOS range in README. Re-test on each HOS bump separately from this plan. |
| User on HOS 21+ with 4 MB default heap, ignores error screen. | Medium | Low | Error screen is unmissable and unblockable (only "Quit" button). README also calls out the requirement. |

## Goal Verification

### Truths

The PRD's intent is satisfied if and only if these observable truths hold (all on **OG Switch handheld undocked, real hardware**):

1. **T1 — Plays Doom end-to-end.** From cold launch, the user can pick Doom Overlay in Ultrahand and reach Freedoom 1 episode 1, map 1's exit door, killing every enemy along the way, with the game responding to input throughout.
2. **T2 — Audio works (or fails gracefully).** Either: shotgun fire, monster grunts, and level music are audible while playing; OR the audio-init-failed toast appeared once and the game continues silently.
3. **T3 — Save/load round-trips.** A save in slot 3 reloads to within ±1 tick of where the player saved (position, weapons, ammo, monsters).
4. **T4 — Settings persist.** After picking 1× scale, volume 50%, active WAD = `doom1.wad`, exit and re-launch the overlay; the picked values are still in effect on cold launch.
5. **T5 — Heap error screen guards the entry point.** On a fresh HOS 21+ install (default 4 MB heap), launching the overlay shows the `Heap Size Too Small` error screen within 2 seconds; the engine never initializes (Freedoom title screen never appears; `perflog.txt` accumulates no entries); no new crash report is written under `/atmosphere/crash_reports/`. After bumping heap to 8 MB through Ultrahand Settings, the engine initializes normally on next launch.
6. **T6 — BYO-WAD detected.** Drop `doom1.wad` at `/switch/.overlays/doom/`; the settings menu's WAD dropdown lists both `freedoom1.wad` and `doom1.wad`.
7. **T7 — Pause-on-dismiss preserves state.** Mid-level, dismiss the overlay; re-summon within the same session; gameplay resumes from the same tick (player position, monster AI, in-flight projectiles preserved). (Depends on Task 6's pre-task verification of libtesla's dismiss/resume lifecycle.)
8. **T8 — 35 Hz holds at scale=2.** During 60 seconds of active E1M1 gameplay (not attract demo) at scale=2 on OG Switch handheld undocked, `perflog.txt` shows no more than 5 consecutive frames exceeding 30 ms tick-to-tick delta. Frame timing log is the artifact; verifier extracts and reports the worst 1% of frame deltas.

### Artifacts

- `source/main.cpp` — `DoomOverlay`, `DoomGui`, `DoomElement`, `tsl::loop` entry. (T1, T7, T8)
- `source/doomgeneric_switch.c` — the 6 `DG_*` functions; key queue; tick clock; per-frame palette refresh from `extern colors[256]` + `palette_changed` flag. (T1, T7)
- `source/i_sound_switch.c` — `DG_sound_module` against libnx audout; checks `g_audio_failed` to short-circuit on submit-time failure. (T2)
- `source/audio_backend_libnx.c` — dedicated audio thread, ring buffer, init-time + submit-time failure handling. (T2)
- `source/blit.cpp` — palette→RGBA4444→scale upscaler with 14-bank PLAYPAL pre-cache and per-frame palette switch detection. (T1)
- `source/settings_gui.hpp` — `tsl::Gui` for scale/volume/WAD/vibration. (T4, T6)
- `source/error_heap_gui.hpp` — heap-too-small error screen. (T5)
- `source/input_map.hpp` — libnx HID → Doom keys. (T1)
- `lib/doomgeneric/` — submodule, patched via `patches/` directory. (engine for T1)
- `data/freedoom1.wad` — bundled IWAD. (T1)
- `Makefile` — root build, includes `lib/libultrahand/ultrahand.mk`, applies patches with the loud-fail pattern. (build)
- `LIFECYCLE_NOTES.md` — confirmed libtesla dismiss/resume behavior with `tesla.hpp` line numbers. (T7 — supporting evidence)
- `tests/desktop/` — Linux unit tests for blit (with palette switching), audio mixer, INI parser, atomic save write. (CI / Phase 1–2 verification)
- `/config/sx-doom-overlay/perflog.txt` — frame-time log produced on hardware. (T5 absence-of-entries proof, T8 frame-delta proof)
- `/atmosphere/crash_reports/` — checked for absence in T5 verification

## Hardware Verification Checklist (replaces structured E2E scenarios)

This is a Switch overlay — browser-driven E2E doesn't apply. The Goal Verification truths T1–T7 above are the hardware verification contract; each is a manual procedure. The verifier (or implementer at Phase 4 milestone) walks through each truth on the OG Switch device and records pass/fail. Output goes into a section of the verify report referencing the Truth ID. No structured `TS-NNN` form is needed because there's no automated browser harness.

## Progress Tracking

- [x] Task 1: Project bootstrap (git init, submodules, Makefile, .gitignore) — committed `2367015`. Three DoD items pending devkitPro install: full `make`, `make patches` no-op, patch-failure-loud test.
- [x] Task 2: Desktop engine smoke + MIN_RAM patch — committed below. Engine boots Freedoom 1 to "Phase 1" banner, 20 PPM frames produced, ASan + UBSan clean. valgrind DoD substituted with ASan (valgrind not installed, no sudo).
- [ ] Task 3: Palette + scale blit module (desktop unit-tested)
- [ ] Task 4: Audio mixer module (`DG_sound_module` impl, desktop unit-tested)
- [ ] Task 5: Cross-build smoke (devkitA64 produces .ovl)
- [ ] Task 6: Overlay skeleton on hardware (test pattern)
- [ ] Task 7: Engine integration on overlay (Freedoom plays, no audio/UI yet)
- [ ] Task 8: Input mapping (libnx HID → Doom keys, full controllable gameplay)
- [ ] Task 9: Audio integration on hardware (sfx + music; coexistence-fallback path)
- [ ] Task 10: Heap-too-small error screen + first-launch self-check
- [ ] Task 11: Settings menu + INI persistence + BYO-WAD detection
- [ ] Task 12: Save/load wiring + release packaging

**Total Tasks:** 12 | **Completed:** 2 | **Remaining:** 10

## Implementation Tasks

### Task 1: Project bootstrap

**Objective:** Initialize the project repository, add the two build dependencies as submodules, write the Makefile skeleton, and remove planning-only reference clones.
**Dependencies:** None
**Mapped Truths:** None (foundation — all later tasks depend on this)

**Files:**

- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/.gitignore`
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/.gitmodules`
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/Makefile`
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/sx-doom-overlay.json` (NACP config)
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/source/main.cpp` (placeholder `tsl::loop` entry that exits cleanly)
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/patches/.gitkeep`
- Create: `/mnt/c/Users/Chase/dev/sx-doom-overlay/data/.gitkeep`
- Delete: `/mnt/c/Users/Chase/dev/sx-doom-overlay/Ultrahand-Overlay/` (reference)
- Delete: `/mnt/c/Users/Chase/dev/sx-doom-overlay/_reference/` (reference)
- Submodule add: `https://github.com/ppkantorski/libultrahand.git` → `lib/libultrahand`
- Submodule add: `https://github.com/ozkl/doomgeneric.git` → `lib/doomgeneric`

**Key Decisions / Notes:**

- `git init` at the working dir; first commit covers the bootstrap files only (no engine source — that's submodule territory).
- `.gitignore` covers `build/`, `out/`, `*.nacp`, `*.nro`, `*.ovl`, `*.elf`, `Ultrahand-Overlay/`, `_reference/`, `/tmp` artifacts, and the bundled WAD (we bundle it into the release zip from `data/`, but the file itself stays out of git history).
- Makefile structure: ARM64 `.ovl` target, `include $(TOPDIR)/lib/libultrahand/ultrahand.mk`, `LIBS = -lcurl -lz -lmbedtls -lmbedx509 -lmbedcrypto -lnx` (Tetris-Overlay's link line; audio handling rides on `-lnx`), `CFLAGS += -DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200 -DUSE_EXCEPTION_WRAP=1`.

- **`patches` target — must fail loud, not silent.** Required pattern (verbatim, do NOT shorten):

  ```make
  PATCH_SENTINEL := lib/doomgeneric/.patched
  
  $(PATCH_SENTINEL): $(wildcard patches/*.patch) | lib/doomgeneric
  	@set -e; \
  	cd lib/doomgeneric && \
  	for p in $(addprefix ../../,$(sort $(wildcard patches/*.patch))); do \
  		echo "Applying $$p"; \
  		git apply --check "$$p" || { echo "ERROR: patch $$p does not apply cleanly. Re-roll the patch against the current submodule HEAD before continuing."; exit 1; }; \
  		git apply "$$p" || { echo "ERROR: git apply failed for $$p (check passed but apply failed — this should not happen)"; exit 1; }; \
  	done
  	@touch $(PATCH_SENTINEL)
  
  build: $(PATCH_SENTINEL)
  ```

  The `set -e` plus the explicit `|| { echo ...; exit 1; }` on each git invocation ensures the build halts with a human-readable message if any patch fails. The sentinel file (`lib/doomgeneric/.patched`) makes re-applying idempotent across incremental builds, and gets blown away on `make clean` or `git submodule update`.

- Verification fixture for this guard: a deliberately-broken patch (e.g. `patches/9999-broken.patch.disabled`) sitting in `tests/patches/` that the test suite renames into `patches/` and runs `make` against; the test passes only when `make` exits non-zero with the expected error message in stderr.
- `source/main.cpp` for this task is just `int main() { return tsl::loop<EmptyOverlay>(); }` with an `EmptyOverlay : tsl::Overlay` that has no GUI — it just exits cleanly when the user dismisses. Validates the toolchain end to end before adding engine code.

**Definition of Done:**

- [ ] `git status` reports clean working tree after first commit
- [ ] `git submodule status` shows two submodules at the pinned upstream HEADs
- [ ] `Ultrahand-Overlay/` and `_reference/` directories no longer exist
- [ ] `make` produces `out/sx-doom-overlay.ovl` of plausible size (>10 KB, <200 KB) — empty overlay
- [ ] `make patches` succeeds (no-op since no patches yet, but the target exists and uses the `set -e + || exit 1` pattern documented in Key Decisions)
- [ ] **Patch-failure-loud test:** create a deliberately-broken `tests/patches/9999-broken.patch.disabled`, copy it into `patches/`, run `make` — verify non-zero exit code AND a human-readable error message in stderr containing "patch ... does not apply cleanly"; then remove the broken patch file
- [ ] No diagnostics errors in `source/main.cpp`

**Verify:**

- `git submodule status | wc -l` returns `2`
- `ls Ultrahand-Overlay/ _reference/ 2>&1` reports "No such file or directory"
- `file out/sx-doom-overlay.ovl` reports an ARM64 NRO binary

---

### Task 2: Desktop engine smoke + MIN_RAM patch

**Objective:** Apply the `MIN_RAM = 3` patch to doomgeneric, build the engine on Linux against doomgeneric's existing `Makefile.soso` or a minimal stub renderer, and confirm Freedoom 1 boots and renders frames.
**Dependencies:** Task 1
**Mapped Truths:** None (engine smoke; supports T1)

**Files:**

- Create: `patches/0001-lower-min-ram.patch` (changes `DEFAULT_RAM 6` → `3` and `MIN_RAM 6` → `3` in `lib/doomgeneric/doomgeneric/i_system.c:58-59`)
- Create: `tests/desktop/Makefile` (Linux build of doomgeneric with stub renderer that dumps PPM frames)
- Create: `tests/desktop/stub_platform.c` (fills the 6 `DG_*` shim functions: PPM frame dump for `DG_DrawFrame`, `usleep` for `DG_SleepMs`, `clock_gettime` for `DG_GetTicksMs`, stdin scancode-to-DOOMKEY for `DG_GetKey`)

**Key Decisions / Notes:**

- The patch is intentionally small. Verify `git apply --check patches/0001-lower-min-ram.patch` before applying. Comment in the patch header explains why.
- The desktop stub uses the same `CMAP256 + 320×200` configuration we'll use on Switch — validates the engine output format.
- Use Freedoom 1's freely-available `freedoom1.wad` for testing (downloaded into `data/`, gitignored).
- Smoke test: launch the engine for ~120 ticks (~3.4 seconds at 35 Hz), dump every 10th frame to `tests/desktop/out/frame_NNN.ppm`, exit cleanly. Expected: title screen → demo intro frames present, no crashes, no `Z_Malloc` failures.
- This task is **performance-sensitive only inasmuch as it must not OOM** — frame timing on Linux is meaningless. The point is to validate engine + patch + WAD + our chosen build flags.

**Definition of Done:**

- [ ] `make patches` applies cleanly (Task 1 target succeeds for the new patch)
- [ ] `make -C tests/desktop smoke` builds and runs without zone-allocator failures
- [ ] At least 12 PPM frames are produced under `tests/desktop/out/`
- [ ] Freedoom 1's title-screen palette is visibly correct in frame_000.ppm (red "FREEDOOM" against dark background)
- [ ] No memory leaks reported by valgrind (`make -C tests/desktop smoke-valgrind`)
- [ ] Patch is reversible: `cd lib/doomgeneric && git apply -R ../../patches/0001-lower-min-ram.patch && git apply ../../patches/0001-lower-min-ram.patch` succeeds

**Verify:**

- `make -C tests/desktop smoke && ls tests/desktop/out/*.ppm | wc -l` ≥ `12`
- `grep "MIN_RAM" lib/doomgeneric/doomgeneric/i_system.c` shows `3` after patches applied

---

### Task 3: Palette + scale blit module (with palette switching)

**Objective:** Implement and unit-test the pure-C++ blit primitive that converts a 320×200 8-bit indexed buffer (using Doom's currently-active palette) into an N×M RGBA4444 buffer at integer scale, AND correctly handles Doom's per-frame palette switches (damage flash, powerup tint, end-of-game fade).
**Dependencies:** Task 2
**Mapped Truths:** Supports T1

**Files:**

- Create: `source/blit.hpp` (declarations: `void blit_doom_to_rgba4444(const uint8_t* src, const uint16_t* lut, uint16_t* dst, int scale)`, `void rebuild_palette_lut_from_colors(uint16_t* lut)`)
- Create: `source/blit.cpp` (implementation; scalar reference + simple SIMD-friendly loops; no NEON intrinsics yet — that's a stretch goal)
- Create: `tests/desktop/test_blit.cpp` (gtest or a hand-rolled assertion harness; golden-frame fixtures)
- Create: `tests/desktop/golden/playpal.bin` (raw 14-bank PLAYPAL extracted from Freedoom — for fixture; 14 × 256 × 3 bytes)
- Create: `tests/desktop/golden/title_320x200_indexed.bin` (one captured 320×200 indexed frame from Task 2)
- Create: `tests/desktop/golden/title_640x400_rgba4444_palette0.bin` (expected 2× output with PLAYPAL bank 0 — the normal palette)
- Create: `tests/desktop/golden/title_640x400_rgba4444_palette3.bin` (expected 2× output with PLAYPAL bank 3 — a damage-flash palette; visibly redder)

**Key Decisions / Notes:**

- **Palette mechanism (the part Task 3 must NOT get wrong):** doomgeneric's `i_video.c` exposes two extern symbols (declared in `lib/doomgeneric/doomgeneric/i_video.h:170-171`):

  ```c
  extern boolean palette_changed;          // set by I_SetPalette, cleared by us
  extern struct color colors[256];          // a/r/g/b per slot, gamma-corrected
  ```

  Doom calls `I_SetPalette()` whenever the active palette changes — taking damage, picking up a berserk pack, completing the game. `I_SetPalette` (which we don't modify) writes the new RGB triples into `colors[]` and sets `palette_changed = true` (in CMAP256 mode). The platform shim (us) is responsible for reading these and rebuilding our RGBA4444 LUT. Reference: see `lib/doomgeneric/doomgeneric/doomgeneric_allegro.c:135-153` for the upstream pattern.

- **Initial PLAYPAL pre-cache:** at engine init, after `doomgeneric_Create` returns, walk all 14 PLAYPAL banks (Freedoom and vanilla Doom both have 14) and pre-compute 14 × 256-entry RGBA4444 LUTs. Total: 14 × 256 × 2 bytes = **7 KB heap** — trivial. This is *purely a startup optimization* — it lets us avoid recomputing the LUT during damage flashes (a hot path during gameplay). Lookup index = which bank Doom currently has loaded.

- **Per-frame palette refresh:** at the top of `DoomElement::draw`, check `palette_changed`. If set, rebuild the active LUT from `colors[]` (which has Doom's current bank with gamma applied) and clear the flag. For the initial case this just picks one of the 14 pre-cached banks; for unusual cases (custom WADs with non-vanilla PLAYPAL counts) we recompute on the fly.

- **Scale algorithm:** integer nearest-neighbor. For scale=N, each source pixel becomes an N×N block in the destination. Inner loop reads source byte, looks up active LUT → 16-bit RGBA4444, writes to dst N times horizontally, repeats N rows.

- **Performance budget:** 320×200 * scale² pixels per frame × 35 frames/s. At scale=2: ~9 megapixels/s — trivial for Tegra X1's NEON-capable A57. At scale=3: ~20 megapixels/s — still trivial. Reference scalar implementation is fine; NEON is a future stretch task.

- **Hot-path note:** function is called every libtesla composite (~60 Hz). LUT is rebuilt only on palette change (cheap when the flag is unset).

**Definition of Done:**

- [ ] `make -C tests/desktop test_blit` builds and runs all unit tests
- [ ] **Palette 0 round-trip:** `blit_doom_to_rgba4444(input, lut_pal0, output, scale=2)` matches `title_640x400_rgba4444_palette0.bin` byte-for-byte
- [ ] **Palette 3 round-trip (damage flash):** `blit_doom_to_rgba4444(input, lut_pal3, output, scale=2)` matches `title_640x400_rgba4444_palette3.bin` and is **visibly different** from palette 0 (red channel max-bit set across at least 50% of pixels — proves the LUT is actually being used)
- [ ] Round-trip tests for scales 1, 2, 3 all pass at palette 0
- [ ] **Palette switch test:** simulate `palette_changed = true` mid-frame, call `rebuild_palette_lut_from_colors`, verify the LUT now matches expected bank-N values
- [ ] Microbenchmark prints throughput in MB/s (informational, no DoD threshold — for sanity check)
- [ ] No undefined-behavior reports from `-fsanitize=address,undefined`

**Verify:**

- `make -C tests/desktop test_blit && ./tests/desktop/out/test_blit`

---

### Task 4: Audio mixer module (`DG_sound_module` impl)

**Objective:** Implement doomgeneric's `DG_sound_module` slot against a desktop-stubbed `audout` (writes 16-bit stereo PCM to a WAV file). Unit-test sfx mixing and music synth output against expected reference samples.
**Dependencies:** Task 2 (engine smoke)
**Mapped Truths:** Supports T2

**Files:**

- Create: `source/i_sound_switch.c` (the `DG_sound_module`'s `Init`, `Shutdown`, `GetSfxLumpNum`, `Update`, `UpdateSoundParams`, `StartSound`, `StopSound`, `SoundIsPlaying` functions; PCM mixing + DMX MUS-to-MIDI fallback synth)
- Create: `source/audio_backend.hpp` (declares an abstract sink: `audio_backend_init`, `audio_backend_submit(int16_t* pcm, size_t frames)`, `audio_backend_shutdown` — implemented twice: once for libnx audout in production, once for desktop-WAV in tests)
- Create: `source/audio_backend_libnx.c` (libnx audout implementation — stub for now; wires up properly in Task 9)
- Create: `tests/desktop/audio_backend_wav.c` (test backend writing to `out/audio_capture.wav`)
- Create: `tests/desktop/test_sound.c` (plays a known sfx via the engine, captures, compares against golden WAV)
- Create: `tests/desktop/golden/dspistol_22050.wav` (reference output of the pistol fire sfx mixed to PCM)

**Key Decisions / Notes:**

- doomgeneric's `i_sound.c` already has the upper layer (`I_StartSound`, `I_PlaySong`, etc.) wired into the engine. Our `DG_sound_module` plugs into `sound_modules[]` at `lib/doomgeneric/doomgeneric/i_sound.c:74-78`. We do **not** modify `i_sound.c` itself; we just provide the lower platform driver via our own file.
- **PCM format:** 16-bit signed stereo, 22050 Hz (Doom's native sfx rate). 8-channel mixer (Doom's max simultaneous sounds). Output buffer = ring of 4 × 1024-frame chunks ≈ 16 KB total.
- **Music:** Doom's MUS format → simple MIDI synth (we lift from Chocolate Doom's `mus2mid.c` if needed; doomgeneric may already have it stubbed). Music is mixed into the same PCM stream.
- **Volume control:** master volume (0–100%) and music volume (0–100%) scale per-channel before mixing.
- **Backend abstraction:** the `audio_backend` interface (Switch audout vs desktop WAV) is the seam. Production backend isn't wired until Task 9; here we use the WAV backend for testing.

**Definition of Done:**

- [ ] `make -C tests/desktop test_sound` builds and runs
- [ ] Pistol-fire sfx test produces a WAV that matches `dspistol_22050.wav` within ±1 LSB per sample (allow tiny rounding)
- [ ] Multi-channel mix test (3 simultaneous sfx) produces non-clipping output (max |sample| < 32700)
- [ ] Music synth produces non-zero PCM for at least one MUS lump from Freedoom1
- [ ] Volume scaling: 50% master volume produces samples at half amplitude vs 100%
- [ ] No memory leaks; no buffer overruns under sanitizers

**Verify:**

- `make -C tests/desktop test_sound && ./tests/desktop/out/test_sound`

---

### Task 5: Cross-build smoke

**Objective:** Confirm the project cross-compiles to ARM64 with devkitA64 and produces a valid `.ovl` (no missing symbols, plausible binary size). No deployment yet — this is a build-system gate.
**Dependencies:** Tasks 3, 4
**Mapped Truths:** None (build-system gate)

**Files:**

- Modify: `Makefile` (link in the new sources from Tasks 3 + 4; verify `audio_backend_libnx.c` compiles even though it's a stub)
- Create: `scripts/check-ovl-size.sh` (reads the file size and warns if outside expected range)

**Key Decisions / Notes:**

- Build flags must include `-DCMAP256 -DDOOMGENERIC_RESX=320 -DDOOMGENERIC_RESY=200`. Verify these reach the doomgeneric source compilation by inspecting the `.d` file or grepping the output object's symbols.
- The patches step (`make patches`) must run before the doomgeneric source is compiled. Add explicit `lib/doomgeneric/.patched` sentinel file so re-builds don't re-apply patches every time.
- Expected `.ovl` size: 1–3 MB (similar to UltraGB-Overlay; Tetris-Overlay is smaller because no engine).

**Definition of Done:**

- [ ] `make clean && make` produces `out/sx-doom-overlay.ovl` without errors or warnings (`-Werror` on)
- [ ] `nm out/build/main.o | grep -c "DG_"` returns ≥ 6 (the 6 platform shim symbols are present in our shim)
- [ ] `scripts/check-ovl-size.sh out/sx-doom-overlay.ovl` reports size between 1 MB and 3 MB
- [ ] No undefined-symbol warnings from the linker
- [ ] `nm` does NOT show any libstdc++ exception or RTTI symbols (no `__cxa_throw`, `_ZTI`)

**Verify:**

- `make clean && make 2>&1 | tee build.log && grep -E "(error|warning)" build.log`
- `scripts/check-ovl-size.sh out/sx-doom-overlay.ovl`

---

### Task 6: Overlay skeleton on hardware

**Objective:** Deploy a minimal overlay to a real Switch that initializes the framebuffer at the runtime scale and renders a recognizable test pattern (color bars, scrolling stripes). Confirms libtesla integration, framebuffer dimensions, basic input, AND the dismiss/resume lifecycle that T7 depends on.
**Dependencies:** Task 5
**Mapped Truths:** None directly — but **verifies the lifecycle assumption that T7 depends on**

**Pre-task verification (no code, ~30 min):**

Before writing any of this task's source, the implementer reads `lib/libultrahand/libtesla/include/tesla.hpp` around the main loop (search for `tsl::loop`, the `mainLoop` function, and the dismiss/show paths) to confirm three things:

1. `tsl::Overlay::onHide()` exists (or its equivalent) and the default behavior is a no-op (does not destroy the active Gui).
2. The active Gui object and its members are not destroyed when the overlay is dismissed via the Ultrahand close hotkey — only when the overlay process exits.
3. `Gui::update()` is simply not called while the overlay is hidden; it resumes being called on re-summon, with the Gui's instance state intact.

Document the confirmed behavior in a `LIFECYCLE_NOTES.md` file at the project root (one paragraph, with the `tesla.hpp` line numbers). If the actual behavior differs from these assumptions, **flag as a deviation immediately and adjust T7 + Task 7's pause-on-dismiss design** before proceeding.

**Files:**

- Modify: `source/main.cpp` — `class DoomOverlay : public tsl::Overlay`, `class DoomGui : public tsl::Gui`, `class DoomElement : public tsl::elm::Element`. `DoomOverlay::initServices` sets `cfg::FramebufferWidth = 320 * scale` / `Height = 200 * scale` from a hardcoded `scale = 2`. `DoomElement::draw` writes a test pattern (color bars + frame counter). `DoomGui::handleInput` exits on B.
- Create: `source/test_pattern.cpp` (color-bars + frame-counter test renderer)

**Key Decisions / Notes:**

- Hardcode `scale = 2` for now. INI persistence comes in Task 11; we just need to validate the framebuffer mechanism.
- Use `Renderer::setPixelAtOffset` for the per-pixel writes. Time the per-frame draw with `armGetSystemTick` and log to `/config/sx-doom-overlay/perflog.txt` to confirm composite cost is in budget.
- This is the first hardware test. Ship a small `dist/` zip with just `sx-doom-overlay.ovl` for the user to drop into `/switch/.overlays/` on their Switch's SD card.

**Definition of Done:**

- [ ] `.ovl` deploys to Switch, summons cleanly from Ultrahand
- [ ] At scale=1 (override hardcoded value), the test pattern displays 320×200 centered on screen
- [ ] At scale=2, displays 640×400 centered with no visible artifacts (no torn pixels, no off-color bands)
- [ ] At scale=3, displays 960×600 centered
- [ ] B button cleanly exits overlay; Doom logo not yet shown (this is a test-pattern build); Ultrahand returns
- [ ] Per-frame draw time is logged to `perflog.txt`; values are < 16 ms at scale=2 (i.e. fits 60 Hz composite budget)
- [ ] No `fatalThrow`s in the foreground game while the overlay is open

**Verify:**

- Manual hardware test on OG Switch handheld undocked. Implementer captures a screenshot of each scale and includes in the verification report.
- `cat /config/sx-doom-overlay/perflog.txt | awk '{print $2}' | sort -n | tail -5` confirms top frame times

---

### Task 7: Engine integration on overlay

**Objective:** Wire doomgeneric into the overlay end-to-end. The overlay opens, loads `freedoom1.wad`, runs the engine at 35 Hz, and renders gameplay (no audio yet, no input control yet beyond Doom's auto-demo).
**Dependencies:** Task 6
**Mapped Truths:** T1 (initial path), T8 (35 Hz hold)

**Files:**

- Modify: `source/main.cpp` — replace test pattern with engine integration. `DoomOverlay::initServices` calls `doomgeneric_Create(argc, argv)` with `argv = ["doom", "-iwad", "/switch/.overlays/doom/freedoom1.wad", "-mb", "3"]`. `DoomGui::update` runs the 35 Hz tick accumulator and calls `doomgeneric_Tick()`. `DoomElement::draw` reads `DG_ScreenBuffer` (CMAP256, 320×200) and blits via Task 3's `blit_doom_to_rgba4444` into libtesla's framebuffer.
- Modify: `source/doomgeneric_switch.c` — implements the 6 `DG_*` functions. `DG_Init` allocates the screen buffer (handled by doomgeneric itself) and the palette LUT. `DG_DrawFrame` is a no-op (we blit in `DoomElement::draw`). `DG_SleepMs` calls `svcSleepThread(ms * 1000000ULL)`. `DG_GetTicksMs` returns `armGetSystemTick / 19200` (Tegra X1 timer = 19.2 MHz). `DG_GetKey` pulls from a libnx-HID-fed key queue — but the queue is unused in this task (input is Task 8). `DG_SetWindowTitle` is a no-op.

**Key Decisions / Notes:**

- The argv to `doomgeneric_Create` is fixed for this task. Active-WAD selection from settings comes in Task 11.
- **Palette setup:** after `doomgeneric_Create` returns, pre-cache all 14 PLAYPAL banks → 14 × 256-entry RGBA4444 LUTs (the mechanism from Task 3). At the top of `DoomElement::draw`, read `extern palette_changed` (from `lib/doomgeneric/doomgeneric/i_video.h:170`); if set, rebuild the active LUT from `extern colors[256]` (line 171) and clear the flag. This is the contract that makes damage flash, powerup tint, and end-game fade visible on hardware — the engine drives palette state via `I_SetPalette`, and our shim is the consumer.
- 35 Hz tick accumulator: track `last_tick_ns` and `accumulated_ns`; on each `update()`, add the delta, then run `doomgeneric_Tick()` while `accumulated_ns >= 28571429ull` (≈ 1/35 s in ns), subtracting after each call. Cap at 4 ticks per `update()` to avoid spiral of death if libtesla composite stalls.
- Performance: log per-tick cost during dev. If 35 Hz isn't holding at scale=2, surface clearly and feed back into Task 11's "default scale = 1" decision.

**Definition of Done:**

- [ ] Freedoom 1 boots to its title screen on Switch hardware
- [ ] Demo loop plays automatically (Doom's built-in attract demos) for at least 30 seconds without crash
- [ ] Frame timing logged: 35 Hz simulation rate stable to within ±1 tick over 1000 ticks at scale=2
- [ ] Memory: peak heap usage logged via `mallinfo`-equivalent; stays < 7.5 MB to leave the 0.5 MB margin
- [ ] No `Z_Malloc` failures during 5 minutes of attract-demo gameplay
- [ ] Visual sanity: title screen colors look correct (red FREEDOOM banner, dark background)

**Verify:**

- Manual hardware test. Verifier records 30+ seconds of attract-demo gameplay (phone video acceptable as artifact).
- Implementer attaches `perflog.txt` and `heaplog.txt` from a 5-minute soak.

---

### Task 8: Input mapping (libnx HID → Doom keys)

**Objective:** Enable the player to actually control Doom — D-pad, ABXY, sticks, triggers all map to Doom keycodes; queue feeds `DG_GetKey`; Freedoom 1 episode 1 map 1 is fully playable to the exit.
**Dependencies:** Task 7
**Mapped Truths:** T1

**Files:**

- Create: `source/input_map.hpp` (mapping table from libnx `HidNpadButton` to `doomkeys.h` constants)
- Modify: `source/doomgeneric_switch.c` — `DG_GetKey` pulls from the same key queue that `DoomGui::handleInput` writes to.
- Modify: `source/main.cpp` — `DoomGui::handleInput(keysDown, keysHeld, ...)` translates HID button events to the key queue.

**Key Decisions / Notes:**

- Default map (configurable in Task 11):
  - Left stick → forward/back/strafe (analog → digital threshold ~0.4)
  - Right stick → turn left/right (analog → digital threshold ~0.4)
  - A → KEY_FIRE
  - B → KEY_USE
  - X → KEY_FIRE alt / weapon next (configurable later)
  - Y → weapon previous
  - L → strafe-left modifier
  - R → strafe-right modifier
  - ZL → KEY_RSHIFT (run modifier)
  - ZR → KEY_FIRE alt
  - D-pad → menu nav inside Doom's own menu
  - Plus → KEY_ESCAPE (Doom in-game menu)
  - Minus → opens our settings (Task 11)
- Press / release events both queued so doomgeneric correctly tracks key-held state.
- HID polling cadence comes for free from libtesla's `update()` (which provides `keysDown` / `keysHeld`).

**Definition of Done:**

- [ ] Player walks, strafes, turns, fires, opens doors with the controls above
- [ ] Key release is registered (e.g., releasing A stops the chaingun)
- [ ] Plus → Doom in-game menu opens
- [ ] Stick deadzone is comfortable; no stuck movement after stick returns to center
- [ ] Player can complete Freedoom 1 E1M1 to the exit door (the literal Doom-on-anything ritual)

**Verify:**

- Manual hardware test. Verifier completes E1M1, records play time and any control issues.

---

### Task 9: Audio integration on hardware

**Objective:** Wire the libnx `audout` backend into Task 4's `DG_sound_module` so sfx and music play during real gameplay. Detect and gracefully fall back at BOTH init time AND submit time (foreground game may acquire the audio device after init succeeds).
**Dependencies:** Task 8 (so we can play with sound while testing)
**Mapped Truths:** T2

> **Threading note:** This task introduces the project's only secondary thread. The main `DoomGui::update()` / `DoomElement::draw()` path remains single-threaded for engine + render. Audio runs on a dedicated libnx `Thread` because `audoutAppendAudioOutBuffer` + `audoutWaitPlayFinish` is blocking and would stall the 35 Hz tick if run on the main thread. Communication is via a lock-free PCM ring buffer (single-producer = `i_sound_switch.c`'s mixer call from main thread; single-consumer = audio thread).

**Files:**

- Modify: `source/audio_backend_libnx.c` — `audoutInitialize`, `audoutStartAudioOut`, dedicated audio thread that drains the ring buffer and submits via `audoutAppendAudioOutBuffer` + `audoutWaitPlayFinish`. **Both calls are wrapped: a non-zero return code sets a `g_audio_failed` atomic flag, drains the ring, and exits the thread cleanly.**
- Modify: `source/i_sound_switch.c` — `DG_sound_module->Init` returns false on backend init failure; doomgeneric falls back to silent. Also: `DG_sound_module->Update` checks `g_audio_failed` each call and short-circuits to no-op if set (so the engine doesn't keep mixing into a doomed ring).
- Modify: `source/main.cpp` — emit a one-time `tsl::notification` toast when audio init fails OR when the audio thread sets `g_audio_failed` due to a submit-time failure. The toast text is the same in both cases ("Audio unavailable — game continues silent").

**Key Decisions / Notes:**

- Audio thread: libnx `Thread` at priority 0x2c, 4 KB stack, mirrors Tetris-Overlay's pattern (consult `lib/libultrahand` for current libtesla audio examples; the deleted Tetris reference used the same approach).
- Ring buffer: 4 × 1024-frame chunks of 16-bit stereo at 22050 Hz ≈ 16 KB total. Single-producer / single-consumer — atomic head/tail indices, no mutex needed.
- Sleep/wake: register an `AppletHookCookie` for resume events; on resume, drop the audio ring and reinit `audoutStartAudioOut`. Prevents the sys-tune-style desync. If reinit fails, follow the same `g_audio_failed` path as a submit-time failure.
- Output sample rate: 22050 Hz mono → upsample to 48000 Hz stereo if the audout service requires it (libnx may negotiate). Confirm during implementation.
- Coexistence test: with `super-mario-odyssey.nsp` running, summon overlay, expect either (a) audio plays alongside the game, (b) `audoutInitialize` fails and we silently fall back, or (c) init succeeds but a later submit fails because the game grabs the device — also handled by the silent-fallback path. All three are acceptable for v1; document which case held in the verify report.

**Definition of Done:**

- [ ] Pistol shot, monster grunt, level music are audible during normal gameplay (or silent-fallback toast appeared, exclusive case)
- [ ] Master/music volume settings (set via Task 11) take effect immediately
- [ ] Sleep/wake cycle: put Switch to sleep mid-level, wake, audio resumes cleanly (no buffer-stuck artifacts)
- [ ] **Init-time failure:** when `audoutInitialize` returns non-zero (forced via test fixture), toast appears once and gameplay continues without crash
- [ ] **Submit-time failure:** when `audoutAppendAudioOutBuffer` returns non-zero mid-gameplay (forced via test fixture: hold the device exclusively from another process after init), `g_audio_failed` is set, ring is drained, audio thread exits cleanly, toast appears once, engine continues at 35 Hz with no stalls
- [ ] `audoutExit` is called on overlay exit (no leaked service handles)
- [ ] Verify T8 (35 Hz hold) is unaffected when audio is silent-fallback — no measurable delta between the audio-on and audio-failed perflog runs

**Verify:**

- Manual hardware test. Verifier launches the overlay over both a foreground game and from Ultrahand-only (no game), confirms audio behavior in both cases.

---

### Task 10: Heap-too-small error screen + first-launch self-check

**Objective:** Read `g_heapSize` (or equivalent libtesla-exposed value) at overlay init; if below 8 MB, refuse to start the engine and show a clear `tsl::Gui` error screen with remediation instructions. Never auto-write `heap_size.bin`.
**Dependencies:** Task 7 (we need the engine init path to gate)
**Mapped Truths:** T5

**Files:**

- Create: `source/error_heap_gui.hpp` (the tsl::Gui that renders the error screen)
- Modify: `source/main.cpp` — `DoomOverlay::initServices` checks heap size BEFORE calling `doomgeneric_Create`. On low heap, instantiate `ErrorHeapGui` instead of `DoomGui`.

**Key Decisions / Notes:**

- Read heap via `svcGetInfo(InfoType_HeapRegionSize, ...)` or equivalent; libtesla may expose a helper.
- Error screen content (per PRD Flow 4):
  - Title: "Overlay Heap Too Small"
  - Three lines: "Open Ultrahand → Settings → Overlay Heap Size → set to 8 MB → close & reopen"
  - "What this means" expander: brief explanation of overlay heap pool
  - **"If 8 MB fails on your device" line:** tells the user that if Ultrahand reports failure when picking 8 MB (i.e., kernel pool can't grant it), they should open a GitHub issue at the project URL with their HOS version and device model — this is our v1 telemetry path for deciding whether to add adaptive mode in v1.1.
  - Quit button (B)
- Threshold: < 8 MB → show error. Exactly 8 MB → proceed. (8 MB is the budget total; at exactly 8 MB we're tight but fit.)
- We do **not** write `heap_size.bin`. Period.

**Definition of Done:**

- [ ] **At heap=4MB** (set heap_size.bin to 0x400000 OR boot HOS 21+ default): the error screen title `Heap Size Too Small` is visible on the physical device within 2 seconds of overlay launch; the engine never initializes (verified by absence of Freedoom title screen and absence of any new entries in `perflog.txt` from that session); the Switch produces no crash dump under `/atmosphere/crash_reports/`.
- [ ] **At heap=6MB:** same — error screen visible (since 6 MB < our 8 MB target); engine does not initialize.
- [ ] **At heap=8MB:** error screen does NOT appear; engine initializes; Freedoom title screen appears; perflog.txt accumulates frame-time entries.
- [ ] B button on the error screen exits cleanly back to Ultrahand (no crash dump, no hung process)
- [ ] Error screen text is readable at libtesla's default framebuffer size (since our `cfg::FramebufferWidth` is not yet customized at this stage in the launch sequence)
- [ ] **Positive crash-absence proof:** at every tested heap size (4/6/8), `ls /atmosphere/crash_reports/ | wc -l` after the test returns the same count as before the test (no new crash report)

**Verify:**

- Manual hardware test. Verifier runs the overlay at heap 4 MB, 6 MB, 8 MB and confirms each branch.

---

### Task 11: Settings menu + INI persistence + BYO-WAD detection

**Objective:** A `tsl::Gui` settings page accessible via Minus from in-game; lets the player change render scale, master/music volume, active WAD, and vibration toggle; persists to `/config/sx-doom-overlay/config.ini`.
**Dependencies:** Tasks 8 (input), 9 (audio)
**Mapped Truths:** T4, T6

**Files:**

- Create: `source/settings_gui.hpp` (`tsl::Gui` subclass with rows for each setting)
- Create: `source/config_ini.cpp` / `.hpp` (INI read/write — wraps libultra's parser if available, else hand-rolled)
- Modify: `source/main.cpp` — Minus button transitions Gui from `DoomGui` to `SettingsGui` and back; `SettingsGui::onHide` calls `config_ini_save` then `config_ini_apply` (re-creates layer if scale changed).
- Modify: `source/i_sound_switch.c` — reads volumes from a global updated by SettingsGui

**Key Decisions / Notes:**

- INI format (under `[doom-overlay]` section): `render_scale=2`, `master_volume=80`, `music_volume=80`, `active_wad=freedoom1.wad`, `vibration=1`.
- BYO-WAD detection: scan `/switch/.overlays/doom/` for `*.wad` files at SettingsGui construction. Populate the `Active WAD` row with the discovered list.
- Changing render scale on the fly: requires re-creating the libtesla layer. Easiest path: save the change to INI, toast "Restart overlay to apply", set a flag that the user must dismiss-and-resummon. (Live re-resize is brittle.)
- Volume changes apply immediately (read every `Update` call by the audio thread).
- WAD changes require process restart (engine doesn't support hot-swap); same toast as scale.
- Vibration: hook libnx HID rumble to weapon-fire events; off by default.

**Definition of Done:**

- [ ] Pressing Minus during gameplay opens the settings page; Minus or B closes it
- [ ] All four settings persist across overlay close/reopen
- [ ] Volume sliders apply in real time
- [ ] Render scale change shows the "restart to apply" toast
- [ ] Drop `doom1.wad` at `/switch/.overlays/doom/`, settings page shows it in the WAD dropdown
- [ ] Selecting a WAD writes to INI and shows the "restart to apply" toast
- [ ] Vibration on: pistol fire produces controller rumble; off: no rumble
- [ ] No INI-file-corruption on partial write (atomic write via temp + rename)

**Verify:**

- Manual hardware test. Verifier walks through each setting, confirms persistence by closing and reopening the overlay.

---

### Task 12: Save/load wiring + release packaging

**Objective:** Confirm Doom's native save/load works on the Switch, with `.dsg` files written to `/config/sx-doom-overlay/savegames/`. Build the release zip with bundled Freedoom 1 and a README.
**Dependencies:** Task 11 (need the settings menu for an end-to-end demo)
**Mapped Truths:** T3

**Files:**

- Modify: `source/doomgeneric_switch.c` — set Doom's save directory (via the engine's `M_SetConfigDir` or equivalent) to `/config/sx-doom-overlay/savegames/`; ensure the directory exists (`ult::createDirectory`).
- Create: `source/atomic_save.c` / `.h` — wraps the engine's save-write with `fwrite` to `*.dsg.tmp`, `fsync`, atomic `rename` to `*.dsg`. Hooked in by patching the engine's save path via `patches/0002-atomic-save.patch` (a 5-10 line redirect through our wrapper) OR by interposing at the file-descriptor level if doomgeneric's save code is too internal to redirect cleanly.
- Create: `tests/desktop/test_atomic_save.c` — desktop test that kills the process mid-write (or simulates it) and verifies the existing `.dsg` is untouched (only `.dsg.tmp` is incomplete).
- Create: `data/freedoom1.wad` (download in build; gitignored)
- Create: `data/LICENSE.freedoom` (BSD-3 license text)
- Create: `README.md` (end-user install/configure instructions, controls, the heap-bump procedure, BYO-WAD instructions, license attribution)
- Create: `scripts/dist.sh` (assembles the release zip: `sx-doom-overlay.ovl`, `data/freedoom1.wad`, `data/LICENSE.freedoom`, `README.md` into `dist/sx-doom-overlay-vX.Y.Z.zip` matching the PRD's SD-card layout)
- Modify: `Makefile` — `make dist` target.

**Key Decisions / Notes:**

- Save format is vanilla Chocolate Doom `.dsg` — downstream-compatible.
- Save directory creation: idempotent. Engine handles bad-save-file errors via its own UI.
- **Atomic write protects against sleep-mid-save:** if the Switch suspends between `fwrite` calls, the existing `.dsg` is untouched (only `.dsg.tmp` is incomplete). On resume, the next save attempt cleans up the `.tmp` file. Without this, a sleep at exactly the wrong moment truncates the user's save permanently.
- Release zip extracts to SD root. Layout mirrors PRD exactly.
- README must include the heap-bump procedure prominently (HOS 21+ users will see the error screen otherwise) and the explicit statement that we don't ship commercial DOOM1.WAD.

**Definition of Done:**

- [ ] Save in slot 3 mid-level, exit overlay, reopen, load slot 3 → state restored within ±1 tick
- [ ] `.dsg` files appear under `/config/sx-doom-overlay/savegames/`
- [ ] Save directory is auto-created on first save attempt if missing
- [ ] **Atomic-write desktop test:** simulate a mid-save process kill; verify previous `.dsg` is unchanged and only `.dsg.tmp` is left (which the next save will overwrite or clean up)
- [ ] `make dist` produces `dist/sx-doom-overlay-vX.Y.Z.zip`
- [ ] Extracting the zip to a clean SD card produces a working install (verified by manual extraction + cold launch)
- [ ] README covers: install, heap-bump, controls, settings, BYO-WAD, license, troubleshooting
- [ ] LICENSE.freedoom present and correct (BSD-3)

**Verify:**

- Manual hardware test (save round-trip).
- `make dist && unzip -l dist/sx-doom-overlay-*.zip | head -30` shows the expected SD-card layout.

---

## Open Questions

None at this stage — all major decisions are resolved either in the PRD or in Batch 2 above. Open questions surfaced during implementation should be filed against the relevant task as deviations.

## Deferred Ideas

- **NEON-accelerated blit** — scalar reference is fine for v1 budget; NEON kernels for `blit_doom_to_rgba4444` would shave headroom but aren't required.
- **OPL2 / OPL3 music synth** — Doom's MUS music sounds best with hardware FM synthesis. Software MIDI fallback is acceptable for v1; OPL emulation is a quality bump for v2.
- **Per-weapon haptic profiles** — vibration is a binary toggle in v1; per-weapon rumble patterns is a polish item.
- **Mid-overlay scale change** — currently requires close-and-reopen. Live re-resize would be smoother but is brittle (libtesla layer recreate path).
- **Multi-screen / docked-mode optimization** — v1 targets handheld undocked; docked-mode framebuffer scaling could be tuned later.
