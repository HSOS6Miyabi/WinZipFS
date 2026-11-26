$ErrorActionPreference = 'Stop'
$root = Join-Path (Get-Location) 'artifacts\fixtures'
New-Item $root -ItemType Directory -Force | Out-Null
$source = Join-Path $root 'source'
New-Item $source -ItemType Directory -Force | Out-Null
Set-Content (Join-Path $source 'sample.txt') -Value 'original' -NoNewline
New-Item (Join-Path $source 'empty') -ItemType Directory -Force | Out-Null
$seven = 'C:\Program Files\7-Zip\7z.exe'
Push-Location $source
try {
    foreach ($format in @('zip','7z','tar')) {
        $archive = Join-Path $root "core.$format"
        & $seven a "-t$format" $archive '.\*' -y
        if ($LASTEXITCODE -ne 0) { throw "Fixture $format failed" }
    }
    & $seven a -tgzip (Join-Path $root 'core.tar.gz') (Join-Path $root 'core.tar') -y
    if ($LASTEXITCODE -ne 0) { throw 'Compressed TAR fixture failed' }
    & $seven a -tzip (Join-Path $root 'ui.zip') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'UI ZIP fixture failed' }
    & $seven a -t7z (Join-Path $root 'tray.7z') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'Tray fixture failed' }
    & $seven a -tzip (Join-Path $root 'idle.zip') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'Idle fixture failed' }
    & makecab.exe (Join-Path $source 'sample.txt') (Join-Path $root 'readonly.cab') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CAB fixture failed' }
} finally { Pop-Location }
