#!/usr/bin/env bash
# =============================================================================
# install-devkitpro.sh — install devkitPro + switch-dev for sx-doom-overlay
#
# Purpose: set up the cross-compile toolchain (devkitA64 + libnx + portlibs)
# needed to build .ovl files for Nintendo Switch homebrew. One-time install.
#
# Requires: sudo. Adds an apt source for devkitPro, installs the pacman
# wrapper, then uses dkp-pacman to pull switch-dev (~3-5 GB).
#
# Source of truth: https://devkitpro.org/wiki/devkitPro_pacman
# Verify the script reads sensibly before running it.
# =============================================================================

set -euo pipefail

DEVKITPRO_DIR="/opt/devkitpro"

bold() { printf '\033[1m%s\033[0m\n' "$*"; }
err() { printf '\033[31mERROR:\033[0m %s\n' "$*" >&2; }

# --- Pre-flight ---------------------------------------------------------------

bold "==> Checking for existing devkitPro install"
if [ -d "$DEVKITPRO_DIR" ] && [ -x "$DEVKITPRO_DIR/devkitA64/bin/aarch64-none-elf-gcc" ]; then
    bold "==> devkitPro already installed at $DEVKITPRO_DIR — nothing to do."
    "$DEVKITPRO_DIR/devkitA64/bin/aarch64-none-elf-gcc" --version | head -1
    bold "==> Verifying switch-dev is present"
    if [ -d "$DEVKITPRO_DIR/libnx" ]; then
        echo "  libnx: present"
    else
        err "libnx is missing from $DEVKITPRO_DIR. Run: sudo dkp-pacman -S switch-dev"
        exit 1
    fi
    exit 0
fi

bold "==> Detecting platform"
if ! command -v sudo >/dev/null 2>&1; then
    err "sudo is required but not found in PATH. Install sudo first."
    exit 1
fi

if ! command -v wget >/dev/null 2>&1; then
    err "wget is required but not found. Install with: sudo apt install -y wget"
    exit 1
fi

if [ ! -f /etc/debian_version ] && [ ! -f /etc/lsb-release ]; then
    err "This script targets Debian/Ubuntu/WSL. For other distros see https://devkitpro.org/wiki/devkitPro_pacman"
    exit 1
fi

# --- Step 1: install the devkitPro pacman wrapper -----------------------------
#
# We INLINE the install-devkitpro-pacman script's logic instead of fetching it.
# Reason: apt.devkitpro.org's Cloudflare returns 403 to wget's default UA, so
# fetching the script directly fails. The script itself is only 18 lines and
# the operations are stable, so inlining is more robust than fighting UA blocks.
# (The script is reproduced verbatim from https://apt.devkitpro.org/install-devkitpro-pacman
# as of devkitPro pacman v6.0.2.)
#
# This will:
#   1. Install apt-transport-https
#   2. Fetch devkitPro's GPG key with UA "dkp apt" (Cloudflare-friendly)
#   3. Add the devkitPro apt source (signed by that key)
#   4. apt update + install devkitpro-pacman

bold "==> Step 1+2: configure apt source + install devkitpro-pacman"
echo "    NOTE: requires sudo. Adds /etc/apt/sources.list.d/devkitpro.list and"
echo "    /usr/share/keyring/devkitpro-pub.gpg, then runs apt update + apt install."

sudo bash <<'INLINE_INSTALL'
set -e
apt-get install -y apt-transport-https

# Store devkitPro gpg key locally if we don't have it already
if ! [ -f /usr/share/keyring/devkitpro-pub.gpg ]; then
    mkdir -p /usr/share/keyring/
    wget -U "dkp apt" -O /usr/share/keyring/devkitpro-pub.gpg \
        https://apt.devkitpro.org/devkitpro-pub.gpg
fi

# Add the devkitPro apt repository if we don't have it set up already
if ! [ -f /etc/apt/sources.list.d/devkitpro.list ]; then
    echo "deb [signed-by=/usr/share/keyring/devkitpro-pub.gpg] https://apt.devkitpro.org stable main" \
        > /etc/apt/sources.list.d/devkitpro.list
fi

apt-get update
apt-get install -y devkitpro-pacman
INLINE_INSTALL

# --- Step 3: install switch-dev ----------------------------------------------

bold "==> Step 3: install switch-dev meta-package (devkitA64 + libnx + portlibs)"
echo "    This downloads ~3-5 GB. May take several minutes."
sudo dkp-pacman -Syu --noconfirm
sudo dkp-pacman -S --noconfirm switch-dev

# --- Step 4: verify ----------------------------------------------------------

bold "==> Step 4: verify install"
if [ ! -x "$DEVKITPRO_DIR/devkitA64/bin/aarch64-none-elf-gcc" ]; then
    err "devkitA64 compiler not found at $DEVKITPRO_DIR/devkitA64/bin/. Install may have failed."
    exit 1
fi

"$DEVKITPRO_DIR/devkitA64/bin/aarch64-none-elf-gcc" --version | head -1

# --- Step 5: shell integration ----------------------------------------------

SHELL_RC=""
if [ -n "${ZSH_VERSION:-}" ]; then SHELL_RC="$HOME/.zshrc"; fi
if [ -n "${BASH_VERSION:-}" ] || [ -z "$SHELL_RC" ]; then SHELL_RC="$HOME/.bashrc"; fi

bold "==> Step 5: shell environment"
if grep -q "DEVKITPRO" "$SHELL_RC" 2>/dev/null; then
    echo "    DEVKITPRO already configured in $SHELL_RC — skipping."
else
    cat >> "$SHELL_RC" <<'EOF'

# devkitPro (Switch homebrew toolchain)
export DEVKITPRO=/opt/devkitpro
export DEVKITARM=${DEVKITPRO}/devkitARM
export DEVKITPPC=${DEVKITPRO}/devkitPPC
export PATH=${DEVKITPRO}/tools/bin:$PATH
EOF
    bold "==> Added DEVKITPRO env to $SHELL_RC"
    echo "    Run: source $SHELL_RC  (or open a new shell) before running 'make'."
fi

bold "==> Done. Verify by running:"
echo "       source $SHELL_RC"
echo "       echo \$DEVKITPRO    # should print /opt/devkitpro"
echo "       cd /mnt/c/Users/Chase/dev/sx-doom-overlay && make"
