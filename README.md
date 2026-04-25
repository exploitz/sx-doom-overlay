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

> **Status: pre-release.** Project tracking via
> [docs/plans/2026-04-25-doom-overlay.md](docs/plans/2026-04-25-doom-overlay.md).
> 4 of 12 implementation tasks complete (project bootstrap, desktop engine
> smoke, palette+blit module, audio mixer skeleton). Hardware integration
> pending devkitPro install.

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

## Settings

Press **Minus** during gameplay to open the settings page. Persisted to
`/config/sx-doom-overlay/config.ini`.

| Setting | Values | Notes |
|---|---|---|
| Render scale | 1× / 2× / 3× | Default 2× (640×400). 3× = 960×600 if your heap allows. |
| Master volume | 0–100 | Applies in real time |
| Music volume | 0–100 | Same |
| Active WAD | (auto-detected) | Drop more `*.wad` files at `/switch/.overlays/doom/` to add to the picker |
| Vibration | on / off | Rumble on weapon fire |

**Render scale and active WAD changes require closing and reopening the
overlay to take effect.** Volume changes are live.

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

Then pick the WAD you want from the settings page. We do **not**
distribute commercial WADs — id Software's redistribution terms apply
to those.

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
> repo with submodules. Tested on Linux/WSL2 and Windows MSYS2.

```bash
# 1. Install devkitPro (Linux/WSL2 — see scripts/install-devkitpro.sh)
sudo ./scripts/install-devkitpro.sh

# 2. Clone with submodules
git clone --recurse-submodules https://github.com/<your-org>/sx-doom-overlay.git
cd sx-doom-overlay

# 3. Fetch Freedoom 1 (BSD-3, ~29 MB download)
./scripts/fetch-freedoom.sh

# 4. Apply patches (lowers Doom zone to 3 MiB; replaces engine exit() calls
#    with longjmp so engine errors don't kill the overlay sysmodule)
./scripts/apply-patches.sh

# 5. Build
make
# Output: out/sx-doom-overlay.ovl

# 6. Build release zip
make dist
# Output: dist/sx-doom-overlay-X.Y.Z.zip
```

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
├── source/                           ← our overlay shim (C++26)
│   ├── main.cpp                      ← tsl::Overlay + tsl::Gui + tsl::Element
│   ├── blit.{cpp,hpp}                ← palette → RGBA4444 + integer-scale upscaler
│   ├── audio_mixer.{c,h}             ← 8-channel SFX mixer
│   ├── audio_backend.h               ← abstract sink interface
│   └── audio_backend_libnx.c         ← production libnx audout backend
├── lib/                              ← build dependencies (git submodules)
│   ├── libultrahand/                 ← libtesla + libultra (overlay framework)
│   └── doomgeneric/                  ← Doom engine (Chocolate Doom fork)
├── patches/                          ← surgical patches against doomgeneric
│   ├── 0001-lower-min-ram.patch      ← MIN_RAM 6→3 MiB (8 MB heap budget)
│   └── 0002-patch-exit-sites.patch   ← exit() → longjmp (overlay survival)
├── tests/desktop/                    ← Linux unit tests (no Switch needed)
├── scripts/                          ← install-devkitpro.sh, apply-patches.sh,
│                                       fetch-freedoom.sh, check-ovl-size.sh,
│                                       dist.sh
├── docs/
│   ├── prd/2026-04-25-doom-overlay.md   ← product spec
│   └── plans/2026-04-25-doom-overlay.md ← implementation plan (12 tasks)
├── data/                             ← bundled at release time (freedoom1.wad)
└── Makefile                          ← cross-compile to .ovl
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
