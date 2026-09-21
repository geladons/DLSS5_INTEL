param([string]$A, [string]$B)
Add-Type -AssemblyName System.Drawing
function Sample($path, $regions) {
    $bmp = [System.Drawing.Bitmap]::FromFile($path)
    Write-Output ("FILE: " + $path)
    foreach ($r in $regions) {
        $x0=$r[0]; $y0=$r[1]; $x1=$r[2]; $y1=$r[3]; $name=$r[4]
        $sr=0; $sg=0; $sb=0; $n=0
        for ($y=$y0; $y -lt $y1; $y+=4) {
            for ($x=$x0; $x -lt $x1; $x+=4) {
                $c = $bmp.GetPixel($x,$y)
                $sr+=$c.R; $sg+=$c.G; $sb+=$c.B; $n++
            }
        }
        Write-Output ("  {0}: R={1} G={2} B={3}" -f $name, [int]($sr/$n), [int]($sg/$n), [int]($sb/$n))
    }
    $bmp.Dispose()
}
$regions = @(
    @(1000,500,1400,800,"wallpaper-center"),
    @(300,200,600,400,"wallpaper-left"),
    @(20,1400,2500,1430,"taskbar"),
    @(1100,250,1300,330,"wallpaper-midtop")
)
Sample $A $regions
Sample $B $regions
