#!/usr/bin/env bash
# =============================================================================
# dist.sh — assemble release zip for sx-doom-overlay
#
# Produces dist/sx-doom-overlay-<version>.zip with the SD-card layout
# documented in docs/prd/2026-04-25-doom-overlay.md (Flow 1 install):
#
#   /switch/.overlays/sx-doom-overlay.ovl       ← the overlay binary
#   /switch/.overlays/doom/freedoom1.wad        ← bundled IWAD (BSD-3)
#   /switch/.overlays/doom/LICENSE.freedoom     ← BSD-3 attribution
#   README.md                                   ← end-user install/play docs
#
# Usage: scripts/dist.sh <version>
#   make dist (= make dist VERSION=$(APP_VERSION) → calls this)
#
# Licensed under GPLv2.
# =============================================================================

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <version>" >&2
    echo "  e.g.: $0 0.1.0" >&2
    exit 1
fi

VERSION="$1"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Resolve out-<platform>/ produced by Makefile (with legacy out/ fallback).
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) PLATFORM=win ;;
    Darwin)               PLATFORM=mac ;;
    Linux)                PLATFORM=linux ;;
    *)                    PLATFORM=unknown ;;
esac
if [ -f "$ROOT/out-$PLATFORM/sx-doom-overlay.ovl" ]; then
    OVL_PATH="$ROOT/out-$PLATFORM/sx-doom-overlay.ovl"
else
    OVL_PATH="$ROOT/out/sx-doom-overlay.ovl"   # legacy layout fallback
fi
WAD_PATH="$ROOT/data/wads/freedoom1.wad"
LICENSE_PATH="$ROOT/data/LICENSE.freedoom"
README_PATH="$ROOT/README.md"

DIST_DIR="$ROOT/dist"
STAGE_DIR="$DIST_DIR/stage-$VERSION"
ZIP_PATH="$DIST_DIR/sx-doom-overlay-$VERSION.zip"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
err() { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; }

# --- Sanity checks -----------------------------------------------------------

bold "==> Pre-flight checks for v$VERSION"

missing=0
for f in "$OVL_PATH" "$WAD_PATH" "$README_PATH"; do
    if [ ! -f "$f" ]; then
        err "missing: $f"
        missing=1
    fi
done
[ $missing -eq 1 ] && exit 1

# License is encouraged but not blocking — warn if missing.
if [ ! -f "$LICENSE_PATH" ]; then
    err "WARNING: $LICENSE_PATH not found. Freedoom is BSD-3 — attribution"
    err "         is required by the license. Run scripts/fetch-freedoom.sh"
    err "         to fetch both wad and LICENSE before re-running dist."
fi

# Verify .ovl size is in expected range
"$SCRIPT_DIR/check-ovl-size.sh" "$OVL_PATH"

# --- Stage and zip -----------------------------------------------------------

bold "==> Staging files"
rm -rf "$STAGE_DIR"
mkdir -p "$STAGE_DIR/switch/.overlays/doom"

cp "$OVL_PATH" "$STAGE_DIR/switch/.overlays/sx-doom-overlay.ovl"
cp "$WAD_PATH" "$STAGE_DIR/switch/.overlays/doom/freedoom1.wad"
[ -f "$LICENSE_PATH" ] && cp "$LICENSE_PATH" "$STAGE_DIR/switch/.overlays/doom/LICENSE.freedoom"
cp "$README_PATH" "$STAGE_DIR/README.md"
# Top-level GPLv2 license for our overlay code (GPL §3 source-availability is
# satisfied by README pointing at the GitHub repo, but shipping a copy of the
# license text in the zip makes the binary legally self-explanatory).
[ -f "$ROOT/LICENSE" ] && cp "$ROOT/LICENSE" "$STAGE_DIR/LICENSE"

bold "==> Zipping"
rm -f "$ZIP_PATH"
# Prefer the `zip` binary if available; fall back to python3's zipfile module
# (which is part of the stdlib and usually available even on minimal systems).
if command -v zip >/dev/null 2>&1; then
    ( cd "$STAGE_DIR" && zip -r "$ZIP_PATH" . -q )
elif command -v python3 >/dev/null 2>&1; then
    echo "    (using python3 zipfile — install 'zip' for faster zipping)"
    ( cd "$STAGE_DIR" && python3 -c "
import os, sys, zipfile
with zipfile.ZipFile('$ZIP_PATH', 'w', zipfile.ZIP_DEFLATED) as z:
    for root, dirs, files in os.walk('.'):
        for f in files:
            p = os.path.join(root, f)
            z.write(p, os.path.relpath(p, '.'))
" )
else
    err "Neither 'zip' nor 'python3' found. Install one: sudo apt install -y zip"
    exit 1
fi

# --- Cleanup + report --------------------------------------------------------

rm -rf "$STAGE_DIR"

ZIP_SIZE=$(stat -c%s "$ZIP_PATH" 2>/dev/null || stat -f%z "$ZIP_PATH")
ZIP_MB=$((ZIP_SIZE / 1048576))

bold "==> OK"
echo "    $ZIP_PATH"
echo "    size: $ZIP_SIZE bytes (~$ZIP_MB MB)"
echo
echo "    Contents:"
unzip -l "$ZIP_PATH" | tail -n +4 | head -n -2 | awk '{printf "      %s\n", $4}'
