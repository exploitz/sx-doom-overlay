---
name: Doom Overlay
description: PRD for a libultrahand-based Nintendo Switch overlay that runs Doom (doomgeneric engine, Freedoom 1 bundled, audio, save/load, runtime scale).
type: prd
---

# Doom Overlay

Created: 2026-04-25
Author: gober.chase@gmail.com
Category: Feature
Status: Draft
Research: Deep

## Problem Statement

Nintendo Switch homebrew has standalone Doom **app** (`.nro`) ports, but no Doom that runs *inside an overlay* — the small libtesla/libultrahand window that floats over a running game and is summoned with a hotkey. The closest precedents are `Tetris-Overlay` (the only published in-overlay game) and `UltraGB-Overlay` (the only published in-overlay full-screen pixel game with runtime windowed scaling). Doom is the canonical "can it run Doom" candidate and conspicuously absent from the overlay ecosystem. Building it is both a technically interesting forcing function — proving the libultrahand framework can host a real game engine in the 8 MB applet-heap envelope — and a feature people will actually use, since the overlay model lets a player tab into Doom from any other game without a system reboot or app launch.

The problem we're solving is twofold:
1. **No in-overlay Doom exists.** Players who want Doom on Switch today must exit their current game, launch a separate `.nro` app, and lose the foreground game's state.
2. **The libultrahand framework's compute envelope is undocumented for action games.** Tetris-Overlay uses ~600 `drawRect` calls per frame on a 10×20 board; UltraGB-Overlay handles a 160×144 emulated framebuffer. Neither answers whether a 320×200 software-rendered FPS at 35 Hz with audio is feasible inside the overlay's 8 MB heap. A working Doom Overlay establishes that envelope and unlocks more ambitious overlay games downstream.

The target hardware floor is **OG Switch (Erista, Tegra X1, handheld mode)** — no Switch 2 RAM/CPU headroom, no Mariko-only assumptions. If it runs there, it runs everywhere.

## Core User Flows

### Flow 1: First-time install and launch

1. User downloads `sx-doom-overlay-vX.Y.Z.zip` from the project's GitHub releases.
2. User extracts the zip to the root of their SD card. Contents land at:
   - `/switch/.overlays/sx-doom-overlay.ovl` — the overlay binary
   - `/switch/.overlays/doom/freedoom1.wad` — bundled Freedoom Phase 1 IWAD
   - `/switch/.overlays/doom/LICENSE.freedoom` — BSD-3 attribution
3. User boots Switch, opens any game, summons Ultrahand with the configured hotkey (default `ZL+ZR+DDOWN`).
4. User scrolls to "Doom Overlay" in the launcher and presses A.
5. The overlay starts. On first launch the overlay self-checks `g_heapSize`:
   - If heap < 8 MB (typical on HOS 21+ where default is 4 MB), the overlay shows a one-screen `tsl::Gui` with the exact remediation step: *"Open Ultrahand → Settings → Overlay Heap Size → set to 8 MB → close and reopen Doom Overlay."* No game starts; B exits.
   - If heap ≥ 8 MB, Doom initializes, loads `/switch/.overlays/doom/freedoom1.wad`, and shows the Doom title screen scaled at 2× (640×400) centered on the Switch screen.
6. User plays Freedoom 1, episode 1, map 1.

### Flow 2: Gameplay session

1. User summons the overlay mid-game-of-something-else, picks Doom from Ultrahand's menu.
2. The overlay framebuffer (640×400 by default) renders Doom at 35 Hz, centered on screen, scaled 2× from Doom's internal 320×200.
3. Input mapping:
   - Left stick: move/strafe
   - Right stick: turn/look
   - A: fire
   - B: use / open door
   - X: alt-fire / next weapon
   - Y: previous weapon
   - L: strafe-left modifier
   - R: strafe-right modifier
   - ZL: run modifier
   - ZR: fire (alternate, for trigger-finger comfort)
   - D-pad: menu navigation in Doom's own menus
   - Plus: pause and open Doom's in-game menu
   - Minus: opens the overlay's own settings page (scale, volume, etc.)
4. Audio plays through libnx `audout`. Sound effects and music coexist with the foreground game's audio if HOS allows; if `audoutInitialize` fails (foreground app holds the service exclusively), the overlay continues silently and shows a one-time toast.
5. User can dismiss the overlay at any time with the Ultrahand close hotkey. Doom **pauses automatically** (no save needed) — overlay state is held in memory until either (a) the user re-summons the overlay (resumes from the same tick), or (b) the loader unloads the overlay process (then state is lost unless saved).
6. User can save mid-game via Doom's in-game menu (Plus → Save Game) to one of 8 slots. Saves persist to `/config/sx-doom-overlay/savegames/doomsavN.dsg`.

### Flow 3: Power-user settings and BYO WAD

1. User presses Minus from the in-game view to open the overlay's own settings page (separate from Doom's native menu).
2. Settings page (`tsl::Gui` modeled after UltraGB-Overlay's `m_scale_item`) exposes:
   - **Render scale:** 1× (320×200), 2× (640×400, default), 3× (960×600 — toast warns of heap pressure with Freedoom)
   - **Master volume:** 0–100% slider
   - **Music volume:** 0–100% slider
   - **Active WAD:** dropdown showing all `*.wad` files found at `/switch/.overlays/doom/`
   - **Vibration:** on/off (via libnx HID rumble for weapon fire, low priority)
3. Settings persist to `/config/sx-doom-overlay/config.ini` and reload on next overlay launch.
4. Power user drops their own copy of `doom1.wad` (commercial DOOM 1) or `doom2.wad` at `/switch/.overlays/doom/`. The settings page now lists both `freedoom1.wad` and `doom1.wad`. User picks `doom1.wad`; setting persists. On next launch, Doom loads from the user-picked IWAD.

### Flow 4: Heap-too-small recovery (HOS 21+ default install)

1. User on HOS 21.0.0+ installs Doom Overlay. The system's default overlay heap is 4 MB.
2. User launches Doom Overlay.
3. The overlay's first-launch self-check sees `g_heapSize < 8 MB`. Doom does NOT initialize.
4. A `tsl::Gui` page renders with:
   - A clear title: "Heap Size Too Small"
   - Three lines of instruction (the remediation steps listed in Flow 1).
   - A "What this means" expander explaining that the overlay heap is shared between all overlays and is configured globally.
   - A "Quit" button (B) that exits cleanly.
5. User follows the instructions, returns, and proceeds with Flow 1 step 5+.

## Scope

### In Scope (v1)

- **Single-player Doom**, end-to-end playable through Freedoom Phase 1's full episode/map rotation.
- **Audio (Path A):** sound effects and music via libnx `audout`, ported from Chocolate Doom's `i_sound.c` / `s_sound.c` on top of doomgeneric. Falls back gracefully to silent on `audout` init failure.
- **Bundled Freedoom Phase 1** as the default IWAD, BSD-3 license attribution included.
- **Save/load** to 8 slots persisted under `/config/sx-doom-overlay/savegames/`.
- **Settings menu** (`tsl::Gui`) with render scale, volume, active WAD, vibration toggle. Settings persist to `/config/sx-doom-overlay/config.ini`.
- **BYO-WAD support:** auto-detect any `*.wad` placed at `/switch/.overlays/doom/`; selectable via the settings menu. Freedoom is the bundled default.
- **Heap-too-small error screen:** graceful first-launch detection of insufficient overlay heap; clear remediation UI; no auto-write of `heap_size.bin`.
- **Runtime render scale 1× / 2× / 3×** (640×400 default), modeled directly on UltraGB-Overlay's `g_win_scale` runtime FB-resize pattern — `cfg::FramebufferWidth = 320 * scale`, `cfg::FramebufferHeight = 200 * scale`.
- **Native libtesla input** mapping (D-pad, A/B/X/Y, L/R, ZL/ZR, sticks, +/−).
- **Pause-on-overlay-dismiss:** when the user closes the overlay (B / Ultrahand hotkey), Doom is held in memory and resumes on next open — no auto-save required for a single session.

### Explicitly Out of Scope (v1)

- **Multiplayer / netplay** — Doom 1's IPX/serial LAN play. Out for v1: no Switch overlay netplay precedent, large surface area, no clear demand. Possible v2.
- **Demo recording / playback** (`.lmp` files) — Out for v1: low user value relative to engineering cost.
- **Mod loading: PWADs, DEHs, MAPINFO** — Out for v1: requires file picker UX and patch management; risks heap budget. Possible v2 once the core engine is stable on hardware.
- **GZDoom / ZDoom feature set** (slopes, scripting, mouselook beyond vanilla) — Out: heavier engines incompatible with the 8 MB heap.
- **Bundling commercial `DOOM1.WAD`** — Out for legal reasons. Shareware redistribution is conditional on shipping the unmodified original shareware archive, not the extracted WAD alone. Users who want commercial Doom drop their own WAD via BYO-WAD.
- **Auto-writing `/config/nx-ovlloader/heap_size.bin`** — Out: nx-ovlloader `fatalThrow`s if the requested heap can't be granted, which would brick the user's overlay sysmodule until manual SD-card recovery. We instead detect at runtime and instruct the user.
- **Switch 2 / SD card emulator-specific optimizations** — Out: target floor is OG Switch (Erista) handheld undocked.
- **Vibration as a major feature** — Optional toggle is in v1, but rumble-on-fire is the only effect; no per-weapon haptic profiles.

## Technical Context

This section is intentionally just enough for `/spec` to produce a sound implementation plan. It is not a full technical design — that's `/spec`'s job.

### Foundation: Engine + Framework

- **Engine:** [`doomgeneric`](https://github.com/ozkl/doomgeneric) — a Chocolate Doom fork with the SDL/audio/video layer abstracted into a six-function platform shim (`DG_Init`, `DG_DrawFrame`, `DG_SleepMs`, `DG_GetTicksMs`, `DG_GetKey`, `DG_SetWindowTitle`). Pure C99, GPLv2+. Default zone heap 6 MiB, tunable via `-mb` argv.
- **Framework:** [`libultrahand`](https://github.com/ppkantorski/libultrahand) — provides the `tsl::Overlay` / `tsl::Gui` / `tsl::elm::Element` skeleton, libtesla rendering primitives, and `libultra` utilities (`<ultra.hpp>`).
- **Loader:** [`nx-ovlloader`](https://github.com/ppkantorski/nx-ovlloader) (Ultrahand fork) — provides the dynamic heap-size mechanism we depend on.
- **Toolchain:** devkitPro / devkitA64, libnx, ARM64. Build flags follow Tetris-Overlay's lead: `-std=c++26 -fno-exceptions -fno-rtti -flto=6 -Wl,-wrap,__cxa_throw`, plus `USE_EXCEPTION_WRAP=1`.

### Memory Budget (8 MB ceiling)

| Component | Size |
|---|---|
| libtesla framebuffer (640×400 RGBA4444 × 2) | 1.0 MB |
| Doom zone allocator (`-mb 4`) | 4.0 MB |
| Doom static data (R_, P_, S_, sprite cache, lump tables) | 1.0 MB |
| Audio ring buffer + DMX music synth state | 0.5 MB |
| libstdc++ runtime + libtesla overhead | 0.5 MB |
| Fragmentation / safety margin | 1.0 MB |
| **Total** | **8.0 MB** |

At 3× render scale, framebuffer cost rises to 2.3 MB; the zone allocator must drop to `-mb 3` to compensate. Freedoom 1 may thrash under that combination; commercial `DOOM1.WAD` is comfortable.

### WAD Loading Model (the ~29 MB WAD vs. 8 MB heap)

Freedoom 1 is ~28.8 MB (v0.13.0; was ~12 MB in earlier releases) on disk, which does *not* need to fit in the 8 MB heap. Chocolate Doom (and therefore doomgeneric) follow the original 1993 Doom design:

- `W_AddFile()` opens the WAD via `fopen` and reads only the **lump directory** (~50–100 KB index of all lumps with their offsets and sizes). The WAD file handle stays open during gameplay; the WAD body stays on SD card.
- When the engine needs a lump (texture, sprite, level geometry, sound), `W_CacheLumpNum()` performs `fseek` + `fread` from the WAD into the zone allocator (`Z_Malloc(PU_CACHE)`).
- Zone-cached lumps are evictable when memory pressure grows. Level-critical data (`PU_STATIC`) stays until level end.
- Per-level working set is typically 1–2 MB — fits comfortably inside our 4 MB zone.
- **The trade-off is hitching** on level transitions and on first-encounter of sprites/sounds. Switch SD cards do 50–100 MB/s sequential and 10–30 MB/s random, so per-lump latency is a few ms; aggregate per-level load on the order of 50–100 ms (covered by the performance targets).

Vanilla Doom shipped on 386s with 4 MB RAM streaming from spinning hard disks. A 29-MB-WAD-on-SD model is the engine's intended design, not a workaround. **Side effect of the larger Freedoom size:** our release zip will weigh in around ~30 MB instead of ~15 MB. Download time on slow connections is roughly doubled. Worth noting in the release README.

### Heap Provisioning Strategy

- **nx-ovlloader is a sysmodule** (Title ID `420000000007E51A`, loaded by Atmosphère's `boot2`). It configures libnx to behave as a `LibraryApplet` for compositor purposes (so the overlay layer renders over a foreground game), but its process is a runtime sysmodule. Heap therefore comes from the **system memory pool reserved for sysmodules** — the same pool Ultrahand's UI labels "System Memory."
- The overlay (`.ovl` NRO) is mapped *into* nx-ovlloader's process address space, so it shares nx-ovlloader's heap.
- Heap is requested via `svcSetHeapSize`. On failure (kernel can't grant the requested size from the system pool) the loader `fatalThrow`s — there is no graceful fallback. Therefore we **never** auto-write `heap_size.bin`.
- The requested size is read from `/config/nx-ovlloader/heap_size.bin` (2 MB-aligned, ≠ 2 MB). Defaults the loader picks when the file is absent: 4 MB on HOS 21.0.0+, 6 MB on HOS 20.0.0+, 8 MB on older. These defaults are empirical safe values that don't `fatalThrow` on those HOS versions.
- Ultrahand's settings UI exposes 4 / 6 / 8 MB as preset values. We require **8 MB**.
- HOS 21+ users see our heap-too-small error screen on first launch and bump heap manually through Ultrahand's UI; this is one-time.

### Heap Ceiling Reality

The 8 MB target is not arbitrary; it's the practical ceiling we can rely on. The mechanism stack:

```
User UI / INI / heap_size.bin       ← request label (just a value on disk)
        ↓
nx-ovlloader (sysmodule process)    ← reads request from heap_size.bin
        ↓
svcSetHeapSize(value)               ← kernel decides: grant or fail
        ↓
System (sysmodule) memory pool      ← reserved by HOS kernel
        ↓
Switch physical RAM (4 GB; OG/Mariko alike)
```

Every "raise the heap" mechanism in the chain — Ultrahand's UI slider, the hidden `custom_overlay_memory_MB` INI override, even editing `heap_size.bin` directly — is just a *request* to the syscall. The kernel's system-pool ceiling is the actual gatekeeper, and it's set by HOS. There is no Atmosphère patch in scope that lifts that ceiling for *runtime sysmodules* like nx-ovlloader. (Mechanisms that *do* expand homebrew memory — "Title Takeover" / "FULL RAM mode" — only apply to foreground homebrew **applications**, not sysmodules. Atmosphère KIP memory tweaks only apply to KIPs, which nx-ovlloader is not. Erista vs. Mariko makes no difference here.)

**Conclusion:** the safe ceiling is whatever the loader's per-HOS default works at — 8 MB on older HOS, 4 MB on HOS 21+. We design for the lowest common denominator that's still playable: 8 MB, requested via Ultrahand's standard slider on the user's first launch. We do not depend on a patch we can't name.

### What we don't depend on

Per the [nx-ovlloader README](https://github.com/ppkantorski/nx-ovlloader), the documented, supported overlay heap sizes are exactly **4 / 6 / 8 MB**, written to `heap_size.bin` by Ultrahand's System Settings UI. The PRD designs to that contract. We do **not** depend on:

- `custom_overlay_memory_MB` (an undocumented Ultrahand INI hook that lets the slider expose values >8 MB) — the value can still `fatalThrow`, the mechanism isn't in the README, and behavior could change in any future Ultrahand release.
- "Title Takeover" / "FULL RAM" mode (only applies to foreground homebrew **applications**, not sysmodules).
- KIP memory tweaks (nx-ovlloader is not a KIP).
- Any other "Atmosphère memory expansion patch" referenced in homebrew folklore — none publicly-documented for runtime sysmodules.

If the user has any of those configured, the overlay still works fine — but the PRD doesn't promise anything more than 8 MB.

### Render Pipeline

- Doom renders into a 320×200 8-bit-paletted internal buffer (Chocolate Doom's normal output via `R_DrawColumn` / `R_DrawSpan`).
- The platform shim's `DG_DrawFrame` upscales the 320×200 frame by the active scale factor (1× / 2× / 3×) into the libtesla framebuffer (RGBA4444). Upscale is plain integer nearest-neighbor with palette lookup; NEON-friendly for 2× and 3× kernels.
- libtesla's framebuffer dimensions are runtime-set via `cfg::FramebufferWidth` / `cfg::FramebufferHeight` before layer creation. Pattern lifted directly from UltraGB-Overlay (`source/main.cpp:2978`: `ult::DefaultFramebufferWidth = static_cast<u32>(GB_W * g_win_scale)`).
- libtesla composites the framebuffer onto the Switch screen at vsync (60 Hz). The overlay's `update()` callback drives a fixed 35 Hz Doom tick accumulator; render-only ticks fire on the libtesla cadence.
- We do **not** use libtesla's per-element `drawRect` API for game pixels (that's how Tetris-Overlay does it; it does not scale). Custom blit primitive modeled on `libtesla/include/tesla.hpp:2685` (`Renderer::drawWallpaper`) — pushes a pre-rendered pixel buffer straight into libtesla's surface.

### Audio Pipeline (Path A)

- Init: `audoutInitialize()` → request a 16-bit stereo stream at 22050 Hz (Doom's native sound rate).
- If `audoutInitialize` returns `LibnxError_BadInput` or similar, we fall back to silent operation and toast the user once.
- Sfx: Chocolate Doom's `i_sound.c` mixer ported on top of doomgeneric. Up to 8 channels, summed into a 22050 Hz stereo PCM ring buffer (~256 KB).
- Music: Chocolate Doom's DMX MUS-to-MIDI fallback synth, output mixed into the same buffer.
- Output: a libnx audio thread drains the ring buffer at the system's preferred chunk size and submits via `audoutAppendAudioOutBuffer`.
- **Coexistence with foreground audio is unproven.** Sys-tune is the closest precedent (mp3/flac/wav over `audren`, not `audout`, plus a sysmodule split). If `audout` is exclusive on the user's HOS+game combo, we degrade silently.

### Frame Loop & Lifecycle

- Doom's tick rate is fixed 35 Hz. We accumulate elapsed time in libtesla's `update()` callback (called per libtesla composite, ~60 Hz) and run zero-or-more Doom `D_DoomLoop` ticks per `update()` call to maintain 35 Hz simulation regardless of libtesla's frame cadence.
- Render output happens once per `update()`: present the latest Doom-rendered 320×200 frame, scaled to the libtesla framebuffer.
- Overlay close → libtesla quietly stops calling `update()`. Doom state is held in memory; on re-summon, `update()` resumes and the simulation continues.
- Overlay process exit (loader unload) → all in-memory state is lost; the user must restore from a saved game.

### File Layout (SD card)

```
/switch/.overlays/
├── sx-doom-overlay.ovl                     ← the overlay binary
└── doom/
    ├── freedoom1.wad                       ← bundled (BSD-3)
    ├── LICENSE.freedoom                    ← required BSD-3 attribution
    ├── doom1.wad                           ← (optional, user-supplied)
    └── doom2.wad                           ← (optional, user-supplied)

/config/sx-doom-overlay/
├── config.ini                              ← scale, volume, active WAD, vibration
└── savegames/
    ├── doomsav0.dsg
    ├── doomsav1.dsg
    └── ...
```

### Project Layout (source repo)

The `Ultrahand-Overlay/` clone in the working directory is reference-only and is **not** the project root. The Doom Overlay is its own repo (`sx-doom-overlay`) modeled after Tetris-Overlay and UltraGB-Overlay:

```
sx-doom-overlay/
├── Makefile                              ← include lib/libultrahand/ultrahand.mk
├── source/
│   ├── main.cpp                          ← tsl::Overlay subclass, tsl::loop entry
│   ├── doom_gui.hpp                      ← in-game render Gui (libtesla)
│   ├── settings_gui.hpp                  ← settings tsl::Gui (scale/volume/WAD)
│   ├── error_heap_gui.hpp                ← heap-too-small first-launch screen
│   ├── doomgeneric_switch.c              ← the 6 DG_ platform-shim functions
│   ├── i_sound_switch.c                  ← ported Chocolate Doom audio mixer
│   └── input_map.hpp                     ← libnx HID → Doom keycode mapping
├── lib/
│   ├── libultrahand/                     ← git submodule
│   └── doomgeneric/                      ← git submodule (vendored upstream)
└── data/
    └── freedoom1.wad                     ← copied into release zip; not in romfs
```

doomgeneric is vendored as a git submodule; its `i_main.c::main()` is excluded from the build (we provide our own entry through libtesla's `tsl::loop`).

### Performance Targets (OG Switch, handheld, undocked)

- **Doom simulation:** 35 Hz fixed (vanilla Doom tick rate). Stretch goal: never miss a tick at 2× scale.
- **Composite cadence:** libtesla's native 60 Hz vsync (we don't control this).
- **First-frame after Ultrahand summon:** < 200 ms.
- **WAD load time (Freedoom 1, cold):** < 2 s on first launch; subsequent levels < 500 ms via lump caching.
- **Audio latency:** < 100 ms button-press to first sample (acceptable for Doom; not a rhythm game).

### Constraints (Hard)

- Target floor: **OG Switch (Erista, Tegra X1) handheld mode**. No GPU access, software framebuffer only, ARM64.
- Overlay heap: **8 MB target** (system-memory pool ceiling on a default Atmosphère install; loader `fatalThrow`s on overshoot).
- Build: `-fno-exceptions -fno-rtti` mandatory (libultrahand convention; doomgeneric is C, unaffected).
- License: GPLv2 (engine is GPLv2; libtesla and libultra are GPLv2; Freedoom is BSD-3, GPL-compatible).
- No bundled commercial WADs.

### Constraints (Soft / Conventions)

- File naming: `sx-doom-overlay.ovl` (project slug); paths under `/switch/.overlays/doom/` and `/config/sx-doom-overlay/`.
- Settings menu UX patterns mirror UltraGB-Overlay's `m_scale_item` / `save_win_scale` for familiarity.
- Save file format: vanilla Chocolate Doom `.dsg` — not invented; downstream-compatible if the user later moves saves to a desktop port.

### Known Risks

| Risk | Severity | Mitigation |
|---|---|---|
| Audio coexistence with foreground game's `audout` may fail on some HOS+game combos. | High | Detect `audoutInitialize` failure, run silent, toast once, document in README. |
| libtesla composite cost at 2× / 3× scale on Tegra X1 has no published benchmark. May force scale-down. | Medium | Profile early on real hardware; if 2× isn't 35 Hz stable, fall back to 1× default. |
| Freedoom 1 (~28.8 MB on disk for v0.13.0; was ~12 MB in earlier releases) thrashes the 4 MB Doom zone on bigger maps; visible hitching on level transitions. | Medium | Document in README; recommend BYO-WAD with shareware DOOM1.WAD for smoother play; tune lump cache replacement policy if needed. |
| nx-ovlloader `fatalThrow`s on `svcSetHeapSize` failure, so any auto-write of `heap_size.bin` is dangerous. | High (already mitigated) | Do not auto-write. Detect at runtime and instruct user. |
| HOS 21+ shipping with 4 MB default heap is a UX cliff for fresh installs. | Medium | First-launch heap-too-small error screen with exact remediation steps. |
| HOS major bumps regularly break libtesla. | Medium | Pin libultrahand submodule; CI-build against multiple HOS versions where possible. |
| Sleep/wake desync of audio (sys-tune precedent reports this). | Low | Hook libnx applet messages; on resume, drop the audio buffer and reinit `audout`. |
| Doom's input model assumes keyboard scancodes; we map libnx HID → Doom keycodes. Edge cases: rapid double-tap, simultaneous keys. | Low | Use Chocolate Doom's existing key-event queue verbatim; only the source of events changes. |

## Key Decisions

| Decision | Choice | Why |
|---|---|---|
| Engine | doomgeneric | Chocolate Doom under the hood with a clean six-function platform seam; pure C99; minimal porting surface. Chocolate Doom proper is too SDL-coupled. |
| Audio | Path A (in v1) | sys-tune proves overlays can produce audio. doomgeneric strips audio, so we re-port `i_sound.c` / `s_sound.c` on top — non-trivial but contained. User strongly preferred audio. Falls back to silent on init failure. |
| WAD | Bundle Freedoom 1; BYO-WAD override | Freedoom is BSD-3, GPL-compatible, redistributable. Commercial DOOM1.WAD is not safely bundleable. BYO is small extra surface and preserves user choice. |
| Display | Native-sized centered, runtime scale 1× / 2× / 3×; default 2× | Mirrors UltraGB-Overlay's proven pattern. Low default heap cost, user can scale up. Avoids the "wide overlay won't work" concern by *not* claiming the whole 1280×720 surface unless the user opts in. |
| Heap target | 8 MB (the maximum documented in the [nx-ovlloader README](https://github.com/ppkantorski/nx-ovlloader)) | nx-ovlloader is a sysmodule; its heap is allocated from the **system memory pool** reserved for sysmodules by HOS. The README documents heap sizes of exactly 4 / 6 / 8 MB — that's the supported contract, and we design to it. Anything above 8 MB depends on undocumented Ultrahand internals, requires user-side INI edits, and can still `fatalThrow` if the kernel pool can't grant it. No Atmosphère patch lifts this ceiling for runtime sysmodules (Title Takeover and KIP memory tweaks don't apply). 8 MB forces tight memory engineering but eliminates a class of "bricked overlay" bugs. |
| Heap-low handling | First-launch error screen, no auto-write | Auto-writing `heap_size.bin` to a value the system can't grant would `fatalThrow` nx-ovlloader. Instructions are safer than silent-magic. |
| Scope of v1 | Single-player, audio, save, settings, BYO-WAD | These are the minimum playable + power-user features. Multiplayer / demos / mod loading are explicitly v2 to keep the heap budget defensible. |
| Target floor | OG Switch (Erista) handheld undocked | If it runs there, it runs everywhere. Rules out Switch-2-or-Mariko-only assumptions in the perf budget. |
| Project layout | New repo `sx-doom-overlay` with libultrahand + doomgeneric submodules | Tetris-Overlay / UltraGB-Overlay both follow this pattern. Keeps the upstream engines tracked separately from our shim. The cloned `Ultrahand-Overlay/` in the working directory is reference-only. |

## Research Findings

The PRD is informed by four parallel deep-research passes; full reports are at:
- `/tmp/prd-research-overlay-constraints.md` (libtesla / nx-ovlloader runtime constraints)
- `/tmp/prd-research-tetris-architecture.md` (Tetris-Overlay implementation breakdown)
- `/tmp/prd-research-doom-port.md` (Doom engine selection)
- `/tmp/prd-research-prior-art.md` (Switch Doom history & WAD legality)

Key cross-cutting findings already incorporated into the body above:

- **Heap is allocated from the system memory pool** (nx-ovlloader is a sysmodule, not an applet — it just uses libnx's `AppletType_LibraryApplet` mode for layer compositing). HOS-tiered defaults (4/6/8 MB) are configured via `/config/nx-ovlloader/heap_size.bin` with no upper bound enforced by the loader source — but the syscall (`svcSetHeapSize`) does enforce a kernel-side ceiling, and overshoot triggers `fatalThrow`. No publicly-documented Atmosphère patch lifts that kernel ceiling for runtime sysmodules.
- **Audio is feasible in overlays** (sys-tune precedent via `audren`; we use `audout` per doomgeneric's expected interface), with the caveat that foreground-app coexistence is unproven on every HOS+game combo.
- **No prior Doom-as-overlay attempt exists**; this is genuinely first-of-kind. Tetris-Overlay and UltraGB-Overlay are the only two published overlay games.
- **Performance envelope is comfortable.** RP2040-Doom (dual Cortex-M0+ at 133 MHz) sustains 35 fps at 320×200; Tegra X1 is two orders of magnitude beyond that. The actual unknown is libtesla composite cost at scaled framebuffer sizes — to be measured on hardware.
- **Freedoom Phase 1 is BSD-3, redistributable in a GPLv2 package** with attribution. Commercial Doom WADs are not.
