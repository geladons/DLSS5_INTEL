# SendInput-based key press with aggressive focus takeover.
# Usage: powershell -ExecutionPolicy Bypass -File _keysend.ps1 <window_substr> <vk_hex> [hold_ms]
param([string]$Title = "GTAIV", [string]$Vk = "0D", [int]$HoldMs = 0)

$VK_RETURN = [Convert]::ToInt32($Vk, 16)

$src = @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class SI {
  [DllImport("user32.dll")] public static extern IntPtr FindWindowW(string c, string n);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr lp);
  public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);
  [DllImport("user32.dll")] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern uint GetCurrentThreadId();
  [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint idAttach, uint idAttachTo, bool fAttach);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
  [DllImport("user32.dll", SetLastError=true)] public static extern uint SendInput(uint n, INPUT[] inputs, int sz);
  [DllImport("kernel32.dll")] public static extern uint GetLastError();
  [StructLayout(LayoutKind.Sequential)] public struct INPUT {
    public uint type;
    public MOUSEKEYBDHARDWAREINPUT mkhi;
  }
  [StructLayout(LayoutKind.Explicit)] public struct MOUSEKEYBDHARDWAREINPUT {
    [FieldOffset(0)] public KEYBDINPUT ki;
  }
  [StructLayout(LayoutKind.Sequential)] public struct KEYBDINPUT {
    public ushort wVk;
    public ushort wScan;
    public uint dwFlags;
    public uint time;
    public IntPtr dwExtraInfo;
  }
  public static IntPtr FindByTitle(string sub) {
    IntPtr hit = IntPtr.Zero;
    EnumWindows(delegate(IntPtr h, IntPtr l) {
      if (IsWindowVisible(h)) {
        StringBuilder sb = new StringBuilder(256);
        GetWindowTextW(h, sb, 256);
        if (sb.ToString().IndexOf(sub, StringComparison.OrdinalIgnoreCase) >= 0) { hit = h; return false; }
      }
      return true;
    }, IntPtr.Zero);
    return hit;
  }
}
"@
Add-Type -TypeDefinition $src

$h = [SI]::FindByTitle($Title)
if ($h -eq [IntPtr]::Zero) { Write-Output "no window: $Title"; exit 2 }
Write-Output ("window 0x{0:X}" -f $h.ToInt64())

# attach our input thread to the foreground thread, then force focus
$fg = [SI]::GetForegroundWindow()
$fgTid = [SI]::GetWindowThreadProcessId($fg, [ref]$null)
$myTid = [SI]::GetCurrentThreadId()
[void][SI]::AttachThreadInput($myTid, $fgTid, $true)
[void][SI]::ShowWindow($h, 9)
[void][SI]::BringWindowToTop($h)
[void][SI]::SetForegroundWindow($h)
Start-Sleep -Milliseconds 300
$now = [SI]::GetForegroundWindow()
Write-Output ("foreground now 0x{0:X} (target 0x{1:X})" -f $now.ToInt64(), $h.ToInt64())
[void][SI]::AttachThreadInput($myTid, $fgTid, $false)

function Send-Key([int]$vk, [bool]$up) {
  $inp = New-Object SI+INPUT
  $inp.type = 1
  $inp.mkhi.ki.wVk = [ushort]$vk
  $inp.mkhi.ki.wScan = 0
  $inp.mkhi.ki.dwFlags = $(if ($up) { 2 } else { 0 })
  $inp.mkhi.ki.time = 0
  $inp.mkhi.ki.dwExtraInfo = [IntPtr]::Zero
  $arr = @($inp)
  $n = [SI]::SendInput(1, $arr, [Runtime.InteropServices.Marshal]::SizeOf([type][SI+INPUT]))
  return $n
}
$down = Send-Key $VK_RETURN $false
if ($HoldMs -gt 0) { Start-Sleep -Milliseconds $HoldMs }
$up = Send-Key $VK_RETURN $true
Write-Output "sendinput down=$down up=$up vk=$VK_RETURN"
