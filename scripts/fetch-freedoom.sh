#!/usr/bin/env bash
# =============================================================================
# fetch-freedoom.sh — download Freedoom Phase 1 WAD into data/
#
# Freedoom is BSD-3-Clause licensed and freely redistributable. We bundle it
# in our release zip rather than ship the commercial DOOM1.WAD (which has
# tighter redistribution rules).
#
# Output:
#   data/wads/freedoom1.wad
#   data/LICENSE.freedoom (BSD-3 attribution, required by the Freedoom license)
#
# Idempotent — re-running with a valid WAD already present is a no-op.
# =============================================================================

set -euo pipefail

# Resolve the project root regardless of where this script is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DATA_DIR="$ROOT/data"
WADS_DIR="$DATA_DIR/wads"
WAD_PATH="$WADS_DIR/freedoom1.wad"
LICENSE_PATH="$DATA_DIR/LICENSE.freedoom"

# Source: https://freedoom.github.io/ — Phase 1, latest stable release.
# Pin to a specific version so the build is reproducible.
FREEDOOM_VERSION="0.13.0"
FREEDOOM_URL="https://github.com/freedoom/freedoom/releases/download/v${FREEDOOM_VERSION}/freedoom-${FREEDOOM_VERSION}.zip"
FREEDOOM_ZIP="/tmp/freedoom-${FREEDOOM_VERSION}.zip"
FREEDOOM_EXTRACT_DIR="/tmp/freedoom-${FREEDOOM_VERSION}"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
err() { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; }

mkdir -p "$WADS_DIR"

# --- Skip if already present and valid ---------------------------------------

if [ -f "$WAD_PATH" ]; then
    SIZE=$(stat -c%s "$WAD_PATH" 2>/dev/null || stat -f%z "$WAD_PATH")
    if [ "$SIZE" -gt 1000000 ]; then
        bold "==> $WAD_PATH already present ($SIZE bytes) — skipping fetch"
        exit 0
    else
        echo "[fetch] existing $WAD_PATH is too small ($SIZE bytes), re-fetching"
        rm -f "$WAD_PATH"
    fi
fi

# --- Verify required tools ---------------------------------------------------

for tool in wget unzip; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        err "$tool is required. Install it: sudo apt install -y $tool"
        exit 1
    fi
done

# --- Download ----------------------------------------------------------------

bold "==> Downloading Freedoom v${FREEDOOM_VERSION}"
echo "    URL: $FREEDOOM_URL"
if [ ! -f "$FREEDOOM_ZIP" ] || [ ! -s "$FREEDOOM_ZIP" ]; then
    if ! wget --show-progress -O "$FREEDOOM_ZIP" "$FREEDOOM_URL"; then
        err "wget failed."
        err "Download manually from https://freedoom.github.io/ and place the .zip at $FREEDOOM_ZIP"
        exit 1
    fi
fi

# --- Extract -----------------------------------------------------------------

bold "==> Extracting"
rm -rf "$FREEDOOM_EXTRACT_DIR"
unzip -q "$FREEDOOM_ZIP" -d "$(dirname "$FREEDOOM_EXTRACT_DIR")"

# Find the WAD within the extracted tree (path varies between releases).
WAD_SRC=$(find "$FREEDOOM_EXTRACT_DIR" -maxdepth 3 -iname 'freedoom1.wad' | head -1)
if [ -z "$WAD_SRC" ] || [ ! -f "$WAD_SRC" ]; then
    err "freedoom1.wad not found in the extracted archive."
    err "Looked under: $FREEDOOM_EXTRACT_DIR"
    err "Listing contents for debugging:"
    find "$FREEDOOM_EXTRACT_DIR" -maxdepth 3 | head -20
    exit 1
fi

cp "$WAD_SRC" "$WAD_PATH"

# Find the license file (Freedoom ships it as COPYING / COPYING.txt / LICENSE).
LICENSE_SRC=$(find "$FREEDOOM_EXTRACT_DIR" -maxdepth 3 \
    \( -iname 'COPYING' -o -iname 'COPYING.txt' -o -iname 'LICENSE' -o -iname 'LICENSE.txt' \) \
    -print -quit 2>/dev/null)
if [ -n "$LICENSE_SRC" ] && [ -f "$LICENSE_SRC" ]; then
    cp "$LICENSE_SRC" "$LICENSE_PATH"
    echo "    license: $(basename "$LICENSE_SRC") → $(basename "$LICENSE_PATH")"
else
    echo "    WARNING: no license file found in extract — release zip will lack LICENSE.freedoom"
    echo "    Listing for debug:"
    ls "$FREEDOOM_EXTRACT_DIR" 2>/dev/null | head -5
fi

# --- Verify ------------------------------------------------------------------

WAD_SIZE=$(stat -c%s "$WAD_PATH" 2>/dev/null || stat -f%z "$WAD_PATH")
bold "==> Done"
echo "    $WAD_PATH ($WAD_SIZE bytes)"
[ -f "$LICENSE_PATH" ] && echo "    $LICENSE_PATH ($(wc -l < "$LICENSE_PATH") lines)"
