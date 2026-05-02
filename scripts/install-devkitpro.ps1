# =============================================================================
# install-devkitpro.ps1 -- install devkitPro for sx-doom-overlay on Windows.
#
# This is the Windows-native counterpart to scripts/install-devkitpro.sh
# (which is Linux/WSL only). One-time setup.
#
# What it does:
#   1. Detects an existing C:\devkitPro install and skips work if present.
#   2. Otherwise fetches the latest devkitProUpdater installer from
#      https://github.com/devkitPro/installer/releases
#   3. Launches the installer (interactive -- you click "Next" as normal).
#   4. Verifies the toolchain post-install.
#   5. Prints the next steps (open a fresh shell so the new PATH takes
#      effect, then `make` from the repo root).
#
# After this runs, `make`, `bash`, `git`, and the cross-toolchain are on
# the system PATH globally. You can build sx-doom-overlay from PowerShell,
# cmd, Windows Terminal, or the bundled "devkitPro MSys2" -- pick whatever
# shell is already open.
#
# Usage:
#   PS> Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   PS> .\scripts\install-devkitpro.ps1
#
# Or one-liner if execution policy is locked down:
#   PS> powershell.exe -ExecutionPolicy Bypass -File scripts\install-devkitpro.ps1
#
# Requires: Windows 10+ with PowerShell 5+. Internet. Admin rights for the
# installer step (UAC prompt is normal -- the installer needs them to write
# under C:\devkitPro and update PATH).
# =============================================================================

$ErrorActionPreference = 'Stop'

function Bold($msg) { Write-Host "==> $msg" -ForegroundColor Cyan }
function Warn($msg) { Write-Host "WARN: $msg" -ForegroundColor Yellow }
function Err ($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red }

$DevKitProDir = "C:\devkitPro"
$ToolchainGcc = Join-Path $DevKitProDir "devkitA64\bin\aarch64-none-elf-gcc.exe"

# -----------------------------------------------------------------------------
# Step 1: skip if already installed
# -----------------------------------------------------------------------------

Bold "Checking for existing devkitPro install"
if ((Test-Path $DevKitProDir) -and (Test-Path $ToolchainGcc)) {
    Bold "devkitPro already installed at $DevKitProDir"
    & $ToolchainGcc --version | Select-Object -First 1
    if (Test-Path (Join-Path $DevKitProDir "libnx")) {
        Write-Host "    libnx: present"
    } else {
        Warn "libnx is missing -- run the devkitPro installer and re-select 'switch-dev'."
        Write-Host "    Or from devkitPro MSys2: dkp-pacman -S switch-dev"
        exit 1
    }

    Bold "Verifying make + bash are on PATH"
    foreach ($tool in @('make','bash','git')) {
        $cmd = Get-Command $tool -ErrorAction SilentlyContinue
        if ($cmd) {
            Write-Host ("    {0,-5} {1}" -f $tool, $cmd.Source)
        } else {
            Warn ("$tool not on PATH -- open a fresh PowerShell window or " +
                  "log out / log in to pick up devkitPro's PATH entries.")
        }
    }
    Bold "Nothing to do -- devkitPro setup looks healthy."
    exit 0
}

# -----------------------------------------------------------------------------
# Step 2: fetch latest installer URL from GitHub Releases API
# -----------------------------------------------------------------------------

Bold "Resolving latest devkitPro Windows installer"
$Api = "https://api.github.com/repos/devkitPro/installer/releases/latest"
try {
    $rel = Invoke-RestMethod -Uri $Api -Headers @{ 'User-Agent' = 'sx-doom-overlay-installer' }
} catch {
    Err "Failed to query GitHub API: $($_.Exception.Message)"
    Err "Manually download from https://github.com/devkitPro/installer/releases and run the .exe."
    exit 1
}

$asset = $rel.assets | Where-Object { $_.name -match '^devkitProUpdater-.*\.exe$' } | Select-Object -First 1
if (-not $asset) {
    Err "No devkitProUpdater-*.exe asset on latest release '$($rel.tag_name)'."
    Err "Manually download from https://github.com/devkitPro/installer/releases."
    exit 1
}

$InstallerUrl  = $asset.browser_download_url
$InstallerName = $asset.name
$InstallerPath = Join-Path $env:TEMP $InstallerName

Write-Host "    Release: $($rel.tag_name) ($($rel.published_at.Substring(0,10)))"
Write-Host "    Asset:   $InstallerName ($([math]::Round($asset.size / 1MB, 1)) MB)"
Write-Host "    URL:     $InstallerUrl"

# -----------------------------------------------------------------------------
# Step 3: download
# -----------------------------------------------------------------------------

if ((Test-Path $InstallerPath) -and ((Get-Item $InstallerPath).Length -eq $asset.size)) {
    Bold "Installer already cached at $InstallerPath -- skipping download"
} else {
    Bold "Downloading installer"
    # Force TLS 1.2 -- older PS defaults can fail GitHub's TLS handshake.
    [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -Uri $InstallerUrl -OutFile $InstallerPath -UseBasicParsing
    Write-Host "    Saved to $InstallerPath"
}

# -----------------------------------------------------------------------------
# Step 4: run installer (interactive -- accept UAC prompt, click through)
# -----------------------------------------------------------------------------

Bold "Launching installer (UAC prompt expected)"
Write-Host "    When the wizard opens:"
Write-Host "      - Accept the default install location (C:\devkitPro)"
Write-Host "      - On the package selection screen, ensure 'switch-dev' is checked"
Write-Host "      - Let it finish (a few minutes -- pulls ~3-5 GB)"
Write-Host ""

$proc = Start-Process -FilePath $InstallerPath -PassThru -Wait
if ($proc.ExitCode -ne 0) {
    Warn "Installer exited with code $($proc.ExitCode)."
    Warn "If you cancelled the wizard intentionally, that's fine -- re-run the script."
    Warn "Otherwise, check $InstallerPath manually."
    exit $proc.ExitCode
}

# -----------------------------------------------------------------------------
# Step 5: post-install verification
# -----------------------------------------------------------------------------

Bold "Verifying install"
if (-not (Test-Path $ToolchainGcc)) {
    Err "Expected $ToolchainGcc but it's not there."
    Err "Re-run the installer and ensure 'switch-dev' was selected on the package page."
    exit 1
}
& $ToolchainGcc --version | Select-Object -First 1

if (-not (Test-Path (Join-Path $DevKitProDir "libnx"))) {
    Err "libnx is missing -- you skipped the 'switch-dev' package."
    Err "Re-run the installer (or from devkitPro MSys2: dkp-pacman -S switch-dev)."
    exit 1
}
Write-Host "    libnx: present"

# -----------------------------------------------------------------------------
# Step 6: next steps
# -----------------------------------------------------------------------------

Bold "Done"
Write-Host ""
Write-Host "Open a NEW PowerShell window (so PATH updates pick up), cd to this repo, and:"
Write-Host ""
Write-Host "    git submodule update --init --recursive" -ForegroundColor Green
Write-Host "    bash scripts\fetch-freedoom.sh           # optional -- bundled WAD" -ForegroundColor Green
Write-Host "    make" -ForegroundColor Green
Write-Host ""
Write-Host "The build embeds the git branch + hash into the .ovl. After deploying"
Write-Host "to the SD, the in-overlay debug line shows 'build: <branch>@<hash>+'"
Write-Host "so you can verify the running binary matches your source tree."
