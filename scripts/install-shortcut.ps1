<# 
.SYNOPSIS
    Install a Start Menu shortcut for Bloom Terminal.

.DESCRIPTION
    Creates a .lnk shortcut in the user's Start Menu Programs folder that
    points to the bloom-terminal executable. The shortcut icon is extracted
    from the executable itself (which embeds the ICO via windres).

    Called from `make install` on Windows (HOST_WINDOWS).

.PARAMETER ExePath
    Full path to bloom-terminal.exe (e.g. C:\Users\...\.local\bin\bloom-terminal.exe)

.PARAMETER IconPath
    Full path to bloom-terminal.ico (e.g. C:\Users\...\.local\share\icons\...\bloom-terminal.ico)

.EXAMPLE
    powershell -NoProfile -File install-shortcut.ps1 \
        -ExePath "$HOME/.local/bin/bloom-terminal.exe" \
        -IconPath "$HOME/.local/share/icons/hicolor/256x256/apps/bloom-terminal.png"
#>
param(
    [Parameter(Mandatory=$true)][string]$ExePath,
    [Parameter(Mandatory=$true)][string]$IconPath
)

$ErrorActionPreference = "Stop"

# Resolve the user's Start Menu Programs folder
$startMenu = [System.Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenu "Bloom Terminal.lnk"

# Create the shortcut via the Shell COM object
$shell = New-Object -ComObject WScript.Shell
$lnk = $shell.CreateShortcut($shortcutPath)
$lnk.TargetPath = $ExePath
$lnk.IconLocation = "$IconPath,0"
$lnk.Description = "Bloom Terminal"
$lnk.Save()

Write-Host "Installed Start Menu shortcut: $shortcutPath"
