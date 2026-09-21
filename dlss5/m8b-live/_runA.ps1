# TASK A harness: verify toggle-resume fix end-to-end (no EnumWindows callbacks here).
# Enumerate via ui.ps1 windows -All -Json; toggle via PostMessageW(0x0312, wParam=2);
# move the CONSOLE window via SetWindowPos. ASCII only.
param()
$ErrorActionPreference = 'Stop'
$ui  = 'C:\Users\AI\.kimi_openclaw\workspace\skills\win-desktop-control\scripts\ui.ps1'
$exe = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release\m8blive.exe'
$wd  = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\build\Release'
$outdir = 'C:\Users\AI\Desktop\DLSS5_INTEL\docs'
$alog   = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\out\m8b_A.log'
$aerr   = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m8b-live\out\m8b_A.err'
if (Test-Path $alog) { Remove-Item $alog -Force }
if (Test-Path $aerr) { Remove-Item $aerr -Force }

Add-Type @"
using System;
using System.Text;
using System.Runtime.InteropServices;
public class H32 {
    public struct RECT { public int Left; public int Top; public int Right; public int Bottom; }
    [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr hWnd, uint Msg, IntPtr wParam, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hIns, int X, int Y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
}
"@

function Ui([string[]]$a) {
    $out = & powershell -NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File $ui @a 2>$null
    return ($out -join "`n")
}
function Shot([string]$name) {
    $p = Join-Path $outdir $name
    if (Test-Path $p) { Remove-Item $p -Force }
    Ui @('screenshot','-Out',$p,'-Scale','1.0') | Out-Null
    return $p
}
function RectOf([IntPtr]$h) {
    $r = New-Object H32+RECT
    [void][H32]::GetWindowRect($h, [ref]$r)
    return "($($r.Left),$($r.Top))-($($r.Right),$($r.Bottom))"
}
function MoveWin([IntPtr]$h, [int]$x, [int]$y) {
    # SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE = 1|4|0x10
    [void][H32]::SetWindowPos($h, [IntPtr]::Zero, $x, $y, 0, 0, 0x15)
}

Write-Output "== start m8blive =="
# Start-Process -RedirectStandardOutput gives the child NO console window on
# PS5.1 (CREATE_NO_WINDOW), so launch via cmd /c with a titled console that
# the exe inherits; the exe's stdout is appended to the log by cmd.
$inner = 'title M8BCON && "' + $exe + '" --frames 100000 > "' + $alog + '" 2>&1'
$proc = Start-Process -FilePath 'cmd.exe' -ArgumentList '/c', $inner -WorkingDirectory $wd -PassThru
Write-Output ("cmd pid=" + $proc.Id)

# wait for overlay window + first present (model load can be slow)
$ovh = [IntPtr]::Zero; $conh = [IntPtr]::Zero
$deadline = (Get-Date).AddSeconds(120)
while ((Get-Date) -lt $deadline) {
    Start-Sleep -Milliseconds 1000
    $j = Ui @('windows','-All','-Json')
    try { $ws = $j | ConvertFrom-Json } catch { $ws = @() }
    foreach ($w in $ws) {
        if ($w.Title -like 'DLSS5_INTEL m8b*' -and $w.Process -like '*m8b*') { $ovh = [IntPtr]([Convert]::ToInt64($w.Handle,16)) }
        if ($w.Title -like '*M8BCON*') { $conh = [IntPtr]([Convert]::ToInt64($w.Handle,16)) }
    }
    $logHasShown = (Test-Path $alog) -and ((Get-Content $alog -Raw) -like '*overlay shown*')
    if ($ovh -ne [IntPtr]::Zero -and $conh -ne [IntPtr]::Zero -and $logHasShown) { break }
}
if ($ovh -eq [IntPtr]::Zero -or $conh -eq [IntPtr]::Zero) {
    Write-Output "FAIL: handles not found (ovh=$ovh conh=$conh)"
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    exit 1
}
Write-Output ("overlay hwnd=" + $ovh + " console hwnd=" + $conh)
Start-Sleep -Seconds 2   # let a couple frames present
$p0 = Shot '_A0.png'
Write-Output ("A0 shot; console rect before: " + (RectOf $conh))

# toggle HIDE (HK_TOGGLE=2)
[void][H32]::PostMessageW($ovh, 0x0312, [IntPtr]2, [IntPtr]0)
Write-Output "toggled HIDE"
Start-Sleep -Milliseconds 1500
MoveWin $conh 350 250
Write-Output ("moved console to 350,250; rect: " + (RectOf $conh))
Start-Sleep -Milliseconds 800
# toggle SHOW
[void][H32]::PostMessageW($ovh, 0x0312, [IntPtr]2, [IntPtr]0)
Write-Output "toggled SHOW"
Start-Sleep -Seconds 3
$p1 = Shot '_A1.png'
Write-Output ("A1 shot; console rect: " + (RectOf $conh))

# move console again while overlay visible
MoveWin $conh 900 500
Write-Output ("moved console to 900,500; rect: " + (RectOf $conh))
Start-Sleep -Seconds 3
$p2 = Shot '_A2.png'
Write-Output ("A2 shot; console rect: " + (RectOf $conh))

# clean quit via HK_QUIT=1 (exe exits -> cmd completes -> both gone)
[void][H32]::PostMessageW($ovh, 0x0312, [IntPtr]1, [IntPtr]0)
$exited = $proc.WaitForExit(8000)
if (-not $exited) {
    Get-Process m8blive -ErrorAction SilentlyContinue | Stop-Process -Force
    Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue
    Write-Output "killed (no clean exit)"
} else { Write-Output ("clean exit, code=" + $proc.ExitCode) }
Write-Output ("shots: " + $p0 + " | " + $p1 + " | " + $p2)
