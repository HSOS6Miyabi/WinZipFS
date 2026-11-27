from pathlib import Path
from zipfile import ZipFile, ZIP_STORED, ZIP_DEFLATED
root = Path("artifacts/fixtures")
with ZipFile(root / "incremental-before-recompress.zip") as z:
    assert z.getinfo("unchanged.bin").compress_type == ZIP_STORED
    assert z.read("unchanged.bin") == b"A" * 1048576
    assert z.read("changed.txt") == b"B" * 12288
with ZipFile(root / "incremental.zip") as z:
    assert z.getinfo("unchanged.bin").compress_type == ZIP_DEFLATED
    assert z.read("unchanged.bin") == b"A" * 1048576
    assert z.read("changed.txt") == b"B" * 12288
print("Incremental update preserved unchanged compressed entries; explicit recompression changed compression method.")

import tarfile
for name in ("metadata.tar", "metadata.tar.gz"):
    with tarfile.open(root / name, "r:*") as archive:
        item = archive.getmember("script.sh")
        assert item.mode == 0o751, (name, item.mode)
        assert (item.uid, item.gid, item.uname, item.gname) == (12345, 23456, "example-user", "example-group"), name
        assert item.mtime == 1712345678.125, (name, item.mtime)
        assert archive.extractfile(item).read() == b"new", name
print("POSIX TAR metadata and updated data verified.")
