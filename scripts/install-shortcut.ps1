<#
.SYNOPSIS
    Install a Start Menu shortcut for Portty.

.DESCRIPTION
    Creates a .lnk shortcut in the user's Start Menu Programs folder that
    points to the windowed launcher (porttyw.exe), so clicking it opens the
    terminal without a console window. The shortcut icon is extracted from
    the executable itself (the ICO embedded via windres).

    The shortcut also carries the app's AppUserModelID as its
    System.AppUserModel.ID property. That property is the whole reason this
    script works through IShellLink/IPropertyStore rather than WScript.Shell,
    which cannot set shortcut properties: the taskbar matches a running window
    to a shortcut carrying the same ID, so "Pin to taskbar" pins *this*
    shortcut — a porttyw.exe launch — and the pinned button stacks with the
    running terminal instead of drawing a second, unpinned button. The value
    must match PORTTY_APP_USER_MODEL_ID (src/portty_appmodel.h); a mismatch
    shows up as a duplicate taskbar button.

    Called from `make install` on Windows (HOST_WINDOWS).

.PARAMETER ExePath
    Full path to porttyw.exe (e.g. C:\Users\...\.local\bin\porttyw.exe)

.EXAMPLE
    powershell -NoProfile -File install-shortcut.ps1 \
        -ExePath "$HOME/.local/bin/porttyw.exe"
#>
param(
    [Parameter(Mandatory = $true)][string]$ExePath
)

$ErrorActionPreference = "Stop"

# Keep in step with PORTTY_APP_USER_MODEL_ID in src/portty_appmodel.h.
$appUserModelId = "Chestso.Portty"
$displayName = "Portty"

# WScript.Shell can set a shortcut's target, icon and description but not its
# property store, and System.AppUserModel.ID lives in the property store, so the
# interop below exists to write that one property.
Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

[StructLayout(LayoutKind.Sequential, Pack = 4)]
internal struct PROPERTYKEY
{
    public Guid fmtid;
    public uint pid;
}

// Only the VT_LPWSTR arm of the PROPVARIANT union is ever used here; the union
// starts 8 bytes in on both architectures.
[StructLayout(LayoutKind.Explicit)]
internal struct PROPVARIANT
{
    [FieldOffset(0)] public ushort vt;
    [FieldOffset(8)] public IntPtr pwszVal;
}

[ComImport, Guid("000214F9-0000-0000-C000-000000000046"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IShellLinkW
{
    void GetPath([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszFile, int cch, IntPtr pfd, uint fFlags);
    void GetIDList(out IntPtr ppidl);
    void SetIDList(IntPtr pidl);
    void GetDescription([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszName, int cch);
    void SetDescription([MarshalAs(UnmanagedType.LPWStr)] string pszName);
    void GetWorkingDirectory([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszDir, int cch);
    void SetWorkingDirectory([MarshalAs(UnmanagedType.LPWStr)] string pszDir);
    void GetArguments([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszArgs, int cch);
    void SetArguments([MarshalAs(UnmanagedType.LPWStr)] string pszArgs);
    void GetHotkey(out short pwHotkey);
    void SetHotkey(short wHotkey);
    void GetShowCmd(out int piShowCmd);
    void SetShowCmd(int iShowCmd);
    void GetIconLocation([Out, MarshalAs(UnmanagedType.LPWStr)] StringBuilder pszIconPath, int cch, out int piIcon);
    void SetIconLocation([MarshalAs(UnmanagedType.LPWStr)] string pszIconPath, int iIcon);
    void SetRelativePath([MarshalAs(UnmanagedType.LPWStr)] string pszPathRel, uint dwReserved);
    void Resolve(IntPtr hwnd, uint fFlags);
    void SetPath([MarshalAs(UnmanagedType.LPWStr)] string pszFile);
}

[ComImport, Guid("0000010b-0000-0000-C000-000000000046"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IPersistFile
{
    void GetClassID(out Guid pClassID);
    [PreserveSig] int IsDirty();
    void Load([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, uint dwMode);
    void Save([MarshalAs(UnmanagedType.LPWStr)] string pszFileName, [MarshalAs(UnmanagedType.Bool)] bool fRemember);
    void SaveCompleted([MarshalAs(UnmanagedType.LPWStr)] string pszFileName);
    void GetCurFile([MarshalAs(UnmanagedType.LPWStr)] out string ppszFileName);
}

[ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"),
 InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IPropertyStore
{
    void GetCount(out uint cProps);
    void GetAt(uint iProp, out PROPERTYKEY pkey);
    [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT pv);
    void SetValue(ref PROPERTYKEY key, ref PROPVARIANT pv);
    void Commit();
}

public static class PorttyShortcut
{
    private const ushort VT_LPWSTR = 31;

    private static PROPERTYKEY AppUserModelIdKey()
    {
        PROPERTYKEY key;
        key.fmtid = new Guid("9F4C2855-9F79-4B39-A8D0-E1D42DE1D5F3");
        key.pid = 5;    // PKEY_AppUserModel_ID
        return key;
    }

    private static object NewShellLink()
    {
        Type t = Type.GetTypeFromCLSID(new Guid("00021401-0000-0000-C000-000000000046"));
        return Activator.CreateInstance(t);
    }

    public static void Create(string linkPath, string targetPath, string description, string appUserModelId)
    {
        object shellLink = NewShellLink();
        try
        {
            IShellLinkW link = (IShellLinkW)shellLink;
            link.SetPath(targetPath);
            link.SetIconLocation(targetPath, 0);
            link.SetDescription(description);
            link.SetWorkingDirectory(System.IO.Path.GetDirectoryName(targetPath));

            PROPERTYKEY key = AppUserModelIdKey();
            PROPVARIANT value;
            value.vt = VT_LPWSTR;
            value.pwszVal = Marshal.StringToCoTaskMemUni(appUserModelId);
            try
            {
                IPropertyStore store = (IPropertyStore)shellLink;
                store.SetValue(ref key, ref value);
                store.Commit();
            }
            finally
            {
                Marshal.FreeCoTaskMem(value.pwszVal);
            }

            ((IPersistFile)shellLink).Save(linkPath, true);
        }
        finally
        {
            Marshal.ReleaseComObject(shellLink);
        }
    }
}
"@

# Resolve the user's Start Menu Programs folder
$startMenu = [System.Environment]::GetFolderPath('Programs')
$shortcutPath = Join-Path $startMenu "Portty.lnk"
$targetPath = (Resolve-Path -LiteralPath $ExePath).Path

[PorttyShortcut]::Create($shortcutPath, $targetPath, $displayName, $appUserModelId)
Write-Host "Installed Start Menu shortcut: $shortcutPath"

# A pin is a shortcut in its own right, created by Explorer at pin time and
# never touched by this installer, so a pin made before the identity existed
# keeps pointing at portty.exe (console subsystem). Say so rather than leave it
# to be discovered as a console window that should not be there.
#
# The reads go through the shell's own property system (System.Link.
# TargetParsingPath, System.AppUserModel.ID) rather than more IShellLink
# interop, which keeps this check to the two properties it needs.
$pinnedDir = Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'
if (Test-Path -LiteralPath $pinnedDir) {
    $folder = (New-Object -ComObject Shell.Application).Namespace($pinnedDir)
    if ($null -ne $folder) {
        foreach ($item in @($folder.Items())) {
            try {
                $target = $item.ExtendedProperty('System.Link.TargetParsingPath')
                if ($null -eq $target -or $target -notlike '*portty*.exe') { continue }
                $id = $item.ExtendedProperty('System.AppUserModel.ID')
                if ($target -ne $targetPath -or $id -ne $appUserModelId) {
                    Write-Warning ("Pinned taskbar shortcut '{0}' is stale: it runs {1} instead of {2}. Unpin it and pin the running window again." -f $item.Name, $target, $targetPath)
                }
            } catch {
                Write-Warning ("Could not read the pinned shortcut '{0}': {1}" -f $item.Name, $_)
            }
        }
    }
}
