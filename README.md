# sx-doom-overlay

[![platform](https://img.shields.io/badge/platform-Switch-898c8c)](https://switchbrew.org/wiki/Main_Page)
[![language](https://img.shields.io/badge/language-C++26%20%2F%20C99-ba1632)](https://github.com/topics/cpp)
[![license](https://img.shields.io/badge/license-GPLv2-189c11)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)

**Doom inside an Ultrahand overlay on the Nintendo Switch.** Pause your
foreground game, summon Ultrahand, pick "Doom Overlay," shoot some demons,
go back. Built on [doomgeneric](https://github.com/ozkl/doomgeneric)
(Chocolate Doom under the hood) +
[libultrahand](https://github.com/ppkantorski/libultrahand) +
[nx-ovlloader](https://github.com/ppkantorski/nx-ovlloader). Ships with
[Freedoom Phase 1](https://freedoom.github.io/) (BSD-3) so it plays out
of the box; bring your own DOOM1.WAD / DOOM2.WAD if you'd rather.

> **Status: playable, active development.** All 12 of the original
> implementation tasks are complete and the engine runs E1M1+ with
> SFX, OPL or OGG music, savestates, touch UI, and per-WAD music
> packs on real hardware. Cross-platform builds: native Windows,
> Linux, WSL2, macOS.
>
> Project tracking: [docs/plans/2026-04-25-doom-overlay.md](docs/plans/2026-04-25-doom-overlay.md).

---

## Install

### Prerequisites on your Switch

1. **Atmosphère + Ultrahand-Overlay** already installed and working.
   ([NH Switch Guide](https://switch.hacks.guide/) is the canonical setup
   walkthrough if you don't have a modded Switch yet.)
2. **Bump your overlay heap to 8 MB.** From the home screen, summon
   Ultrahand → Settings → System → Overlay Memory → 8 MB → close.
3. *(Strongly recommended for first-time users)*
   **Install [CrashLogger](https://github.com/p-sam/switch-crashlogger)**
   so any crash reports from the engine are human-readable instead of
   binary `.bin` files. One-time setup; you'll thank yourself.

### Drop the release zip

1. Download `sx-doom-overlay-vX.Y.Z.zip` from the
   [latest release](https://github.com/<your-org>/sx-doom-overlay/releases/latest).
2. Extract to the **root of your SD card**. The zip lays files out at
   the right paths automatically:

   ```
   /switch/.overlays/sx-doom-overlay.ovl
   /switch/.overlays/doom/freedoom1.wad
   /switch/.overlays/doom/LICENSE.freedoom
   README.md
   ```

3. Boot the Switch, summon Ultrahand (default `ZL+ZR+DDOWN`), pick
   **Doom Overlay** from the menu.

If the overlay opens but Doom never starts, see [Troubleshooting](#troubleshooting).

---

## Controls

| Button | Action |
|---|---|
| Left stick | Move / strafe |
| Right stick | Turn / look |
| A | Fire |
| B | Use / open door |
| X | Alt-fire / next weapon |
| Y | Previous weapon |
| L | Strafe-left modifier |
| R | Strafe-right modifier |
| ZL | Run modifier |
| ZR | Fire (alt for trigger comfort) |
| D-pad | Menu navigation in Doom's own menus |
| Plus | Pause / Doom in-game menu |
| Minus | Open Doom Overlay's settings page |

Customizable in `/config/sx-doom-overlay/config.ini` once you've launched
the overlay at least once. Default mapping above is what most Switch
homebrew Doom ports use; mess with it if it doesn't suit you.

---

## What's on screen

The overlay header shows the libtesla title bar with the animated DOOM
logo, the semver `0.1.0`, and your sysmon widget (clock / battery / CPU)
in the top-right (matches every other libtesla overlay).

Below the game viewport, a small debug line reports two live status fields:

```
newlib: 124k used / 392k free   doom zone: 1843k free
build: main@1b5a2d0+   audio: OGG
```

- `newlib:` and `doom zone:` are the two heap pools, refreshed once per
  second. Watch them during dense combat — if the doom zone drops below
  ~200 KB you're at risk of an `I_Error` zone-out-of-memory on the next
  level transition.
- `build:` is the git branch and short hash baked into the binary
  (passed via `-DBUILD_ID` at compile time). Use this to verify the
  `.ovl` on the SD matches the source you just built — most "my change
  isn't showing up" reports turn out to be a stale `.ovl`.
- `audio:` is the music backend currently driving output:
  - `OGG` — stb_vorbis decoding from your music pack on disk
  - `OPL` — FM-synth fallback for songs with no matching OGG file
  - `silent` — between songs (intermissions, menus)

The bottom row has touch buttons:

| Button | Action |
|---|---|
| **QUICK SAVE** | Saves to slot 7 (overwrites any existing slot 7 silently) |
| **QUICK LOAD** | Loads slot 7 |
| **Quit** | Closes the overlay (auto-saves to slot 7 on the way out if a level is loaded) |

Tap any button or use Ultrahand's launch combo to close the overlay.

---

## Bring your own WAD

`freedoom1.wad` is bundled, but you can also play with the commercial
DOOM and DOOM 2 WADs (or any compatible PWAD — though mod loading is
v2 territory). Drop them at `/switch/.overlays/doom/`:

```
/switch/.overlays/doom/freedoom1.wad   ← bundled, BSD-3
/switch/.overlays/doom/doom1.wad       ← shareware DOOM (your copy)
/switch/.overlays/doom/doom.wad        ← registered DOOM (your copy)
/switch/.overlays/doom/doom2.wad       ← DOOM II (your copy)
```

Pick the WAD from the in-overlay launcher list when you open the
overlay. We do **not** distribute commercial WADs — id Software's
redistribution terms apply to those.

---

## Music packs

The overlay's music backend has three layers, tried in order per song:

1. **Per-WAD OGG pack** at `/switch/sx-doom-overlay/music/<iwad-stem>/d_*.ogg` —
   if the WAD you're loading is `chex.wad`, the overlay looks here first.
   Use this when you have multiple WADs cached side-by-side.
2. **Flat OGG layout** at `/switch/sx-doom-overlay/music/d_*.ogg` —
   legacy / single-WAD setups where you don't bother with subdirs.
3. **OPL fallback** — if no matching OGG file exists, the engine
   FM-synthesizes the original MUS lump from the WAD via Nuked-OPL3.
   No download or file required; works for any WAD.

The `audio:` field in the on-screen debug line shows which layer is
active for the song that's currently playing.

**Where to get OGG packs:**

- **Doom 1+2, Plutonia, TNT** — Brandon Blume's
  [Roland SC-55 archive](https://archive.org/details/sc55-doom-music-pack)
  on archive.org (free).
- **Chex Quest** — `bash scripts/fetch-chex-music.sh` from the repo
  fetches Maxime Abbey's Arachno SoundFont pack (50 MB, free).
- **Anything else** — `python3 scripts/extract-wad-music.py --sf2 path/to/font.sf2`
  renders any WAD's MUS lumps to OGG using your SoundFont of choice.
  Output lands in `data/music/<wad-stem>/` ready to copy to the SD.

Layout on the SD when you have multiple packs:

```
/switch/sx-doom-overlay/music/doom/d_e1m1.ogg     ← SC-55 pack for DOOM/DOOM1
/switch/sx-doom-overlay/music/chex/d_e1m1.ogg     ← Arachno pack for CHEX
/switch/sx-doom-overlay/music/freedoom1/d_e1m1.ogg
```

The overlay reads the loaded WAD's filename, lowercases the stem, and
looks in the matching subdir — no manual selection needed.

---

## Save / Load

Standard Doom save slots, accessed via the in-game menu (Plus button).
Saves persist to `/config/sx-doom-overlay/savegames/doomsavN.dsg` and
are atomic-write protected — sleeping the Switch mid-save won't corrupt
your existing save file.

Saves are vanilla Chocolate Doom `.dsg` format, so they're portable to
any desktop chocolate-doom build.

---

## Troubleshooting

### "Heap Size Too Small" toast appears, Doom doesn't start
Your overlay heap is below 8 MB. Open Ultrahand → Settings → System →
Overlay Memory → 8 MB → close & reopen the overlay. (Default on HOS 21+
is 4 MB; you have to bump it manually. One-time fix.)

### Overlay opens but the game freezes / black screen
1. Check `/atmosphere/crash_reports/` for a `.log` file (requires
   CrashLogger sysmodule).
2. Read `/config/sx-doom-overlay/error.log` — engine errors land here.
3. Most likely cause: the active WAD is corrupted or unreadable. Try
   switching to `freedoom1.wad` from the settings page.

### No audio
Either:
- The foreground game holds `audout` exclusively → we degrade to silent
  with a one-time toast. Nothing wrong; this is by design.
- HOS 21+ permission quirk → if you have CrashLogger installed, check
  `/atmosphere/crash_reports/` for `audoutInitialize` failures and
  open an issue with your HOS version + the foreground game.

### "Sleep mode broke audio when I came back"
Closes/reopens the audio device on resume — should self-heal within a
couple seconds. If it doesn't, dismissing and re-summoning the overlay
forces a clean reinit.

### Hitching / stutters during level transitions
Expected behavior for Freedoom 1's larger levels — the engine's zone
allocator (4 MiB) flushes lump caches between maps. For a smoother
experience use the smaller commercial DOOM1.WAD or shareware WAD.

### Switch is on HOS 22+
nx-ovlloader 2.0.0 is incompatible with HOS 22 (libnx upstream commit
`fdf3c87`, [Ultrahand-Overlay/issues/305](https://github.com/ppkantorski/Ultrahand-Overlay/issues/305)).
Wait for nx-ovlloader 2.0.1 or downgrade to a compatible HOS.

---

## Build from source

> Requires devkitPro (devkitA64 + libnx + portlibs) and a clone of this
> repo with submodules. Native Windows, Linux, macOS, and WSL2 are all
> first-class supported.

### Step 1: install devkitPro (one-time setup)

Pick the path that matches your OS:

| OS | How |
|---|---|
| **Windows native** | Run `.\scripts\install-devkitpro.ps1` from PowerShell (downloads + launches the official installer interactively), or grab the [installer](https://github.com/devkitPro/installer/releases) yourself. Either way it puts everything at `C:\devkitPro\` and adds `make` + `bash` + the cross-toolchain to `PATH`. **`make` then works from any shell** — PowerShell, cmd, Windows Terminal, or the bundled "devkitPro MSys2". WSL is not required. For a one-shot bootstrap (install + submodules + first build), use `.\scripts\setup-windows.ps1`. |
| **Linux (Debian/Ubuntu/WSL2)** | `sudo ./scripts/install-devkitpro.sh` (apt-based). Installs to `/opt/devkitpro/`. |
| **macOS / other Linux** | Follow [devkitPro pacman docs](https://devkitpro.org/wiki/devkitPro_pacman) directly — install pacman, then `sudo dkp-pacman -S switch-dev switch-curl switch-zlib switch-mbedtls`. |

Both supported install scripts also pull `switch-curl`, `switch-zlib`,
and `switch-mbedtls` automatically (libultrahand links against curl;
mbedtls is its TLS provider). If you set up devkitPro by hand and
later see `fatal error: curl/curl.h: No such file or directory` mid-
build, that's the missing piece — install with `pacman -S switch-curl
switch-zlib switch-mbedtls --noconfirm` from any shell where `pacman`
is on PATH.

### Step 2: clone + build (all platforms)

The commands below assume bash syntax. On Windows, run them in any shell
where `make` is on PATH (PowerShell, cmd, Windows Terminal, or devkitPro
MSys2 — all work, since the devkitPro installer wires up bash + make
globally). On Linux/WSL/macOS, bash directly:

```bash
# Clone with submodules
git clone --recurse-submodules https://github.com/<your-org>/sx-doom-overlay.git
cd sx-doom-overlay

# Fetch Freedoom 1 (BSD-3, ~29 MB download)
./scripts/fetch-freedoom.sh

# Build (patches auto-apply via Makefile dependency)
make
# Output: out-<platform>/sx-doom-overlay.ovl
#         (out-win/, out-linux/, or out-mac/ — auto-detected via uname so
#          you can build from WSL and PowerShell against the same checkout
#          without 'make clean' between switches)

# Build release zip
make dist
# Output: dist/sx-doom-overlay-X.Y.Z.zip
```

The bottom of the running overlay shows `build: <branch>@<hash>+` so you
can confirm a Switch is running the binary you just built.

### Step 3 (optional): per-WAD music packs

Drop your WADs into `data/wads/`, then either:

- `bash scripts/fetch-chex-music.sh` — pulls a pre-rendered Chex Quest
  pack (50 MB, Arachno SoundFont, free) into `data/music/chex/`.
- `python3 scripts/extract-wad-music.py --sf2 path/to/font.sf2` — render
  any WAD's MUS lumps to OGG at the SoundFont of your choice. Output
  lands in `data/music/<wad-stem>/`.

Copy the matching subdir to `sdmc:/switch/sx-doom-overlay/music/<wad-stem>/`
on the SD card. Runtime auto-picks the right pack per loaded WAD.

### Desktop tests

The engine, palette/scale blit, and audio mixer have desktop unit tests
(no Switch hardware needed):

```bash
cd tests/desktop
make smoke         # boots the engine, dumps 20 PPM frames
make smoke-asan    # same under AddressSanitizer + UBSan
make test_blit     # palette + scale unit tests
make test_sound    # audio mixer unit tests
```

---

## Project layout

```
sx-doom-overlay/
├── source/                              ← overlay shim + audio backends (C++26 / C99)
│   ├── main.cpp                         ← libtesla overlay UI + engine lifecycle
│   ├── elm_ultradoomframe.hpp           ← header element (DOOM banner + version)
│   ├── blit.{cpp,hpp}                   ← palette → RGBA4444 + integer-scale upscaler
│   ├── input_map.hpp                    ← Switch buttons → Doom keys
│   ├── audio_mixer.{c,h}                ← 8-channel SFX mixer (active-channel averaging)
│   ├── audio_backend{,_libnx}.{c,h}     ← libnx audout backend
│   ├── i_sound_switch.c                 ← engine-side SFX bridge + DMX decode + cache
│   ├── music_ogg.{c,h}                  ← stb_vorbis OGG decoder + per-WAD lookup
│   │                                       + OPL fallback dispatcher
│   ├── stb_vorbis_impl.h                ← vendored stb_vorbis (renamed .h on purpose)
│   └── opl/                             ← vendored Nuked-OPL3 + chocolate-doom MIDI player
├── lib/                                 ← build dependencies (git submodules)
│   ├── libultrahand/                    ← libtesla + libultra (overlay framework)
│   └── doomgeneric/                     ← Doom engine (chocolate-doom fork)
├── patches/0001..0008-*.patch           ← surgical patches against doomgeneric
│                                          (lower MIN_RAM, exit→longjmp, faster turn,
│                                           per-WAD savegames, ENDOOM no-op,
│                                           bypass loop interface, decouple sound module)
├── tests/desktop/                       ← Linux unit tests (no Switch needed)
├── scripts/
│   ├── install-devkitpro.{sh,ps1}       ← bootstrap toolchain (Linux + Windows)
│   ├── setup-windows.ps1                ← one-shot install + submodules + first build
│   ├── apply-patches.sh                 ← idempotent patch applier (CRLF-tolerant)
│   ├── fetch-freedoom.sh                ← bundled WAD download
│   ├── fetch-chex-music.sh              ← Arachno Chex Quest OGG pack
│   ├── extract-wad-music.py             ← MUS → MIDI → OGG offline renderer
│   ├── sync-sd.sh + mtp-sync.ps1        ← deploy .ovl + pull diagnostics
│   ├── check-ovl-size.sh + dist.sh      ← release zip + size sanity
├── data/
│   ├── wads/                            ← drop your WADs here (gitignored)
│   ├── music/<wad-stem>/                ← OGG packs per WAD (gitignored)
│   ├── freedoom1.wad → wads/            ← bundled at release via dist.sh
│   └── LICENSE.freedoom                 ← BSD-3 attribution
├── docs/
│   ├── prd/                             ← product specs
│   └── plans/                           ← implementation plans
├── build-<platform>/                    ← gcc intermediates (gitignored)
├── out-<platform>/sx-doom-overlay.ovl   ← final build output (gitignored)
├── Makefile                             ← cross-compile to .ovl (uname-based BUILD/OUT)
├── CLAUDE.md                            ← guidance for AI assistants in this repo
├── .gitattributes                       ← LF for *.patch/*.sh, CRLF for *.ps1
└── .gitmodules                          ← submodule config (ignore=dirty for doomgeneric)
```

---

## License

GPLv2 (engine, libtesla/libultra all GPLv2). Freedoom is BSD-3-Clause and
GPL-compatible; bundled with attribution at
`/switch/.overlays/doom/LICENSE.freedoom`.

Doom and the original DOOM 1/2/3 WADs are © id Software and not
distributed by this project.

---

## Credits

- [doomgeneric](https://github.com/ozkl/doomgeneric) by ozkl — the Chocolate
  Doom fork with the platform shim that made this port possible
- [libultrahand / Ultrahand-Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)
  by ppkantorski — the framework
- [nx-ovlloader](https://github.com/ppkantorski/nx-ovlloader) by
  ppkantorski / WerWolv — the loader sysmodule
- [UltraGB-Overlay](https://github.com/ppkantorski/UltraGB-Overlay) by
  ppkantorski — the architectural reference for engine-in-overlay
- [Tetris-Overlay](https://github.com/ppkantorski/Tetris-Overlay) by
  ppkantorski — the original "game in overlay" precedent
- [Freedoom](https://freedoom.github.io/) — free, GPL-compatible IWAD
- id Software — DOOM
