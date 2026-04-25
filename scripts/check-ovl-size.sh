#!/usr/bin/env bash
# =============================================================================
# check-ovl-size.sh — verify .ovl is in plausible size range
#
# Used by Task 5's cross-build smoke DoD: confirms the linked overlay binary
# is not absurdly small (link error producing a stub) or absurdly large
# (toolchain wedge / accidental static link of huge libs).
#
# Expected range: 1 MB – 4 MB.
#   - Bootstrap (Task 1, no engine): ~50–500 KB
#   - With engine + audio (Task 5 onward): 1 MB – 3 MB
#   - Anything > 4 MB is suspicious — investigate before deploying
#
# Exit codes:
#   0  - in expected range
#   1  - file does not exist
#   2  - too small (likely missing engine/library)
#   3  - too large (likely accidental fat link / debug bloat)
#
# Licensed under GPLv2.
# =============================================================================

set -euo pipefail

if [ $# -ne 1 ]; then
    echo "Usage: $0 <path-to-.ovl>" >&2
    exit 1
fi

OVL="$1"

if [ ! -f "$OVL" ]; then
    echo "ERROR: $OVL does not exist" >&2
    exit 1
fi

SIZE=$(stat -c%s "$OVL" 2>/dev/null || stat -f%z "$OVL")
SIZE_KB=$((SIZE / 1024))
SIZE_MB_X100=$((SIZE * 100 / 1048576))   # MB × 100, for one-decimal display

MIN_BYTES=$((100 * 1024))         # 100 KB — anything smaller is a stub
MAX_BYTES=$((4 * 1024 * 1024))    # 4 MB

bold() { printf '\033[1m%s\033[0m\n' "$*"; }

bold "==> $OVL"
echo "    size: $SIZE bytes ($SIZE_KB KB; ${SIZE_MB_X100:0:-2}.${SIZE_MB_X100: -2} MB)"

if [ "$SIZE" -lt "$MIN_BYTES" ]; then
    echo "FAIL: smaller than 100 KB. Likely a stub — check link errors." >&2
    exit 2
fi

if [ "$SIZE" -gt "$MAX_BYTES" ]; then
    echo "FAIL: larger than 4 MB. Suspect debug symbols or accidental static linkage." >&2
    echo "      Try: aarch64-none-elf-strip $OVL" >&2
    exit 3
fi

# Sanity check: file should start with the homebrew NRO magic ("NRO0").
# devkitA64-built .nro / .ovl are NRO format (Nintendo homebrew Resource Object).
MAGIC=$(dd if="$OVL" bs=4 count=1 skip=4 2>/dev/null || true)
case "$MAGIC" in
    NRO0)
        echo "    magic: NRO0 (homebrew Resource Object — OK)"
        ;;
    *)
        echo "WARN: file does not start with NRO0 magic — may not be a valid overlay" >&2
        ;;
esac

bold "==> OK — size in expected range"
