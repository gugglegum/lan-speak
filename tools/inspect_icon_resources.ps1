param(
    [Parameter(Mandatory = $true)]
    [string[]]$Path
)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;
using System.Text;

public static class NativeResources {
    public delegate bool EnumResNameProc(IntPtr hModule, IntPtr lpszType, IntPtr lpszName, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr LoadLibraryEx(string lpFileName, IntPtr hFile, uint dwFlags);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern bool FreeLibrary(IntPtr hModule);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern bool EnumResourceNames(IntPtr hModule, IntPtr lpszType, EnumResNameProc lpEnumFunc, IntPtr lParam);

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    public static extern IntPtr FindResource(IntPtr hModule, IntPtr lpName, IntPtr lpType);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LoadResource(IntPtr hModule, IntPtr hResInfo);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern IntPtr LockResource(IntPtr hResData);

    [DllImport("kernel32.dll", SetLastError = true)]
    public static extern uint SizeofResource(IntPtr hModule, IntPtr hResInfo);
}
"@

function New-IntResource([int]$id) {
    return [IntPtr]$id
}

function Get-ResourceNameText([IntPtr]$name) {
    if (($name.ToInt64() -band 0xffff0000L) -eq 0) {
        return "#" + $name.ToInt64()
    }
    return [Runtime.InteropServices.Marshal]::PtrToStringUni($name)
}

function Get-ResourceBytes([IntPtr]$module, [IntPtr]$type, [IntPtr]$name) {
    $resource = [NativeResources]::FindResource($module, $name, $type)
    if ($resource -eq [IntPtr]::Zero) {
        return $null
    }
    $size = [NativeResources]::SizeofResource($module, $resource)
    $loaded = [NativeResources]::LoadResource($module, $resource)
    $locked = [NativeResources]::LockResource($loaded)
    $bytes = New-Object byte[] $size
    [Runtime.InteropServices.Marshal]::Copy($locked, $bytes, 0, $size)
    return $bytes
}

function Get-IconResourceSummary([string]$file) {
    $module = [NativeResources]::LoadLibraryEx((Resolve-Path $file), [IntPtr]::Zero, 0x00000002)
    if ($module -eq [IntPtr]::Zero) {
        throw "LoadLibraryEx failed for $file"
    }

    try {
        foreach ($typeId in 3, 14) {
            $type = New-IntResource $typeId
            $names = New-Object System.Collections.Generic.List[IntPtr]
            $callback = [NativeResources+EnumResNameProc]{
                param([IntPtr]$hModule, [IntPtr]$lpszType, [IntPtr]$lpszName, [IntPtr]$lParam)
                $names.Add($lpszName)
                return $true
            }
            [void][NativeResources]::EnumResourceNames($module, $type, $callback, [IntPtr]::Zero)

            foreach ($name in $names) {
                $bytes = Get-ResourceBytes $module $type $name
                $hash = [System.BitConverter]::ToString(
                    [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
                ).Replace("-", "")
                [PSCustomObject]@{
                    File = [IO.Path]::GetFileName($file)
                    Type = if ($typeId -eq 3) { "RT_ICON" } else { "RT_GROUP_ICON" }
                    Name = Get-ResourceNameText $name
                    Size = $bytes.Length
                    Sha256 = $hash
                }
            }
        }
    } finally {
        [void][NativeResources]::FreeLibrary($module)
    }
}

foreach ($file in $Path) {
    Get-IconResourceSummary $file
}
