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
