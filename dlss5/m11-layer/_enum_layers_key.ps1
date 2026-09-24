$paths = @(
  'HKCU:\Software\Khronos\Vulkan\ImplicitLayers',
  'HKCU:\Software\Khronos\Vulkan\ImplicitLayers"'
)
foreach ($path in $paths) {
    Write-Host "== $path"
    $k = Get-Item $path -ErrorAction SilentlyContinue
    if ($null -eq $k) { Write-Host '   (missing)'; continue }
    foreach ($p in $k.Property) {
        $v = (Get-ItemProperty -Path $k.PSPath -Name $p).$p
        Write-Host ('   name=[{0}] data=[{1}]' -f $p, $v)
    }
}
