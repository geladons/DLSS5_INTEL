# Screenshot a window via its process (FindWindow is unreliable here).
# One Add-Type: P/Invoke methods + a nested RECT struct (accessed as
# [W.U32+RECT] from PowerShell).
param([int]$ProcId = 0, [string]$Out = "_ui_shot.png")
Add-Type -AssemblyName System.Drawing
Add-Type '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool ShowWindow(System.IntPtr h, int c); [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool GetWindowRect(System.IntPtr h, out RECT r); [System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool PrintWindow(System.IntPtr h, System.IntPtr dc, uint flags); public struct RECT { public int L, T, R, B; }' -Name U32 -Namespace W
if ($ProcId -eq 0) {
    Get-Process | Where-Object { $_.MainWindowTitle -ne "" } |
        Select-Object Id, ProcessName, MainWindowTitle | Format-Table -AutoSize
    exit 0
}
$p = Get-Process -Id $ProcId
$h = $p.MainWindowHandle
if ($h -eq [IntPtr]::Zero) { Write-Host "no main window for pid $ProcId"; exit 1 }
[W.U32]::ShowWindow($h, 9) | Out-Null   # SW_RESTORE
Start-Sleep -Milliseconds 400
$r = New-Object 'W.U32+RECT'
[W.U32]::GetWindowRect($h, [ref]$r) | Out-Null
$w = $r.R - $r.L; $hgt = $r.B - $r.T
if ($w -le 0 -or $hgt -le 0) { Write-Host "bad rect ${w}x$hgt"; exit 1 }
$bmp = New-Object System.Drawing.Bitmap $w, $hgt
$g = [System.Drawing.Graphics]::FromImage($bmp)
$hd = $g.GetHdc()
[W.U32]::PrintWindow($h, $hd, 0) | Out-Null
$g.ReleaseHdc($hd)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
Write-Host "saved $Out ${w}x$hgt from pid $ProcId"
