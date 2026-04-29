#!/usr/bin/env bash
# =============================================================================
# fetch-chex-music.sh — download Chex Quest OGG music pack into data/music/chex/
#
# Source: Arachnosoft "Arachno SoundFont Chex Quest Music Pack"
#   https://www.arachnosoft.com/main/download.php?id=arachno-chexquest-music-pack-pk3-ogg
#
# Maxime Abbey (Arachnosoft) renders Doom/Chex MIDI tracks via the Arachno
# SoundFont and packages them as PK3 (zip) for in-engine use. Same idea as
# the Brandon Blume SC-55 Doom pack — different instrument bank, but
# audibly higher fidelity than OPL FM synthesis.
#
# Output:
#   data/music/chex/d_*.ogg
#
# Drop-in for sx-doom-overlay's OGG music branch when CHEX.WAD is loaded:
#     cp data/music/chex/*.ogg /sdmc/switch/sx-doom-overlay/music/
#
# Idempotent — re-running with files already present is a no-op.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OUT_DIR="$ROOT/data/music/chex"
PK3_URL="http://maxime.abbey.free.fr/mirror/arachnosoft/files/music-packs/pk3-ogg/arachno-chexquest-music-pack-ogg.pk3"
PK3_PATH="/tmp/arachno-chexquest-music-pack-ogg.pk3"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
err()  { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; }

mkdir -p "$OUT_DIR"

# --- Skip if already populated -----------------------------------------------

EXISTING_OGGS=$(find "$OUT_DIR" -maxdepth 1 -iname 'd_*.ogg' 2>/dev/null | wc -l)
if [ "$EXISTING_OGGS" -ge 18 ]; then
    bold "==> $OUT_DIR/ already has $EXISTING_OGGS OGG files — skipping fetch"
    exit 0
fi

# --- Verify required tools ---------------------------------------------------

for tool in unzip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        err "$tool is required. Install it: sudo apt install -y $tool"
        exit 1
    fi
done

# --- Download (Python urllib so this works without wget/curl) ---------------

if [ ! -f "$PK3_PATH" ] || [ ! -s "$PK3_PATH" ]; then
    bold "==> Downloading Chex Quest music pack"
    echo "    URL: $PK3_URL"
    python3 -c "
import sys, urllib.request
url = '$PK3_URL'
out = '$PK3_PATH'
last_pct = -1
def progress(blocks, block_size, total):
    global last_pct
    if total > 0:
        pct = min(100, blocks * block_size * 100 // total)
        if pct != last_pct and pct % 10 == 0:
            print(f'    {pct}%', flush=True)
            last_pct = pct
urllib.request.urlretrieve(url, out, progress)
" || { err "download failed"; exit 1; }
fi

# --- Extract -----------------------------------------------------------------

bold "==> Extracting"
TMP_EXTRACT="/tmp/chex-music-extract"
rm -rf "$TMP_EXTRACT"
mkdir -p "$TMP_EXTRACT"
unzip -q "$PK3_PATH" -d "$TMP_EXTRACT"

# Find the music subdir within the PK3 (it's `music/` at top level).
MUSIC_SRC=$(find "$TMP_EXTRACT" -type d -iname 'music' | head -1)
if [ -z "$MUSIC_SRC" ]; then
    err "no music/ subdir in PK3 — listing for debug:"
    find "$TMP_EXTRACT" -maxdepth 2 | head -10
    exit 1
fi

# Copy + lowercase. Music_ogg lookup builds lowercase d_<name>.ogg paths;
# uppercase D_E1M1.ogg works on FAT32 (case-insensitive) but lowercase is
# safer across host filesystems too.
COUNT=0
for f in "$MUSIC_SRC"/D_*.ogg; do
    [ -f "$f" ] || continue
    base=$(basename "$f" | tr '[:upper:]' '[:lower:]')
    cp "$f" "$OUT_DIR/$base"
    COUNT=$((COUNT + 1))
done

rm -rf "$TMP_EXTRACT"

# --- Verify ------------------------------------------------------------------

bold "==> Done"
echo "    $OUT_DIR/  ($COUNT OGG files)"
echo ""
echo "    To use on Switch with CHEX.WAD loaded:"
echo "      cp $OUT_DIR/*.ogg /sdmc/switch/sx-doom-overlay/music/"
