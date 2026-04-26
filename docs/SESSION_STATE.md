# Session State — 2026-04-25 (end of long session 1)

Snapshot for resuming after a context clear. **Read this first.**

## TL;DR

Doom is **running on a real Switch** inside an Ultrahand overlay. E1M1
loads, animations play, no crashes. Task 7 (engine integration) and
Task 8 (input mapping) are both code-complete; Task 8 wasn't hardware-
verified yet but built and pushed. Outstanding: input testing, audio
(Task 9), heap-too-small toast (Task 10), settings UI (Task 11), save/
load + release polish (Task 12).

## What works (verified on hardware)

- **Build → push → run pipeline.** `make` produces `.ovl`, `make dist`
  produces release zip. `scripts/sync-sd.sh` auto-detects the Switch's
  SD card via either `/mnt/<letter>/` (Hekate UMS / card reader) OR
  PowerShell + Shell COM API (DBI/MTP — Switch in Explorer as
  `This PC\Nintendo Switch\SD Card\...`). Pushes new `.ovl` and pulls
  trace.log + crash reports into `diagnostics/<timestamp>/`.
- **Overlay loads cleanly.** No nx-ovlloader fault on launch.
- **Engine boots E1M1 directly via `-warp 1 1 -skill 2`.** Title screen
  + demo replay path **CRASHES** Atmosphère; bypassing it via -warp
  works fine. `D_DoomMain`'s `autostart=true` path through P_SetupLevel
  is solid; the crash was specific to the `G_DeferedPlayDemo →
  G_DoPlayDemo` chain.
- **Engine ticks at ~35 Hz** clean through hundreds of frames per
  trace.log (latest run: tick 199 with no crashes).
- **Rendering works.** Block-linear FB writes via libtesla
  `Renderer::setPixel`. Palette LUT built once at init from
  `colors[256]` (BGRA byte order; doomgeneric `struct color` has
  bit-fields `b:8, g:8, r:8, a:8` which is BGRA on little-endian ARM).
  RGBA4444 packing: r=bits 0-3, g=4-7, b=8-11, a=12-15.
- **patches/0001 + patches/0002 apply cleanly** via
  `scripts/apply-patches.sh`. 0001 lowers Doom MIN_RAM to 3 MiB. 0002
  replaces 5 `exit()` sites in `i_system.c` with
  `longjmp(g_doom_error_jmp, code)` so engine errors don't terminate
  the entire nx-ovlloader sysmodule.

## What's known-broken / deferred

- **Demo replay crashes Atmosphère**. After ~5 sec at title screen,
  `D_AdvanceDemo → G_DeferedPlayDemo("DEMO1") → G_DoPlayDemo` hits a
  fault somewhere. v1 mitigation: pass `-warp 1 1` so the engine never
  enters the demo-replay path. Real fix needs CrashLogger sysmodule
  (https://github.com/p-sam/switch-crashlogger) installed on the user's
  Switch — produces readable `.log` files in `/atmosphere/crash_reports/`
  with stack trace, register dump, faulting PC. **Not yet installed.**
- **Sticks not yet wired** — only D-pad + buttons in input_map.hpp.
  Adding stick→movement is straightforward; deferred.
- **Audio** is `-nosound -nomusic` argv-disabled. Task 9 unimplemented.
- **Heap-too-small toast** (Task 10) unimplemented; relies on user
  having 8 MB heap set in Ultrahand Settings.
- **Settings UI** (Task 11) unimplemented.
- **Save/load** (Task 12) unimplemented; engine paths exist but not
  wired to overlay UI.

## Repo state

12 commits this session (rough chain):
- Tasks 1-4 complete (bootstrap, desktop engine smoke, palette+blit
  module, audio mixer skeleton)
- Plan v2 + PRD v2 (research v2 findings folded in)
- Task 7 prep (patches/0002 — 5 exit()→longjmp conversions, validated)
- Task 5 complete (cross-build smoke; build system fully wired)
- Task 7 minimal viable engine integration on hardware
- Color literal fix (0x000F red → 0xF000 black for fillScreen)
- Block-linear FB swizzle fix (route blits through Renderer::setPixel)
- -warp 1 1 -skill 2 to bypass demo-replay crash
- MTP sync pipeline (sync-sd.sh + mtp-sync.ps1)
- Task 8 input mapping (D-pad + buttons → Doom keys; sticks deferred)

Latest commit: `5baa13b` (Task 8). Working tree clean except for
.nvmrc (pre-existing untracked) and `diagnostics/` (gitignore TODO).

## Lessons learned (5 expensive ones)

1. **libtesla framebuffer is block-linear (Tegra GPU swizzled).**
   `cfg::FramebufferWidth/Height` overrides do NOT recompute the
   swizzle constants. Stay on default 448×720; route writes through
   `Renderer::setPixel` (correct swizzle by construction). Custom
   non-default sizes need libtesla "windowed" mode, not yet
   investigated.
2. **`__nx_main_thread_stack_size` is a CONFIG SYMBOL, not an
   override target.** libnx default 1 MiB; declaring `extern const u32`
   with a smaller value SHRINKS the stack. Don't touch.
3. **RGBA4444 + BGRA struct read both need consistent endianness.**
   libtesla wants r in bits 0-3, a in bits 12-15. Doom's
   `struct color {b:8;g:8;r:8;a:8}` on little-endian = BGRA byte order.
   Both must match or you get pink-tint corruption + memory writes
   to wrong addresses.
4. **`-mb 6` is too tight in 8 MB heap.** Doom's `AutoAllocMemory`
   I_Errors immediately if `malloc(6 MiB)` fails. -mb 4 is the working
   value with libtesla framebuffer + libstdc++ overhead.
5. **nx-ovlloader heap state is NOT cleanly reset on overlay crash.**
   Every subsequent overlay launch fast-fails at `Z_Init` until a
   hard reboot of the Switch. There's no shortcut. Always full power
   cycle between diagnostic tests.

## How to resume next session

1. **Read this file first.** Then read
   `docs/plans/2026-04-25-doom-overlay.md` (Plan v2) and
   `docs/prd/2026-04-25-doom-overlay.md` (PRD v2) for full context.
2. Check `git log --oneline | head -20` for recent commits.
3. Current branch is `main`; everything committed.
4. To rebuild:
   ```bash
   export DEVKITPRO=/opt/devkitpro
   export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
   make
   ```
5. To deploy: `bash scripts/sync-sd.sh` (auto-detects MTP vs UMS).
6. Test plan: hardware test Task 8 (does input actually move Doomguy
   on E1M1?), then Task 9 (audio), then Tasks 10-12 in plan order.

## Outstanding to-do

- Hardware-test Task 8 input — confirm D-pad moves player, A fires
- Add stick handling (left = movement, right = look) once D-pad verified
- CrashLogger install + diagnose demo-replay crash for v1.1
- Tasks 9, 10, 11, 12 per Plan v2
- Add `diagnostics/` to .gitignore (or keep its `.gitkeep` and ignore
  contents) — currently bloats untracked
