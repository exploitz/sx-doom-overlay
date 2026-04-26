# Session State — 2026-04-25

Snapshot of project state at end of the first long working session.
Read this first when resuming.

## What works

- **Build pipeline end-to-end.** `make` produces `out/sx-doom-overlay.ovl`
  (~987 KB). `make dist` produces `dist/sx-doom-overlay-X.Y.Z.zip` with
  the correct SD-card layout (`/switch/.overlays/sx-doom-overlay.ovl`,
  `/switch/.overlays/doom/freedoom1.wad`, `LICENSE.freedoom`, README,
  top-level LICENSE).
- **Overlay loads on hardware.** No nx-ovlloader fault on launch.
  libtesla UI renders. Default 448×720 framebuffer; Doom drawn centered
  at 1× scale (320×200 in the middle of that buffer).
- **Engine boots and renders Freedoom 1 title screen.** doomgeneric_Create
  succeeds (~2.8 sec on hardware), 4-byte struct `colors[]` palette
  read works (BGRA byte order), RGBA4444 packing works (r=bits 0-3,
  a=bits 12-15), block-linear FB writes via `Renderer::setPixel`.
- **Engine runs cleanly through the title screen** for the entire
  `pagetic = 170` countdown (~5 seconds, 169 ticks).
- **patches/0001-lower-min-ram.patch + patches/0002-patch-exit-sites.patch
  apply cleanly via `scripts/apply-patches.sh`.** patches/0002 catches
  Doom's `I_Error` paths via setjmp/longjmp so engine errors don't
  terminate the entire nx-ovlloader sysmodule.
- **Tracing infrastructure.** `sdmc:/config/sx-doom-overlay/trace.log`
  receives per-tick log lines for ticks 0/35/70/105 plus every tick
  130-200, plus engine-init checkpoints and longjmp recovery codes.

## What's broken

- **Atmosphère fault at tick 169** (5 sec into engine run, on title-to-
  demo transition). Title sits for `pagetic=170` ticks then `D_AdvanceDemo`
  fires, which kicks `G_DeferedPlayDemo("DEMO1")` → on the very next
  tick `G_DoPlayDemo` runs `P_SetupLevel(E1M1)`. Crash is *during* tick
  169's `doomgeneric_Tick()` call (last log line is "tick 169 → calling
  Tick", no matching "Tick returned OK").
- **Subsequent loads fail-fast** with `longjmp received code=6` (I_Error
  during init, ~16ms into doomgeneric_Create). Resolved by hard-rebooting
  the Switch (cold restart of nx-ovlloader's heap state).

## Hypothesis for the crash

Most likely **memory corruption inside `P_SetupLevel`** — its initial
`Z_FreeTags(PU_LEVEL, PU_PURGELEVEL-1)` runs on a never-freed-before
zone, plus the chain of ~12 `Z_Malloc(PU_LEVEL)` calls for vertices,
segs, sectors, sides, lines, blockmap etc., plus `W_CacheLumpNum(PU_STATIC)`
for level lumps. Any one of those silently corrupting zone metadata
would lead to a SIGSEGV that bypasses our `I_Error` patch.

**Without a stack trace** (no CrashLogger sysmodule installed yet) we
can't pin down which specific call. Every guess costs a hardware
roundtrip.

## Two ways to break out next session

### A — Install CrashLogger sysmodule

https://github.com/p-sam/switch-crashlogger — drop-in sysmodule that
writes human-readable `.log` files to `/atmosphere/crash_reports/`.
Next crash gives us:
- Faulting instruction's program counter
- Register state at fault
- Backtrace through the call chain
- Decoded function names if symbols are present

**One run after install = exact diagnosis.** Highest-leverage move.

### B — Patch out the demo loop

Quick `patches/0003-disable-demo-loop.patch` to make `D_AdvanceDemo`
a no-op. Title screen sits forever, no demo init, no crash at tick 169.
Result: stable "Freedoom title on Switch" build that proves the entire
architecture works end-to-end. Then proceed to Task 8 (input mapping),
which lets the user press Start → New Game on the menu, which goes
through *a different* `P_SetupLevel` path that may or may not also
crash. If it does, we're back to needing CrashLogger.

## Build state

- Latest commit: see `git log`. Recent commits:
  - Task 7 minimal viable engine integration
  - Multiple iteration commits with revert-and-fix cycles
- `out/sx-doom-overlay.ovl` is current with all latest source.
- `dist/sx-doom-overlay-0.0.1-bootstrap.zip` is current.

## Open task list

- Task 7 (engine integration): **in progress, blocked on crash**.
- Tasks 8-12: pending, all depend on Task 7 stable.

## Lessons learned this session

1. **The libtesla framebuffer is block-linear** (Tegra GPU swizzled).
   Setting `cfg::FramebufferWidth/Height` to non-default values does
   NOT recompute the swizzle constants — pixels past x=447 land at
   wrong addresses. **Stay on the default 448×720 framebuffer; draw
   inside it.** Only way to support 2×/3× scale is libtesla's
   "windowed" mode or a custom swizzle replacement.
2. **`__nx_main_thread_stack_size` is a CONFIG SYMBOL, not an override
   target** — libnx default is 1 MiB; declaring `extern const u32`
   with a smaller value SHRINKS it. Don't touch unless you're growing.
3. **Both `pack_rgba4444` color literals AND struct color reads need
   consistent bit ordering**. libtesla expects bits 0-3=r, 12-15=a.
   Doom's `struct color` is bit-field `b:8, g:8, r:8, a:8` = BGRA byte
   order on little-endian.
4. **`-mb 6` is too tight in 8 MB heap** — `malloc(6MB)` fails when
   libstdc++/libtesla overhead is in play. `-mb 4` is the working
   value.
5. **nx-ovlloader's heap state is NOT cleanly reset on overlay crash**.
   After any atmosphère fault, every subsequent overlay launch fails
   fast at `Z_Init`/`I_Error`. **Hard reboot the Switch** between each
   diagnostic test — there's no shortcut.
