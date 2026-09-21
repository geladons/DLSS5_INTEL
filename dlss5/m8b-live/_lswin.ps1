Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class E32 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
"@
[void][E32]::EnumWindows({ param($h,$l)
    if (-not [E32]::IsWindowVisible($h)) { return $true }
    $sb = New-Object System.Text.StringBuilder 256
    [void][E32]::GetWindowTextW($h, $sb, 256)
    $t = $sb.ToString()
    if ([string]::IsNullOrWhiteSpace($t)) { return $true }
    $cb = New-Object System.Text.StringBuilder 128
    [void][E32]::GetClassNameW($h, $cb, 128)
    $r = New-Object E32+RECT
    [void][E32]::GetWindowRect($h, [ref]$r)
    Write-Output ("{0,-8} [{1}] '{2}'  at {3},{4}  {5}x{6}" -f $h, $cb.ToString(), $t, $r.Left, $r.Top, ($r.Right-$r.Left), ($r.Bottom-$r.Top))
    return $true
}, [IntPtr]::Zero) | Out-Null
