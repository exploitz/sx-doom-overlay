#!/usr/bin/env bash
# =============================================================================
# apply-patches.sh — apply patches/*.patch to lib/doomgeneric, idempotent.
#
# Used by both the parent Makefile (cross-build to .ovl) and tests/desktop
# (Linux smoke). Fails LOUDLY on any patch that doesn't apply cleanly —
# never silent skip, because that would compile the engine with the upstream
# MIN_RAM=6 and produce a runtime OOM with no compile error.
#
# Sentinel: lib/doomgeneric/.patched (ts of last successful apply).
# Re-running with no changes is a fast no-op; mtime triggers re-apply.
#
# Licensed under GPLv2.
# =============================================================================

set -euo pipefail

# Resolve the project root regardless of where this script is invoked from.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

PATCHES_DIR="$ROOT/patches"
TARGET_DIR="$ROOT/lib/doomgeneric"
SENTINEL="$TARGET_DIR/.patched"

if [ ! -d "$TARGET_DIR" ]; then
    echo "ERROR: $TARGET_DIR does not exist. Did you forget \`git submodule update --init\`?" >&2
    exit 1
fi

# Collect patches in lexical order. No patches → success no-op.
PATCH_FILES=()
if [ -d "$PATCHES_DIR" ]; then
    while IFS= read -r -d '' p; do
        PATCH_FILES+=("$p")
    done < <(find "$PATCHES_DIR" -maxdepth 1 -name '*.patch' -print0 | sort -z)
fi

if [ ${#PATCH_FILES[@]} -eq 0 ]; then
    echo "[apply-patches] no patches in $PATCHES_DIR — skipping (sentinel touched)"
    touch "$SENTINEL"
    exit 0
fi

# Sentinel optimization: if .patched mtime is newer than every patch file,
# we've already applied them. Skip the work.
if [ -f "$SENTINEL" ]; then
    NEED_REAPPLY=0
    for p in "${PATCH_FILES[@]}"; do
        if [ "$p" -nt "$SENTINEL" ]; then
            NEED_REAPPLY=1
            break
        fi
    done
    if [ $NEED_REAPPLY -eq 0 ]; then
        echo "[apply-patches] sentinel up to date — no work to do"
        exit 0
    fi
    echo "[apply-patches] patch files newer than sentinel — re-applying"
    # Reset to clean state before re-applying. We use git restore on the
    # specific files the patches touch, not a full submodule reset, to
    # preserve any submodule update the user may have run.
    cd "$TARGET_DIR"
    # Conservative: hard reset only the working tree of the submodule.
    # If a user has staged or unstaged work in the submodule (unusual), this
    # discards it — but that work was inside a vendored upstream pin, so it
    # has no business being there.
    git checkout -- .
    cd - >/dev/null
fi

# Pre-process each patch through `tr -d '\r'` to strip CRLF on the fly.
# Background: Windows git with core.autocrlf=true rewrites checked-out
# `*.patch` files to CRLF; `git apply` then sees CR mismatches against
# the LF-checked-out source and refuses with "patch does not apply".
# Stripping CR at apply-time fixes existing dirty clones; the new
# .gitattributes file fixes future clones.
cd "$TARGET_DIR"
for p in "${PATCH_FILES[@]}"; do
    NAME=$(basename "$p")
    # If the patch's reverse applies cleanly, it's already applied.
    # If forward applies cleanly, apply it.
    # If neither, it's broken (e.g., submodule HEAD moved past it).
    if tr -d '\r' < "$p" | git apply -R --check 2>/dev/null; then
        echo "[apply-patches] $NAME already applied — skip"
        continue
    fi
    if tr -d '\r' < "$p" | git apply --check 2>/dev/null; then
        echo "[apply-patches] applying $NAME"
        if ! tr -d '\r' < "$p" | git apply; then
            echo "ERROR: git apply failed for $NAME (check passed but apply failed — this should not happen)" >&2
            exit 1
        fi
        continue
    fi
    # Neither forward nor reverse applies cleanly — actually broken.
    echo "" >&2
    echo "ERROR: patch $NAME does not apply cleanly to lib/doomgeneric, and is not already applied either." >&2
    echo "       Most likely causes:" >&2
    echo "         1. lib/doomgeneric submodule is on the wrong commit." >&2
    echo "            Fix: git submodule update --init --recursive" >&2
    echo "         2. The patch file itself was edited / corrupted." >&2
    echo "            Fix: git checkout -- $p" >&2
    echo "         3. doomgeneric upstream moved and the patch needs re-rolling." >&2
    echo "            Re-roll: cd $TARGET_DIR; <apply changes manually>; git diff > $p" >&2
    echo "" >&2
    exit 1
done
cd - >/dev/null

touch "$SENTINEL"
echo "[apply-patches] applied ${#PATCH_FILES[@]} patch(es); sentinel touched"
