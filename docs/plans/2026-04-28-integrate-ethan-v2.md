# Plan: v2 integration of a contributor's UI onto OPL-having main

**Branch:** `feat/integrate-ui-v2` (off `main` @ `313c3da` — post-OPL merge)
**Status:** PENDING
**Type:** Feature
**Worktree:** No
**Approved:** Yes (user greenlit 2026-04-28)
**Supersedes:** `feat/integrate-ui` (v1, off pre-OPL main — now stale)

## Why v2

`main` was at `9cb4785` (SFX-only) when v1 of the integration was attempted. Then we merged `feat/task-9-music` to main, bringing the OPL music engine. Main is now at `313c3da`.

a contributor's branch (`contributor/feat/ui-frame`) is an **orphan** — never had OPL music. A naive merge of his branch onto new main would silently delete the entire `source/opl/` stack (12 files, ~6300 LoC of vendored chocolate-doom code) because his branch has no concept of those files existing.

V1 was easy because main had nothing valuable for him to inadvertently delete. V2 is harder because we now have a working music engine to preserve.

## Decisions per file (verified by actual diff inspection)

| File | Decision | Reason |
|---|---|---|
| `source/elm_ultradoomframe.hpp` | **take a contributor's (NEW file)** | The UI gem — animated DOOM title, footer, version label |
| `source/doom_globals.hpp` | take a contributor's, then **delete in followup** | Broken-by-design statics-in-header; currently unused. Take it for completeness, delete it post-merge |
| `source/doom_input.hpp` | take a contributor's, then **delete in followup** | Duplicate dead mapping table; conflicts with `input_map.hpp`. Same as above — take then drop |
| `source/main.cpp` | **take a contributor's UI rewrite, then ADD BACK** OPL-specific lines: drop `-nomusic` from `engine_argv`, add MIDI arena init if needed | UI rewrite is valuable; OPL wiring must survive |
| `source/i_sound_switch.c` | **keep main's** | Wired to `music_opl_module`; a contributor's stubs music. Reject a contributor's |
| `source/audio_backend_libnx.c` | **keep main's** | Has the music render bus mix. a contributor's strips it. Reject |
| `source/audio_glue.h`, `audio_lock.cpp` | **keep main's** | OPL-specific bridges. a contributor's branch never had these (deleted) — preserve |
| `source/audio_mixer.{c,h}` | **keep main's** | Minor refactor on a contributor's side; main's is tested with OPL |
| `source/doomgeneric_switch.c` | **keep main's** (do NOT replace with a contributor's `doomgeneric_nx.c`) | Has L/R bumper bugfix (`enqueue_key` with `eligible_tic`) that a contributor's regresses |
| `source/input_map.hpp` | **keep main's** (cycle-on-bumpers default) | Tested with OPL build. Remappability is a Task 11 feature, not a v2 concern |
| `source/blit.{cpp,hpp}` | **keep main's** | Pure comment-shrinkage diff on a contributor's side; same code |
| `source/stdio_stubs.c` | **keep main's** | Same 10 functions, same behavior; only formatting differs |
| `source/opl/*` (12 files) | **keep all of main's** | The OPL music engine. **CRITICAL — must NOT be removed** |

## Execution steps

```bash
# 1. Branch off the new (OPL-having) main
git checkout main          # 313c3da
git checkout -b feat/integrate-ui-v2

# 2. Take a contributor's UI files (the NEW additions only)
git checkout contributor/feat/ui-frame -- \
    source/elm_ultradoomframe.hpp \
    source/doom_globals.hpp \
    source/doom_input.hpp

# 3. Take a contributor's main.cpp wholesale (UI rewrite is valuable)
git checkout contributor/feat/ui-frame -- source/main.cpp

# 4. Surgically restore OPL bits in main.cpp:
#    - drop arg_nomusic[] declaration
#    - drop arg_nomusic from engine_argv
#    - add MIDI_InitArena() call if needed (check whether it's still in i_sound_switch.c init)

# 5. DO NOT touch any of these (left as main):
#    - source/audio_backend.h, audio_backend_libnx.c
#    - source/audio_glue.h, audio_lock.cpp
#    - source/audio_mixer.{c,h}
#    - source/blit.{cpp,hpp}
#    - source/i_sound_switch.c
#    - source/input_map.hpp
#    - source/doomgeneric_switch.c
#    - source/stdio_stubs.c
#    - source/opl/* (entire stack)

# 6. Build, fix any compile errors
make

# 7. Build clean → commit + push
git add source/
git commit -m "feat(ui): integrate a contributor's DoomOverlayFrame onto OPL-having main"
git push -u origin feat/integrate-ui-v2
```

## Risks / what-could-go-wrong

- **a contributor's main.cpp expects `doomgeneric_nx.c` symbols** — references `doomgeneric_switch_reanchor_clock` (which exists in main's `_switch.c` too) but might reference others that differ. Build will tell us. Likely fix is `extern` declarations for symbols main has under different names.
- **a contributor's main.cpp uses `g_lcd_grid` config** — that's new. Saves to `/config/doom/config.ini`. We need to make sure `kConfigDir` doesn't conflict with main's logging conventions.
- **a contributor's main.cpp uses `input_map.hpp::doom_input::dispatch`** — main's `input_map.hpp` exposes `dispatch` with the same signature. Should compile.
- **`stdio_stubs.c` is in main from OPL merge AND a contributor's branch has its own** — they're compatible, but if a contributor's main.cpp references stdio override behavior subtly differently, could cause runtime weirdness. Functionally identical from inspection; probably fine.

## Build-fix checklist (likely needed)

1. `APP_VERSION` macro — a contributor's `elm_ultradoomframe.hpp` references it; we already added a `#ifndef APP_VERSION #define APP_VERSION "0.1.0" #endif` fallback in v1. Re-apply.
2. Strip `-nomusic` from main.cpp engine_argv so the engine binds music_opl_module.
3. Verify `i_sound_switch.c` initializes the OPL music subsystem from its existing init path (not in main.cpp directly).

## Verification

- [ ] `make` produces `out/sx-doom-overlay.ovl` with no errors
- [ ] Push to Switch
- [ ] Boot the overlay → animated DOOM title shows on header
- [ ] Pick a WAD → engine inits, "Loading..." briefly, then game appears
- [ ] In-game music plays (OPL FM synth)
- [ ] SFX plays
- [ ] L/R bumpers cycle weapons (skip unowned)
- [ ] Plus opens menu, Minus opens settings, settings has LCD Grid toggle
- [ ] Quit cleanly via combo (no fatal)

## Post-merge follow-ups

In order:
1. **Cleanup PR**: delete `source/doom_globals.hpp`, `source/doom_input.hpp`, strip unused `extern int gamestate; gametic; demosequence;` from `main.cpp`. Could be a contributor's first PR through the new workflow — validates his collaborator setup.
2. **Update README.md**: WAD path now `/roms/doom` primary (with `/switch/sx-doom-overlay/` as legacy fallback), document the SC-55 OGG pack option (when OGG lands), the contributor added to credits.
3. **OGG resume**: rebase `feat/task-9b-ogg-music` onto post-v2 main; resume the post-decode crash investigation.
4. **Task 11 (Settings UI)**: includes runtime-configurable keymap (Controls page in ConfigGui).

## Naming convention going forward

- Code style: keep main's verbose comments. a contributor's terse-comment style is a stylistic choice; once one style dominates the codebase, drift is harder to manage. Prefer documented "why" over compact.
- File naming: stick with existing conventions. `doomgeneric_switch.c` (not `_nx.c`). `audio_*` prefix for the audio path. Headers `.hpp` when C++, `.h` when C.
- Action mappings (input_map.hpp): "action → button" semantic, with cycle-on-bumpers as default. Remappable via settings UI in Task 11.
