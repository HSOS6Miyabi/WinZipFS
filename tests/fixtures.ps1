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
from zipfile import ZIP_STORED, ZIP_DEFLATED
import tarfile
from io import RawIOBase
MIB = 1024 * 1024
class Pattern(RawIOBase):
    def __init__(self):
        self.offset = 0
    def read(self, n=-1):
        n = min(64*MIB-self.offset, n if n >= 0 else 64*MIB)
        result = bytearray()
        while n:
            block = self.offset // MIB
            take = min(n, MIB-self.offset%MIB)
            result.extend(bytes([block%251])*take)
            self.offset += take
            n -= take
        return bytes(result)
with ZipFile(root / "random.zip", "w", compression=ZIP_DEFLATED) as z:
    with z.open("large.bin", "w") as out:
        source = Pattern()
        while chunk := source.read(MIB):
            out.write(chunk)
with ZipFile(root / "random-stored.zip", "w", compression=ZIP_STORED) as z:
    with z.open("large.bin", "w") as out:
        source = Pattern()
        while chunk := source.read(MIB):
            out.write(chunk)
for name, mode in (("metadata.tar", "w"), ("metadata.tar.gz", "w:gz")):
    with tarfile.open(root / name, mode, format=tarfile.PAX_FORMAT) as tar:
        from io import BytesIO
        info = tarfile.TarInfo("script.sh")
        info.size = 3
        info.mode = 0o751
        info.uid, info.gid = 12345, 23456
        info.uname, info.gname = "example-user", "example-group"
        info.mtime = 1712345678.125
        tar.addfile(info, BytesIO(b"old"))
with tarfile.open(root / "pax-extra.tar", "w", format=tarfile.PAX_FORMAT) as tar:
    from io import BytesIO
    info = tarfile.TarInfo("sample.txt")
    info.size = 8
    info.pax_headers = {"SCHILY.xattr.user.example": "preserve-me"}
    tar.addfile(info, BytesIO(b"original"))
with tarfile.open(root / "special.tar", "w") as tar:
    info = tarfile.TarInfo("fifo")
    info.type = tarfile.FIFOTYPE
    tar.addfile(info)
with tarfile.open(root / "random.tar", "w") as tar:
    info = tarfile.TarInfo("large.bin")
    info.size = 64*MIB
    tar.addfile(info, Pattern())
with ZipFile(root / "incremental.zip", "w") as z:
    z.writestr("unchanged.bin", b"A" * 1048576, compress_type=ZIP_STORED)
    z.writestr("changed.txt", b"old" * 4096, compress_type=ZIP_DEFLATED)


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
