param([string]$Mode = "start")
Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class P32 {
    public delegate bool EnumWindowsProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder sb, int max);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hh, IntPtr after, int x, int y, int cx, int cy, uint flags);
}
"@
if ($Mode -eq "start") {
    $existing = Get-Process notepad -ErrorAction SilentlyContinue
    if (-not $existing) { Start-Process notepad | Out-Null; Start-Sleep -Milliseconds 1200 }
    $np = [IntPtr]::Zero
    [void][P32]::EnumWindows({ param($h,$l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][P32]::GetWindowTextW($h, $sb, 256)
        if ($sb.ToString() -like "*Notepad*") { $script:np = $h }
        return $true
    }, [IntPtr]::Zero)
    if ($np -eq [IntPtr]::Zero) { Write-Output "notepad window NOT found"; exit 1 }
    [void][P32]::SetWindowPos($np, [IntPtr]::Zero, 150, 150, 700, 500, 0x0040)
    Write-Output ("notepad hwnd=" + $np + " placed at 150,150 700x500")
} elseif ($Mode -eq "move") {
    $np = [IntPtr]::Zero
    [void][P32]::EnumWindows({ param($h,$l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][P32]::GetWindowTextW($h, $sb, 256)
        if ($sb.ToString() -like "*Notepad*") { $script:np = $h }
        return $true
    }, [IntPtr]::Zero)
    if ($np -eq [IntPtr]::Zero) { Write-Output "notepad window NOT found"; exit 1 }
    [void][P32]::SetWindowPos($np, [IntPtr]::Zero, 950, 480, 700, 500, 0x0040)
    Write-Output ("notepad hwnd=" + $np + " moved to 950,480")
} elseif ($Mode -eq "home") {
    $np = [IntPtr]::Zero
    [void][P32]::EnumWindows({ param($h,$l)
        $sb = New-Object System.Text.StringBuilder 256
        [void][P32]::GetWindowTextW($h, $sb, 256)
        if ($sb.ToString() -like "*Notepad*") { $script:np = $h }
        return $true
    }, [IntPtr]::Zero)
    if ($np -eq [IntPtr]::Zero) { Write-Output "notepad window NOT found"; exit 1 }
    [void][P32]::SetWindowPos($np, [IntPtr]::Zero, 150, 150, 700, 500, 0x0040)
    Write-Output ("notepad hwnd=" + $np + " moved home to 150,150")
} else {
    Stop-Process -Name notepad -Force -ErrorAction SilentlyContinue
    Write-Output "notepad killed"
}
