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
    foreach ($entry in @(@('gzip','gz'),@('bzip2','bz2'),@('xz','xz'))) {
        & $seven a "-t$($entry[0])" (Join-Path $root "core.tar.$($entry[1])") (Join-Path $root 'core.tar') -y
        if ($LASTEXITCODE -ne 0) { throw "Compressed TAR fixture $($entry[0]) failed" }
    }
    foreach ($entry in @(@('gzip','gz'),@('bzip2','bz2'),@('xz','xz'))) {
        & $seven a "-t$($entry[0])" (Join-Path $root "sample.txt.$($entry[1])") (Join-Path $source 'sample.txt') -y
        if ($LASTEXITCODE -ne 0) { throw "Single-stream fixture $($entry[0]) failed" }
    }
    & $seven a -tzip (Join-Path $root 'encrypted.zip') '.\*' -pTestFixturePassword -mem=AES256 -y
    if ($LASTEXITCODE -ne 0) { throw 'Encrypted fixture failed' }
    & $seven a -t7z (Join-Path $root 'encrypted.7z') '.\*' -pTestFixturePassword -mhe=on -y
    if ($LASTEXITCODE -ne 0) { throw 'Header-encrypted fixture failed' }
    & $seven a -tzip (Join-Path $root 'ui.zip') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'UI ZIP fixture failed' }
    & $seven a -t7z (Join-Path $root 'tray.7z') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'Tray fixture failed' }
    & $seven a -tzip (Join-Path $root 'idle.zip') '.\*' -y
    if ($LASTEXITCODE -ne 0) { throw 'Idle fixture failed' }
    & makecab.exe (Join-Path $source 'sample.txt') (Join-Path $root 'readonly.cab') | Out-Null
    if ($LASTEXITCODE -ne 0) { throw 'CAB fixture failed' }
} finally { Pop-Location }

@'
from pathlib import Path
from zipfile import ZipFile
import sys
root = Path(sys.argv[1])

def zstd_raw(data):
    assert 0 < len(data) < 65792
    if len(data) < 256:
        frame = b"\x20" + bytes([len(data)])
    else:
        frame = b"\x60" + (len(data) - 256).to_bytes(2, "little")
    block = ((len(data) << 3) | 1).to_bytes(3, "little")
    return b"\x28\xb5\x2f\xfd" + frame + block + data

(root / "sample.txt.zst").write_bytes(zstd_raw(b"original"))
(root / "core.tar.zst").write_bytes(zstd_raw((root / "core.tar").read_bytes()))
with ZipFile(root / "traversal.zip", "w") as z:
    z.writestr("../outside.txt", "must never be extracted")
with ZipFile(root / "collision.zip", "w") as z:
    z.writestr("sample.txt", "one")
    z.writestr("SAMPLE.TXT", "two")
with ZipFile(root / "reserved.zip", "w") as z:
    z.writestr("CON.txt", "reserved device name")
'@ | python - $root
if ($LASTEXITCODE -ne 0) { throw 'Invalid archive fixtures failed' }
