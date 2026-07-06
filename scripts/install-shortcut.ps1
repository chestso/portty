<# 
.SYNOPSIS
    Install a Start Menu shortcut for Portty.

.DESCRIPTION
    Creates a .lnk shortcut in the user's Start Menu Programs folder that
    points to the portty executable. The shortcut icon is extracted from
    the executable itself (the ICO embedded via windres).

    Called from `make install` on Windows (HOST_WINDOWS).

.PARAMETER ExePath
    Full path to portty.exe (e.g. C:\Users\...\.local\bin\portty.exe)

.EXAMPLE
    powershell -NoProfile -File install-shortcut.ps1 \
        -ExePath "$HOME/.local/bin/portty.exe"
#>
param(
    [Parameter(Mandatory=$true)][string]$ExePath
)

$ErrorActionPreference = "Stop"

# Resolve the user's Start Menu Programs folder
$startMenu = [System.Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenu "Portty.lnk"

# Create the shortcut via the Shell COM object
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcutPath)
$lnk.TargetPath = $ExePath
$lnk.IconLocation = "$ExePath,0"
$lnk.Description = "Portty"
$lnk.Save()

Write-Host "Installed Start Menu shortcut: $shortcutPath"
