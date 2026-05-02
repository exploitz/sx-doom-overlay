# CLAUDE.md — guidance for AI assistants working in this repo

This file is loaded automatically by Claude Code (and similar tools) at the
start of every session. Read it before suggesting commands or paths.

## Project at a glance

`sx-doom-overlay` is a Nintendo Switch homebrew **overlay** (a libtesla/
libultrahand .ovl) that embeds the Doom engine (chocolate-doom via
doomgeneric). Cross-compiled with **devkitPro / devkitA64** to a single
`.ovl` binary that loads in-system on top of Atmosphère.

Branches: `main` is the canonical line. Feature branches land via merge or
cherry-pick. The project supports OPL FM music + OGG-streamed music
(per-WAD lookup), OPL fallback, savestates, and touch UI.

## ⛔ Cross-platform — DO NOT assume the user is on the same OS as this repo's primary author

Multiple developers work in this repo. **Match the user's environment;
do not push the other path.**

| User environment | Reality |
|---|---|
| Native Windows + devkitPro Updater install | devkitPro lives at `C:\devkitPro`. The installer adds `make`, `bash`, `git`, and the cross-toolchain to the system `PATH`, so **`make` works from any Windows shell** — PowerShell, cmd, Windows Terminal, or the bundled "devkitPro MSys2". The Makefile invokes `bash scripts/*.sh` internally, which resolves through that PATH. **DO NOT suggest WSL, `/opt/devkitpro`, `sudo apt`, or `/mnt/c/...` paths.** Repo path is `C:\Users\<name>\…` not `/mnt/c/Users/<name>/…`. |
| Linux native | devkitPro at `/opt/devkitpro`, set up via `scripts/install-devkitpro.sh` (Debian/Ubuntu only). Standard bash everywhere. |
| WSL2 (Ubuntu under Windows) | Linux conventions. Repo paths look like `/mnt/c/Users/<name>/dev/sx-doom-overlay/`. **DO NOT** assume this is the universal layout — Ethan's clone is at `D:\Users\ereid\Documents\Ethans-Claude-Projects\UltraDoom\sx-doom-overlay\` on native Windows, not under `/mnt/c/`. |
| macOS | devkitPro via dkp-pacman. Bash works. Same Makefile path as Linux. |

### How to know which environment you're in

Before running any platform-specific command, check at least one of:

```bash
uname -a                     # Linux / Darwin / MINGW64_NT (= MSys2 on Windows)
echo "$DEVKITPRO"            # set by all working installs; path tells you the OS
echo "$WSL_DISTRO_NAME"      # set only inside WSL
pwd                          # /mnt/c/... = WSL,  /c/... = MSys2,  /home/... = Linux,  C:\... = PowerShell
```

If the user's shell is PowerShell or cmd on native Windows, **`make`
itself still works** because devkitPro adds `make` and `bash` to PATH —
the Makefile dispatches to `bash scripts/*.sh` and that resolves through
PATH transparently. What you should NOT do:
  - Suggest `sudo apt …` (no apt on Windows)
  - Suggest `./scripts/foo.sh` invocation patterns where the leading `./`
    or shebang gets misinterpreted — call `bash scripts/foo.sh` instead
  - Suggest `wslpath`, `/mnt/c/`, or any other WSL-isms

If you can't tell from context — **ask the user**, don't guess. "Are you
on native Windows with devkitPro installed at `C:\devkitPro`, or in WSL/Linux?"

## Build invariants (all platforms)

- `DEVKITPRO` env var must be set. On Windows-native with the official
  installer this is set automatically by the installer. On Linux it's set
  by `/etc/profile.d/devkit-env.sh`.
- `make` from the repo root produces `out/sx-doom-overlay.ovl`.
- `make patches` applies `patches/*.patch` to `lib/doomgeneric/` (idempotent).
- `make clean` blows away `build/` and `out/`.
- Submodules: `git submodule update --init --recursive` after first clone.
- The Makefile invokes `bash scripts/*.sh` for patches and dist. This works
  in WSL, Linux, macOS, and **devkitPro's MSys2 shell on Windows**.
- Setup is split by platform — pick the right one when guiding a user:
  - **Windows native** → `scripts\install-devkitpro.ps1` (PowerShell;
    fetches latest installer from GitHub, runs interactively) OR
    `scripts\setup-windows.ps1` (full bootstrap: install + submodules +
    Freedoom fetch + first build, with `-Skip*` flags for partial runs).
  - **Linux / WSL2 (Debian/Ubuntu)** → `scripts/install-devkitpro.sh`
    (apt-based, requires sudo).
  - **macOS / other Linux** → manual via dkp-pacman per
    https://devkitpro.org/wiki/devkitPro_pacman.

  Never suggest the `.sh` install to a Windows user, never suggest the
  `.ps1` install to a Linux user. Each script bails early on the wrong
  platform, but pointing at the wrong one is still wasted time.

## Windows clones: CRLF gotcha (the doomgeneric patch failure)

If a Windows user reports `make` failing during `apply-patches.sh` with
"patch does not apply" or "submodule HEAD may have moved" — the actual
cause is almost always **CRLF line endings on the `.patch` files**. Git
on Windows defaults to `core.autocrlf=true`, which rewrites checked-out
text files (including `*.patch`) with Windows line endings; `git apply`
then sees CR mismatches against the LF-checked-out source and refuses.

This was Ethan's original blocker. The repo now ships:
  - `.gitattributes` enforcing `eol=lf` on `*.patch`, `*.sh`, Makefile,
    and source files (so future clones don't repeat the problem).
  - `scripts/apply-patches.sh` pipes each patch through `tr -d '\r'`
    before `git apply` (so existing dirty clones still work).

**Existing clone showing the bug:** run once to re-normalize the
working tree:
```
git rm --cached -r .
git reset --hard
```
Or to fix only the patch files in place without touching anything else:
```
dos2unix patches/*.patch     # if dos2unix is available
# or:
git ls-files -z patches/*.patch | xargs -0 -I{} sh -c 'tr -d "\r" < {} > {}.tmp && mv {}.tmp {}'
```

## Common Claude failure modes to avoid

1. **Telling a Windows-native user to "open WSL and run `make`".** They don't
   have WSL installed and don't need it. devkitPro on Windows puts `make`
   and `bash` directly on `PATH`, so `make` works from PowerShell, cmd,
   Windows Terminal, or the devkitPro MSys2 shell — pick whichever they
   already have open.
2. **Suggesting `/opt/devkitpro` paths to Windows-native users.** Their
   install is at `C:\devkitPro` (= `/c/devkitPro/` in MSys2).
3. **Pasting `/mnt/c/...` paths into instructions.** That's a WSL-only
   prefix. On native Windows it's just `C:\Users\...`.
4. **Suggesting `sudo apt install …`.** Only works on Debian/Ubuntu/WSL.
   Native Windows has no apt.
5. **Assuming `make all` re-applies patches automatically.** It does (via
   `make patches` dependency), but if a user reports "I ran `make all`
   and got no music," the actual diagnosis is almost always **stale .ovl
   on the SD card**, not a missing patch step. The bottom of the in-game
   UI now shows `build: <branch>@<hash>+ audio: OGG/OPL` — have the user
   confirm the hash matches `git rev-parse --short HEAD` before assuming
   the source is wrong.

## Repo conventions

- `data/wads/` — drop your `.wad` files here (gitignored). Both the
  build and the music-extract script default to scanning this folder.
- `data/music/` — OGG music packs (gitignored). Per-WAD subdirs:
  `data/music/doom/`, `data/music/chex/`, etc.
- `scripts/extract-wad-music.py` — Python 3, no pip deps. Runs anywhere.
- `scripts/fetch-chex-music.sh`, `fetch-freedoom.sh` — bash. Runnable in
  MSys2 / WSL / Linux / macOS. Use `python urllib` not `wget`/`curl` so
  they work with the bare devkitPro MSys2 install (which has bash but
  not always wget).
- Patches in `patches/0001..0010-*.patch` apply on top of
  `lib/doomgeneric/` after submodule init. `make patches` is idempotent.

## Build identity in the running overlay

Every built `.ovl` embeds its git branch + short hash via
`-DBUILD_ID=<branch>@<hash>[+]`. The bottom of the in-game viewport
shows `build: main@a1b2c3d+ audio: OGG`. Use this to verify the binary
on a Switch matches the source — most "the change isn't there" reports
are stale `.ovl` on SD, not bad code.

## When the user says they're stuck on a build error

First questions to ask:

1. What does `echo $DEVKITPRO` (bash) or `echo %DEVKITPRO%` (cmd) /
   `$env:DEVKITPRO` (PowerShell) print?
2. Which shell are you in — PowerShell, cmd, devkitPro MSys2, WSL, Linux?
3. Is `lib/doomgeneric/` populated? (`ls lib/doomgeneric/doomgeneric/`
   should list `d_main.c` etc — if empty, run `git submodule update --init --recursive`)

Don't jump straight to "install WSL" unless they're already on WSL.
