# Session State — 2026-04-28

Snapshot for resuming after context clear or branch switch.

## Branches

```
main                       b2a0fb6
  └── full integrated build:
        SFX + OPL music + the contributor UI + savestate UX + QUICK SAVE/LOAD slot 7

feat/task-9b-ogg-music     9f4e73b  (rebased on top of main)
  └── adds:
        stb_vorbis OGG decoder
        DG_music_module → music_ogg_module (overrides OPL wiring)
        linear-interp resampler for non-48kHz OGGs
        diagnostic traces around post-decode crash
        last commit: "diag: bracket submit thread with iter counters around music paths"

feat/task-9-music          stale, merged to main, can delete
feat/integrate-ui-v2    stale, merged to main, can delete
feat/integrate-ui       stale, v1 attempt, can delete
```

## Open tasks

| # | Subject | Status |
|---|---------|--------|
| 60 | Delete dead code: doom_globals.hpp + doom_input.hpp + unused externs | pending |
| 61 | Make input keymap runtime-configurable (settings UI) | pending |
| 62 | Resume OGG debug after v2 integration lands | in_progress |
| 63 | Auto-quicksave on launch-combo close | pending |
| 64 | Custom WAD exit crash — investigate + harden iwadname path | pending |
| 65 | Make `make all` auto-apply patches | pending |

## Where OGG debug stands

Last hardware test (before this session's UI/integration work):

```
[536760] music_ogg: first decode ENTER want=1011 decoder=0x1e7d83e48
[536766] music_ogg: first decode RETURN got=1011
                                       ← previous build crashed here
```

stb_vorbis returned successfully (got=1011). Crash was downstream — either
in the resampler interp loop, the bus-mix loop, or the audoutAppendAudioOutBuffer
call. The current build (9f4e73b) has bracketed `submit iter=N: post-X` traces
around each step, so the next trace will localize the crash precisely.

## Latest hardware report

User got music playing on the OGG branch. Couldn't tell whether it was OGG
SC-55 quality vs. OPL fallback. Need trace.log to confirm `music_ogg: opened
sdmc:/.../d_<name>.ogg` lines are present. If yes, OGG decoder is fully alive
and the post-decode crash from earlier is resolved.

## Per-WAD layout

- WADs: `/roms/doom/` (primary) AND `/switch/sx-doom-overlay/` (legacy fallback)
- Saves: `<configdir>/.savegame/<iwadname>/doomsavN.dsg` — patches/0006 namespaces by IWAD
- Music OGGs: `/switch/sx-doom-overlay/music/d_<lumpname>.ogg` — flat, no subfolders
  (lump names are globally unique across WADs, so no per-WAD subdir needed)
- Savestate slot: 7 (Doom menu "Save Slot 8") — last slot, doesn't collide with manual saves

## Build invariants

- Heap: 8 MB overlay slider required (Ultrahand → Settings → System → Overlay Memory)
- WIDGET: USING_WIDGET_DIRECTIVE in Makefile (libtesla widget shows top-right)
- Audio: own audoutInitialize via `audio_backend_libnx.c`, drain thread + ring buffer
- Music: DG_music_module aliased to music_opl_module (main) or music_ogg_module (OGG branch)
- Patches: 8 total, applied via `./scripts/apply-patches.sh` BEFORE `make`. Foot-gun: `make all` doesn't auto-apply (task #65)

## What the contributor needs to do for a working build

```bash
git clone https://github.com/<your-org>/sx-doom-overlay.git
cd sx-doom-overlay
git submodule update --init --recursive
./scripts/apply-patches.sh    # critical — must run before make
./scripts/fetch-freedoom.sh   # optional — bundled IWAD
make clean
make
# push out/sx-doom-overlay.ovl to Switch
```

Set Ultrahand overlay memory to 8 MB on first launch. Drop a WAD at
`/switch/sx-doom-overlay/` or `/roms/doom/`.
