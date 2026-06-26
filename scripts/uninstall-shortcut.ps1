<#
.SYNOPSIS
    Uninstall the Start Menu shortcut for Bloom Terminal.

.DESCRIPTION
    Removes the .lnk shortcut from the user's Start Menu Programs folder.
    Called from `make uninstall` on Windows (HOST_WINDOWS).

.EXAMPLE
    powershell -NoProfile -File uninstall-shortcut.ps1
#>

$ErrorActionPreference = "Stop"

$startMenu = [System.Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenu "Bloom Terminal.lnk"

if (Test-Path $shortcutPath) {
    Remove-Item $shortcutPath
    Write-Host "Removed Start Menu shortcut: $shortcutPath"
} else {
    Write-Host "Start Menu shortcut not found (already removed): $shortcutPath"
}
