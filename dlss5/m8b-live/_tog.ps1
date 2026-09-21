param([string]$Id = "2")
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class T32 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
}
"@
$found = [IntPtr]::Zero
[void][T32]::EnumWindows({ param($h,$l)
    $sb = New-Object System.Text.StringBuilder 256
    [void][T32]::GetWindowTextW($h, $sb, 256)
    if ($sb.ToString() -like "DLSS5_INTEL*") { $script:found = $h }
    return $true
}, [IntPtr]::Zero)
if ($found -eq [IntPtr]::Zero) { Write-Output "m8b window NOT found"; exit 1 }
[void][T32]::PostMessageW($found, 0x0312, [IntPtr][int]$Id, [IntPtr]::Zero)
Write-Output ("posted WM_HOTKEY id=" + $Id + " to hwnd=" + $found)
