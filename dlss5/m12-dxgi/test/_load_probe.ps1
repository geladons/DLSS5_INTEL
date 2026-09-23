Add-Type -Namespace W -Name K -MemberDefinition @"
[System.Runtime.InteropServices.DllImport("kernel32.dll", SetLastError=$true, CharSet=[System.Runtime.InteropServices.CharSet.Unicode)] public static extern System.IntPtr LoadLibraryW(string p);
[System.Runtime.InteropServices.DllImport("kernel32.dll")] public static extern uint GetLastError();
"@
$h = [W.K]::LoadLibraryW('C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m12-dxgi\test\run\dxgi.dll')
Write-Output ("handle=" + $h)
Write-Output ("err=" + [W.K]::GetLastError())
