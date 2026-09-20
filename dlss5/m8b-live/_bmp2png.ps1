param([string]$src, [string]$dst)
Add-Type -AssemblyName System.Drawing
$b = [System.Drawing.Bitmap]::FromFile($src)
$b.Save($dst, [System.Drawing.Imaging.ImageFormat]::Png)
$b.Dispose()
