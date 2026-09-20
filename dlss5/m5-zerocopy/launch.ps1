$exe = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m5-zerocopy\build\Release\m5zerocopy.exe'
$wd  = 'C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m5-zerocopy'
$run = $args[0]
$rest = @()
for ($i = 1; $i -lt $args.Count; $i++) { $rest += $args[$i] }
$out = 'C:\Users\AI\Desktop\DLSS5_INTEL\docs\' + $run + '.out.log'
$err = 'C:\Users\AI\Desktop\DLSS5_INTEL\docs\' + $run + '.err.log'
if (Test-Path $out) { Remove-Item $out -Force }
if (Test-Path $err) { Remove-Item $err -Force }
# NOTE: -WindowStyle Hidden must NOT be used with -RedirectStandard*:
# it forces ShellExecute, which cannot inherit redirect handles -> 0-byte logs.
$p = Start-Process -FilePath $exe -ArgumentList $rest -WorkingDirectory $wd `
        -RedirectStandardOutput $out -RedirectStandardError $err -PassThru
Write-Output ('started pid ' + $p.Id)
exit 0
