param([string]$Img1, [string]$Img2)
Add-Type -AssemblyName System.Drawing
$a = [System.Drawing.Bitmap]::FromFile($Img1)
$b = [System.Drawing.Bitmap]::FromFile($Img2)
if ($a.Width -ne $b.Width -or $a.Height -ne $b.Height) { Write-Output "SIZE MISMATCH"; exit 1 }
$regions = @(
    @(0,0,$a.Width,$a.Height,"full"),
    @(60,60,340,260,"notepad-old-150-150"),
    @(380,192,660,392,"notepad-new-950-480")
)
foreach ($r in $regions) {
    $x0=$r[0]; $y0=$r[1]; $x1=$r[2]; $y1=$r[3]; $name=$r[4]
    $d=0; $n=0
    for ($y=$y0; $y -lt $y1; $y+=2) {
        for ($x=$x0; $x -lt $x1; $x+=2) {
            $ca=$a.GetPixel($x,$y); $cb=$b.GetPixel($x,$y)
            $d += [Math]::Abs($ca.R-$cb.R) + [Math]::Abs($ca.G-$cb.G) + [Math]::Abs($ca.B-$cb.B)
            $n++
        }
    }
    Write-Output ("{0}: meanAbsDiff/chan = {1:N2}" -f $name, ($d/($n*3.0)))
}
$a.Dispose(); $b.Dispose()
