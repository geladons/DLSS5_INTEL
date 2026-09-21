Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32Enum {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
$proc = Get-Process m8blive -ErrorAction SilentlyContinue
if ($proc) { Write-Output ("m8blive PID: " + $proc.Id) } else { Write-Output "no m8blive process" }
[Win32Enum]::EnumWindows({ param($h,$l)
    $sb = New-Object System.Text.StringBuilder 256
    [void][Win32Enum]::GetWindowTextW($h, $sb, 256)
    $t = $sb.ToString()
    if ($t -like "*m8b*") {
        $pid2 = 0
        [void][Win32Enum]::GetWindowThreadProcessId($h, [ref]$pid2)
        $r = New-Object Win32Enum+RECT
        [void][Win32Enum]::GetWindowRect($h, [ref]$r)
        $vis = [Win32Enum]::IsWindowVisible($h)
        Write-Output ("window: '$t' pid=$pid2 visible=$vis rect=$($r.Left),$($r.Top)-$($r.Right),$($r.Bottom)")
    }
    return $true
}, [IntPtr]::Zero) | Out-Null
