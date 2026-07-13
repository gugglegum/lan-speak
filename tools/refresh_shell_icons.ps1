Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class ShellNotify {
    [DllImport("shell32.dll")]
    public static extern void SHChangeNotify(int wEventId, uint uFlags, IntPtr dwItem1, IntPtr dwItem2);
}
"@

[ShellNotify]::SHChangeNotify(0x08000000, 0, [IntPtr]::Zero, [IntPtr]::Zero)
& "$env:windir\System32\ie4uinit.exe" -show
