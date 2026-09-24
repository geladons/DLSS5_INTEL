# SendInput key hold helper (keybd_event based - works on this host).
# Usage: powershell -ExecutionPolicy Bypass -File _keyhold.ps1 <key> <down_ms>
# Keys: w a s d enter esc space up down left right f
param([string]$Key = "w", [int]$Ms = 1000)

$VK = @{ 'w'=0x57; 'a'=0x41; 's'=0x53; 'd'=0x44; 'enter'=0x0D; 'esc'=0x1B;
         'space'=0x20; 'up'=0x26; 'down'=0x28; 'left'=0x25; 'right'=0x27; 'f'=0x46 }
if (-not $VK.ContainsKey($Key)) { Write-Output "unknown key $Key"; exit 2 }
$b = [byte]$VK[$Key]
$down = [uint32]0; $up = [uint32]2
# keybd_event via user32
$sig = '[DllImport("user32.dll")] public static extern void keybd_event(byte bVk, byte bScan, uint dwFlags, System.UIntPtr dwExtraInfo);'
Add-Type -MemberDefinition $sig -Name K -Namespace I
[I.K]::keybd_event($b, 0, $down, [System.UIntPtr]::Zero)
Start-Sleep -Milliseconds $Ms
[I.K]::keybd_event($b, 0, $up, [System.UIntPtr]::Zero)
Write-Output "held $Key for ${Ms}ms"
