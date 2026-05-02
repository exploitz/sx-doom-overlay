# =============================================================================
# setup-windows.ps1 -- one-shot bootstrap for a fresh Windows machine.
#
# Runs the full setup pipeline so a contributor can go from a blank
# Windows install to a working sx-doom-overlay build in one command:
#
#   1. install-devkitpro.ps1     -- installs the toolchain to C:\devkitPro
#   2. git submodule update      -- pulls libultrahand + doomgeneric
#   3. bash scripts\fetch-freedoom.sh  -- optional bundled WAD (only if
#                                         data\wads\freedoom1.wad missing)
#   4. make                      -- produces out-win\sx-doom-overlay.ovl
#
# Usage:
#   PS> Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
#   PS> .\scripts\setup-windows.ps1
#
# Or one-liner:
#   PS> powershell.exe -ExecutionPolicy Bypass -File scripts\setup-windows.ps1
#
# Skip individual steps if you've already done them by hand:
#   PS> .\scripts\setup-windows.ps1 -SkipDevKitPro -SkipFreedoom
# =============================================================================

[CmdletBinding()]
param(
    [switch]$SkipDevKitPro,
    [switch]$SkipSubmodules,
    [switch]$SkipFreedoom,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'

function Bold($msg) { Write-Host ""; Write-Host "==> $msg" -ForegroundColor Cyan }
function Err ($msg) { Write-Host "ERROR: $msg" -ForegroundColor Red }

# Resolve repo root from script location (works when invoked from any cwd).
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Definition
$Root      = Split-Path -Parent $ScriptDir
Set-Location $Root

# -----------------------------------------------------------------------------
# Step 1: devkitPro
# -----------------------------------------------------------------------------

if ($SkipDevKitPro) {
    Bold "Skipping devkitPro install (--SkipDevKitPro)"
} else {
    Bold "Step 1/4 -- devkitPro toolchain"
    & (Join-Path $ScriptDir "install-devkitpro.ps1")
    if ($LASTEXITCODE -ne 0) {
        Err "install-devkitpro.ps1 failed (exit $LASTEXITCODE). Stopping."
        exit $LASTEXITCODE
    }
}

# -----------------------------------------------------------------------------
# Verify the tools we need from this point onward are on PATH. If devkitPro
# was just installed in this session, PATH won't update until the user opens
# a new shell -- bail with a clear message rather than puking later.
# -----------------------------------------------------------------------------

$missing = @()
foreach ($tool in @('make','bash','git')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { $missing += $tool }
}
if ($missing.Count -gt 0) {
    Err ("Required tool(s) not on PATH yet: " + ($missing -join ', '))
    Err "If you JUST installed devkitPro, close this PowerShell window and open a new one."
    Err "Then re-run this script with -SkipDevKitPro to pick up where you left off:"
    Err "    .\scripts\setup-windows.ps1 -SkipDevKitPro"
    exit 1
}

# -----------------------------------------------------------------------------
# Step 2: submodules
# -----------------------------------------------------------------------------

if ($SkipSubmodules) {
    Bold "Skipping submodule update (--SkipSubmodules)"
} else {
    Bold "Step 2/4 -- git submodule update"
    & git submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { Err "submodule update failed"; exit $LASTEXITCODE }
}

# -----------------------------------------------------------------------------
# Step 3: bundled WAD (optional -- only if missing)
# -----------------------------------------------------------------------------

$FreedoomWad = Join-Path $Root "data\wads\freedoom1.wad"
if ($SkipFreedoom) {
    Bold "Skipping Freedoom fetch (--SkipFreedoom)"
} elseif (Test-Path $FreedoomWad) {
    Bold "Step 3/4 -- Freedoom already at $FreedoomWad, skipping"
} else {
    Bold "Step 3/4 -- fetching Freedoom 1 (BSD-3, ~29 MB)"
    & bash (Join-Path "scripts" "fetch-freedoom.sh")
    if ($LASTEXITCODE -ne 0) {
        Err "fetch-freedoom.sh failed. You can still build without it -- the"
        Err "engine just won't have a default WAD bundled into release zips."
        Err "Re-run with -SkipFreedoom to continue past this step."
        exit $LASTEXITCODE
    }
}

# -----------------------------------------------------------------------------
# Step 4: build
# -----------------------------------------------------------------------------

if ($SkipBuild) {
    Bold "Skipping build (--SkipBuild) -- done."
    exit 0
}

Bold "Step 4/4 -- make"
& make
if ($LASTEXITCODE -ne 0) { Err "make failed"; exit $LASTEXITCODE }

# Makefile auto-detects PLATFORM via uname; from PowerShell + devkitPro
# bundled make, that resolves to MINGW* -> 'win'. Mirror the same logic
# here so we can locate the freshly-built .ovl.
$OvlPath = Join-Path $Root "out-win\sx-doom-overlay.ovl"
if (-not (Test-Path $OvlPath)) {
    # Fall back to legacy out/ path in case the Makefile's namespacing
    # ever gets disabled (or for dirs predating this change).
    $OvlPath = Join-Path $Root "out\sx-doom-overlay.ovl"
}
if (Test-Path $OvlPath) {
    $size = [math]::Round((Get-Item $OvlPath).Length / 1KB, 0)
    Bold "Done -- built $OvlPath ($size KB)"
    Write-Host ""
    Write-Host "Deploy with:" -ForegroundColor Green
    Write-Host "    bash scripts\sync-sd.sh     # SD card mounted as drive letter" -ForegroundColor Green
    Write-Host ""
    Write-Host "Or copy manually to:" -ForegroundColor Green
    Write-Host "    <SD>:\switch\.overlays\sx-doom-overlay.ovl" -ForegroundColor Green
} else {
    Err "make reported success but $OvlPath is missing -- investigate."
    exit 1
}
