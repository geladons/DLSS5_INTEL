param([string]$Bmp)
Add-Type -AssemblyName System.Drawing
$fs = [System.IO.File]::OpenRead($Bmp)
$br = New-Object System.IO.BinaryReader($fs)
$br.BaseStream.Seek(10, 'Begin') | Out-Null
$off = $br.ReadInt32()
$br.BaseStream.Seek(18, 'Begin') | Out-Null
$w = $br.ReadInt32(); $h = $br.ReadInt32()
$br.BaseStream.Seek(28, 'Begin') | Out-Null
$bpp = $br.ReadInt16()
Write-Output ("$Bmp  ${w}x${h} bpp=$bpp dataOff=$off")
$rowSize = [int]([Math]::Ceiling($w * $bpp / 8.0))
$alphas = @{}
$sumA=0; $nA=0; $minA=255; $maxA=0
for ($y=0; $y -lt [Math]::Abs($h); $y+=37) {
    $fs.Seek($off + $y*$rowSize, 'Begin') | Out-Null
    $row = $br.ReadBytes($rowSize)
    for ($x=3; $x -lt $rowSize; $x+=4*89) {
        $a = $row[$x]
        $sumA+=$a; $nA++
        if ($a -lt $minA) {$minA=$a}
        if ($a -gt $maxA) {$maxA=$a}
        $alphas[$a] = 1
    }
}
Write-Output ("alpha samples n=$nA mean=$([int]($sumA/$nA)) min=$minA max=$maxA distinct=$($alphas.Count)")
$br.Close(); $fs.Close()
