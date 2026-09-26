Add-Type -AssemblyName System.Drawing
$b = New-Object System.Drawing.Bitmap 2560, 1440
$g = [System.Drawing.Graphics]::FromImage($b)
$g.CopyFromScreen(0, 0, 0, 0, $b.Size)
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path
$b.Save((Join-Path $repo 'work\_m11\screen.png'))
$g.Dispose(); $b.Dispose()
Write-Output "saved"
