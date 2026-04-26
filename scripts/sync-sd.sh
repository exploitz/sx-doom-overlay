#!/usr/bin/env bash
# =============================================================================
# sync-sd.sh — auto-deploy .ovl to Switch SD + pull back diagnostics.
#
# Use case: Switch is in USB mass-storage mode (Hekate UMS) so the SD card
# shows up on Windows as a removable drive, accessible from WSL via /mnt/X/.
# This script:
#   1. Detects the SD card by looking for /mnt/<letter>/switch/.overlays/
#   2. Copies out/sx-doom-overlay.ovl into /switch/.overlays/
#   3. Optionally syncs the bundled WAD if --wad is passed
#   4. Pulls back trace.log + new /atmosphere/crash_reports/*.{log,bin}
#      into ./diagnostics/<timestamp>/ so we don't lose history across runs
#
# Usage:
#   ./scripts/sync-sd.sh                  — push .ovl, pull diagnostics
#   ./scripts/sync-sd.sh --wad            — also push the bundled WAD
#   ./scripts/sync-sd.sh --pull-only      — only pull diagnostics (no push)
#   ./scripts/sync-sd.sh --push-only      — only push .ovl (no pull)
#
# Licensed under GPLv2.
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

OVL_PATH="$ROOT/out/sx-doom-overlay.ovl"
WAD_PATH="$ROOT/data/freedoom1.wad"
LICENSE_PATH="$ROOT/data/LICENSE.freedoom"
DIAG_BASE="$ROOT/diagnostics"

DO_PUSH_OVL=1
DO_PUSH_WAD=0
DO_PULL_DIAG=1

for arg in "$@"; do
    case "$arg" in
        --wad)        DO_PUSH_WAD=1 ;;
        --pull-only)  DO_PUSH_OVL=0 ;;
        --push-only)  DO_PULL_DIAG=0 ;;
        *) echo "unknown arg: $arg" >&2; exit 1 ;;
    esac
done

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
err()  { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; }
warn() { printf '\033[33mWARN:\033[0m %s\n' "$*" >&2; }

# --- Detect SD ---------------------------------------------------------------

bold "==> Detecting Switch SD card via /mnt/* drives"
SD_ROOT=""
for d in /mnt/[a-z]; do
    if [ -d "$d/switch/.overlays" ] || [ -d "$d/atmosphere/contents" ]; then
        SD_ROOT="$d"
        break
    fi
done

if [ -z "$SD_ROOT" ]; then
    # Fall back to MTP mode (Switch via DBI/MTP shows up in Windows Explorer
    # as "This PC\Nintendo Switch\SD Card\..." but isn't a drive letter, so
    # WSL can't see it directly. We use a PowerShell helper that talks to
    # the device via the Shell.Application COM API.
    if command -v powershell.exe >/dev/null 2>&1; then
        bold "==> No /mnt drive — trying MTP via PowerShell"
        SD_ROOT="MTP"
    else
        err "No SD card detected on /mnt/* and powershell.exe not in PATH."
        err ""
        err "Two options:"
        err "  A) Hekate UMS — boot into Hekate → Tools → USB Tools → SD Card"
        err "     Windows mounts SD as drive letter; rerun script."
        err "  B) DBI MTP / Goldleaf — already shows in Explorer as"
        err "     'This PC\\Nintendo Switch\\SD Card', but this WSL has no"
        err "     powershell.exe so we can't reach it from here."
        exit 1
    fi
fi

if [ "$SD_ROOT" = "MTP" ]; then
    echo "    Mode: MTP (via PowerShell + Shell COM API)"
else
    echo "    Found: $SD_ROOT"
    [ -d "$SD_ROOT/switch/.overlays" ] && echo "    /switch/.overlays/ : present"
    [ -d "$SD_ROOT/atmosphere/contents" ] && echo "    /atmosphere/contents/ : present"
fi

# --- MTP helpers (only used when SD_ROOT=MTP) --------------------------------

PS_SCRIPT_WIN=""
if [ "$SD_ROOT" = "MTP" ]; then
    PS_SCRIPT_WIN="$(wslpath -w "$SCRIPT_DIR/mtp-sync.ps1")"
fi

mtp_push() {
    local local_path="$1" remote_path="$2"
    local local_win="$(wslpath -w "$local_path")"
    powershell.exe -ExecutionPolicy Bypass -File "$PS_SCRIPT_WIN" \
        -Action push -LocalPath "$local_win" -RemotePath "$remote_path"
}

mtp_pull() {
    local remote_path="$1" local_path="$2"
    local local_win="$(wslpath -w "$local_path")"
    powershell.exe -ExecutionPolicy Bypass -File "$PS_SCRIPT_WIN" \
        -Action pull -RemotePath "$remote_path" -LocalPath "$local_win"
}

# --- Pull diagnostics first ---------------------------------------------------
# (do this before push so we don't blow away the trace from the run we're
# debugging.)

if [ "$DO_PULL_DIAG" -eq 1 ]; then
    TS="$(date +%Y%m%d-%H%M%S)"
    OUT_DIR="$DIAG_BASE/$TS"
    mkdir -p "$OUT_DIR"

    bold "==> Pulling diagnostics → $OUT_DIR"

    if [ "$SD_ROOT" = "MTP" ]; then
        # MTP path — pull individually
        if mtp_pull "config/sx-doom-overlay/trace.log" "$OUT_DIR/trace.log" 2>/dev/null; then
            echo "    trace.log: $(wc -l < "$OUT_DIR/trace.log" 2>/dev/null || echo 0) lines"
        else
            echo "    trace.log: not present on SD"
        fi
        mtp_pull "config/sx-doom-overlay/error.log" "$OUT_DIR/error.log" 2>/dev/null || true
        echo "    (skipping crash_reports pull on MTP — listing/discovery is slow)"
    else
        # trace.log — our overlay's own log
        if [ -f "$SD_ROOT/config/sx-doom-overlay/trace.log" ]; then
            cp "$SD_ROOT/config/sx-doom-overlay/trace.log" "$OUT_DIR/trace.log"
            echo "    trace.log: $(wc -l < "$OUT_DIR/trace.log") lines"
        else
            echo "    trace.log: not present (no run yet)"
        fi

        if [ -f "$SD_ROOT/config/sx-doom-overlay/error.log" ]; then
            cp "$SD_ROOT/config/sx-doom-overlay/error.log" "$OUT_DIR/error.log"
        fi

        if [ -d "$SD_ROOT/atmosphere/crash_reports" ]; then
            mkdir -p "$OUT_DIR/crash_reports"
            find "$SD_ROOT/atmosphere/crash_reports" -maxdepth 1 \
                \( -name '*.log' -o -name '*.bin' \) -mtime -1 \
                -exec cp {} "$OUT_DIR/crash_reports/" \; 2>/dev/null || true
            COUNT=$(ls "$OUT_DIR/crash_reports/" 2>/dev/null | wc -l)
            if [ "$COUNT" -gt 0 ]; then
                echo "    crash_reports: $COUNT file(s) (last 24h)"
            else
                echo "    crash_reports: none new (last 24h)"
                rmdir "$OUT_DIR/crash_reports" 2>/dev/null || true
            fi
        fi
    fi

    # If nothing was pulled, remove the empty timestamp dir
    if [ -z "$(ls -A "$OUT_DIR" 2>/dev/null)" ]; then
        rmdir "$OUT_DIR"
        echo "    (no new diagnostics — directory removed)"
    fi
fi

# --- Push .ovl ---------------------------------------------------------------

if [ "$DO_PUSH_OVL" -eq 1 ]; then
    if [ ! -f "$OVL_PATH" ]; then
        err "$OVL_PATH not found — run 'make' first"
        exit 1
    fi

    DEST_DIR="$SD_ROOT/switch/.overlays"
    mkdir -p "$DEST_DIR"
    DEST="$DEST_DIR/sx-doom-overlay.ovl"

    bold "==> Pushing $OVL_PATH"
    cp "$OVL_PATH" "$DEST"
    sync
    SIZE_LOCAL=$(stat -c%s "$OVL_PATH")
    SIZE_REMOTE=$(stat -c%s "$DEST")
    if [ "$SIZE_LOCAL" -ne "$SIZE_REMOTE" ]; then
        warn "size mismatch — local $SIZE_LOCAL vs SD $SIZE_REMOTE"
    fi
    echo "    → $DEST ($SIZE_REMOTE bytes)"
fi

# --- Push WAD (optional) ------------------------------------------------------

if [ "$DO_PUSH_WAD" -eq 1 ]; then
    if [ ! -f "$WAD_PATH" ]; then
        err "$WAD_PATH not found — run scripts/fetch-freedoom.sh first"
        exit 1
    fi

    DEST_DIR="$SD_ROOT/switch/.overlays/doom"
    mkdir -p "$DEST_DIR"

    bold "==> Pushing $WAD_PATH (this is large; takes a bit)"
    cp "$WAD_PATH" "$DEST_DIR/freedoom1.wad"
    [ -f "$LICENSE_PATH" ] && cp "$LICENSE_PATH" "$DEST_DIR/LICENSE.freedoom"
    sync
    echo "    → $DEST_DIR/freedoom1.wad"
fi

bold "==> Done"
echo "    Eject SD safely on Windows, return Switch from UMS to Atmosphere,"
echo "    then summon the overlay to test."
