# mtp-sync.ps1 — push/pull files to a Switch SD via MTP (PowerShell + Shell COM).
#
# Used by scripts/sync-sd.sh when the user has the Switch in DBI/MTP mode
# (shows up in Explorer as "This PC\Nintendo Switch\SD Card\..." rather
# than as a drive letter that WSL can see at /mnt/<x>/).
#
# Usage from WSL:
#   powershell.exe -ExecutionPolicy Bypass -File scripts/mtp-sync.ps1 -Action push -LocalPath C:\path\to\file.ovl -RemotePath "switch/.overlays/sx-doom-overlay.ovl"
#   powershell.exe -ExecutionPolicy Bypass -File scripts/mtp-sync.ps1 -Action pull -RemotePath "config/sx-doom-overlay/trace.log" -LocalPath C:\path\to\trace.log
#
# Licensed under GPLv2.

param(
    [Parameter(Mandatory = $true)] [ValidateSet("push", "pull", "list")] [string] $Action,
    [string] $LocalPath  = "",
    [string] $RemotePath = "",
    [string] $DeviceName = "Nintendo Switch",
    [string] $RootName   = "SD Card"
)

$ErrorActionPreference = "Stop"

# --- Locate the Switch MTP device + root --------------------------------------

$shell = New-Object -ComObject Shell.Application

# 0x11 = ssfDRIVES (My Computer / This PC)
$thisPC = $shell.NameSpace(0x11)
if ($null -eq $thisPC) { throw "Could not access This PC namespace" }

$device = $thisPC.Items() | Where-Object { $_.Name -eq $DeviceName }
if ($null -eq $device) {
    Write-Host "Available devices in This PC:" -ForegroundColor Yellow
    $thisPC.Items() | ForEach-Object { Write-Host "  - $($_.Name)" }
    throw "Device '$DeviceName' not found. Make sure Switch is in DBI/MTP mode."
}

$deviceFolder = $device.GetFolder
if ($null -eq $deviceFolder) { throw "Device '$DeviceName' has no folder (not connected?)" }

$root = $deviceFolder.Items() | Where-Object { $_.Name -eq $RootName }
if ($null -eq $root) {
    Write-Host "Available items under '$DeviceName':" -ForegroundColor Yellow
    $deviceFolder.Items() | ForEach-Object { Write-Host "  - $($_.Name)" }
    throw "Item '$RootName' not found under '$DeviceName'."
}

# --- Helper: walk a path under the SD Card root -------------------------------

function Get-MtpFolder {
    param([Parameter(Mandatory = $true)] $RootFolderItem,
          [Parameter(Mandatory = $true)] [string] $Path,
          [bool] $Create = $false)
    $current = $RootFolderItem.GetFolder
    if ($null -eq $current) { throw "Root has no folder accessor" }
    $parts = $Path -split '[\\/]+' | Where-Object { $_ -ne "" }
    foreach ($p in $parts) {
        $next = $current.Items() | Where-Object { $_.Name -eq $p }
        if ($null -eq $next) {
            if ($Create) {
                # MTP folder creation via Shell.Application is finicky; in
                # practice we only ever push to existing dirs (switch/.overlays
                # exists, config/sx-doom-overlay is created by our overlay on
                # first run). Document and bail.
                throw "Path component '$p' not found under '$($current.Self.Path)' and auto-create is unreliable on MTP. Create the folder via the overlay first run, or via Explorer."
            }
            return $null
        }
        $current = $next.GetFolder
        if ($null -eq $current) { return $next }  # leaf file
    }
    return $current
}

# --- Action dispatch ----------------------------------------------------------

switch ($Action) {

    "list" {
        $folder = Get-MtpFolder -RootFolderItem $root -Path $RemotePath
        if ($null -eq $folder) { throw "Path not found: $RemotePath" }
        if ($folder -is [__ComObject] -and $folder.Items) {
            $folder.Items() | ForEach-Object { Write-Host "  $($_.Name)  ($($_.Size) B)" }
        } else {
            Write-Host "  (leaf): $($folder.Name)"
        }
    }

    "push" {
        if (-not (Test-Path -LiteralPath $LocalPath)) { throw "Local file not found: $LocalPath" }
        # Split RemotePath into directory + filename
        $remoteDir  = Split-Path $RemotePath -Parent
        $remoteName = Split-Path $RemotePath -Leaf
        $destFolder = Get-MtpFolder -RootFolderItem $root -Path $remoteDir
        if ($null -eq $destFolder) { throw "Remote dir not found: $remoteDir (Make sure the dirs exist on SD already.)" }
        # If destination file already exists, delete it first so CopyHere doesn't prompt.
        $existing = $destFolder.Items() | Where-Object { $_.Name -eq $remoteName }
        if ($null -ne $existing) {
            Write-Host "  (replacing existing $remoteName)" -ForegroundColor DarkGray
            $existing.InvokeVerb("delete")
            Start-Sleep -Milliseconds 500
        }
        Write-Host "  pushing $LocalPath → $RemotePath ..." -ForegroundColor Cyan
        # 0x14 = no-prompt + replace silently
        $destFolder.CopyHere((Get-Item -LiteralPath $LocalPath).FullName, 0x14)
        # CopyHere is async — wait until the file appears at the destination.
        $deadline = (Get-Date).AddSeconds(60)
        do {
            Start-Sleep -Milliseconds 500
            $check = $destFolder.Items() | Where-Object { $_.Name -eq $remoteName }
        } while ($null -eq $check -and (Get-Date) -lt $deadline)
        if ($null -eq $check) { throw "Push timed out (60s). File may not have copied." }
        Write-Host "  → done ($($check.Size) bytes)"
    }

    "pull" {
        if ([string]::IsNullOrEmpty($LocalPath)) { throw "-LocalPath required for pull" }
        $remoteDir  = Split-Path $RemotePath -Parent
        $remoteName = Split-Path $RemotePath -Leaf
        $srcFolder  = Get-MtpFolder -RootFolderItem $root -Path $remoteDir
        if ($null -eq $srcFolder) { throw "Remote dir not found: $remoteDir" }
        $srcItem = $srcFolder.Items() | Where-Object { $_.Name -eq $remoteName }
        if ($null -eq $srcItem) {
            Write-Host "  (file not present on SD: $RemotePath)"
            exit 2
        }
        # CopyHere into a temp directory, then move/rename into the requested path.
        $localDir  = Split-Path $LocalPath -Parent
        $localName = Split-Path $LocalPath -Leaf
        if (-not (Test-Path $localDir)) { New-Item -ItemType Directory -Force -Path $localDir | Out-Null }
        $localFolder = $shell.NameSpace($localDir)
        if ($null -eq $localFolder) { throw "Could not access local dir: $localDir" }
        Write-Host "  pulling $RemotePath → $LocalPath ..." -ForegroundColor Cyan
        $localFolder.CopyHere($srcItem, 0x14)
        # Wait for the file to appear locally with the source's name, then rename if needed
        $deadline = (Get-Date).AddSeconds(60)
        $copiedPath = Join-Path $localDir $remoteName
        do {
            Start-Sleep -Milliseconds 500
        } while (-not (Test-Path -LiteralPath $copiedPath) -and (Get-Date) -lt $deadline)
        if (-not (Test-Path -LiteralPath $copiedPath)) { throw "Pull timed out (60s)." }
        if ($remoteName -ne $localName) {
            Move-Item -Force -LiteralPath $copiedPath -Destination $LocalPath
        }
        Write-Host "  → done ($((Get-Item -LiteralPath $LocalPath).Length) bytes)"
    }
}
