param([string[]]$Files)
Add-Type -AssemblyName System.Drawing
foreach ($f in $Files) {
    $bmp = [System.Drawing.Bitmap]::FromFile($f)
    $w = $bmp.Width; $h = $bmp.Height
    $minX = $w; $maxX = -1; $minY = $h; $maxY = -1
    for ($y = 0; $y -lt $h; $y += 2) {
        $runStart = -1; $bestLen = 0; $bestS = -1; $bestE = -1
        for ($x = 0; $x -lt $w; $x++) {
            $p = $bmp.GetPixel($x, $y)
            $black = ($p.R -lt 20 -and $p.G -lt 20 -and $p.B -lt 20)
            if ($black -and $runStart -lt 0) { $runStart = $x }
            if (-not $black -and $runStart -ge 0) {
                $len = $x - $runStart
                if ($len -gt $bestLen) { $bestLen = $len; $bestS = $runStart; $bestE = $x - 1 }
                $runStart = -1
            }
        }
        if ($runStart -ge 0) {
            $len = $w - $runStart
            if ($len -gt $bestLen) { $bestLen = $len; $bestS = $runStart; $bestE = $w - 1 }
        }
        if ($bestLen -ge 400) {
            if ($bestS -lt $minX) { $minX = $bestS }
            if ($bestE -gt $maxX) { $maxX = $bestE }
            if ($y -lt $minY) { $minY = $y }
            if ($y -gt $maxY) { $maxY = $y }
        }
    }
    $bmp.Dispose()
    Write-Output ($f + '  console-bbox=(' + $minX + ',' + $minY + ')-(' + $maxX + ',' + $maxY + ')  size=' + $w + 'x' + $h)
}
