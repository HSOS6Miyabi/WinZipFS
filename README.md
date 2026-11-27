# WinZipFS

WinZipFS opens archives as Windows drives or UNC paths using WinFsp, with a native C++20 core, the 7-Zip engine through bit7z, libzip/zlib random access, and a responsive WinUI 3 desktop interface.

## Install

Download the Windows x64 installer or portable package from the latest successful [Windows build](https://github.com/HSOS6Miyabi/WinZipFS/actions/workflows/windows.yml). The installer adds a Start menu shortcut, registers archive types and installs WinFsp if required. Windows may request administrator approval for the driver. To enable double-click mounting for an existing association, choose WinZipFS in Windows Default apps.

For the portable package, extract all files to a permanent folder and install [WinFsp](https://winfsp.dev/rel/) 2.2 or later. Open WinZipFS and select Register file types. Before upgrading or uninstalling, exit WinZipFS from the notification area so that pending modifications are saved. Uninstalling preserves recovery files and the shared WinFsp driver.

## UNC access without drive letters

WinZipFS registers a local WinFsp network namespace while the application is running. The **UNC access without drive letters** panel displays the current Windows user's namespace. A local archive such as `D:\Documents\example.zip` is addressed as:

```text
\\WinZipFS\Archives-<Windows user SID>-<session ID>\D\Documents\example.zip\folder\file.txt
```

Use **Copy UNC path** to obtain the exact address without extracting or mounting the archive. Opening that address directly from Explorer, PowerShell, or another application creates an in-memory archive index. No drive letter is assigned. After the last handle closes, the configured idle cache lifetime (30 seconds by default, 0–86400 seconds) determines when changes are saved and the cache is released. Opening the same address again creates a fresh session. Batch write mode also saves pending changes before expiry. A failed save retains pending edits in memory while WinZipFS is running.

For a stable read-only address, insert `ReadOnly` before the drive component (for example, `...\\ReadOnly\\D\\Documents\\example.zip`). That address remains read-only after the cache expires. The global cache lifetime is the default for newly opened sessions; each active UNC archive can override it under Compression and cache settings.

The namespace is local to the Windows session and checks the caller's user identity and integrity level; it does not provide an SMB share or expose ordinary local files. Source archives must be on local physical drives without reparse-point path components. A session cannot simultaneously be mounted through a drive letter and through UNC, which prevents competing modifications. For encrypted archives, use **Open UNC archive** to enter the password in the application. You can choose UNC as the default for double-click opening or use `WinZipFS.exe --unc "D:\Documents\example.zip"`. Direct UNC access requires the WinZipFS process to be running.

## Features

- Mount several archives simultaneously, including in an explicitly selected read-only mode; open the drive in File Explorer.
- Save a separate ZIP or 7z copy of the current cached files without changing the original. Discard pending changes only after confirmation.
- Modify, create, rename and remove files on writable volumes.
- Cache changes locally; save automatically after a period without open handles, or enable batch mode and explicitly save or eject.
- Configure compression level, method, thread count, solid compression, writeback delay and idle eject timeout for individual mounted archives.
- Manage archives from the notification area, including read-only mounting, exporting a copy and discarding unsaved changes. Closing the main window hides it; Exit saves and ejects all mounted archives.
- Register common archive extensions in Windows Settings. Choose WinZipFS as the default app for the extensions you want to open by double-clicking.

### Format support

| Read/write | Read-only |
| --- | --- |
| ZIP, ZIPX, 7z, TAR, TAR.GZ/TGZ, TAR.BZ2/TBZ/TBZ2, TAR.XZ/TXZ | TAR.ZST/TZST, RAR, CAB, ARJ, LZH/LHA, CPIO, DEB, RPM, ISO, WIM, DMG, XAR, standalone GZIP/BZIP2/XZ/ZSTD and other 7-Zip-readable formats |

Formats are detected by the archive engine. Multi-volume archives, archives with metadata that cannot be safely rewritten, and files without write access are read-only. Archives with unsafe Windows names, conflicting paths, symbolic links or special entries are rejected. Password-protected archives can be mounted read-only after entering the password in the application. Decrypted data is read into memory on demand and released on eject. Exporting a protected session as an unencrypted archive is disabled. Standalone single-stream formats and disk images cannot be edited as general multi-file archives.

### Writeback and recovery

Archive contents are never extracted to a disk cache. Opening ordinary ZIP, 7z and TAR archives reads metadata only. Stored ZIP and TAR entries are read directly at their byte offsets. ZIP DEFLATE entries use evictable block checkpoints containing the 32 KiB history dictionary, bit alignment, and rolling CRC. On a cache miss, decoding resumes from the nearest available checkpoint, or from the beginning if no checkpoint exists. Other compression methods use seekable archive streams where supported, or decode the requested ranges into bounded memory windows; sequential and solid compression may require decoding preceding data. Writes use sparse copy-on-write pages; untouched content is read from the original compressed archive. The configurable memory limit defaults to 512 MiB. It covers cached payload and pending write pages, excluding the compression engine's own working memory and metadata. Clean pages are evicted when necessary; dirty pages are retained until saved or explicitly discarded. Eight MiB of the configured memory limit is reserved for read and writeback decoding. Exhausting the dirty-page budget rejects new writes rather than spilling to disk, while preserving enough space to read and save the accepted changes. Automatic writeback and idle eject save changes after the configured idle time. Batch mode defers saving until an explicit save or eject.

ZIP and 7z use incremental archive updates: filesystem operations track modified pages, renames and metadata changes, and unchanged entries are retained by the compression engine. For solid 7z archives, the engine may need to recompress affected solid blocks. Use **Recompress all** to apply new compression settings to every entry. TAR-family formats use streaming reconstruction. A compressed TAR is read through a logical seekable stream, and is written directly to the outer compression engine without an intermediate TAR file.

A replacement archive is generated beside the original, tested using the 7-Zip engine, checked against the in-memory file and directory manifest, and verified against the intended file contents, flushed to storage and then used to replace the original. The source archive stays open without write sharing during the session. If another application replaces its path, writeback stops and the original source handle and pending memory pages remain available for exporting a copy. During archive generation, opening new handles on the drive temporarily fails. If save fails, the session stays mounted and unsaved memory pages are retained. The replacement operation creates a backup of the original; if replacement fails, remaining backup and replacement files are retained for recovery. Unsaved memory changes are lost if the process or system terminates unexpectedly. The original archive remains unchanged until a verified replacement is committed. The recovery manager can export existing recovery directories. Windows may page ordinary process memory according to system policy.

## Build

Prerequisites: Windows x64, Visual Studio 2022 with C++ desktop and Windows SDK, CMake, vcpkg, WinFsp 2.2 with developer files, and the full 7-Zip 26.06 DLL. The workflow in .github/workflows/windows.yml builds and tests the native core and WinUI 3 application.

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
msbuild ui/WinZipFS.vcxproj /restore /p:Configuration=Release /p:Platform=x64
Copy-Item build/vcpkg_installed/x64-windows/bin/*.dll artifacts/app/ -Force
Copy-Item build/Release/WinZipFSCore.dll artifacts/app/ -Force
Copy-Item 'C:\Program Files\7-Zip\7z.dll' artifacts/app/
```

Run `artifacts/app/WinZipFS.exe` or pass an archive path as the first argument. Windows App SDK is included in the application output. Install WinFsp before mounting drives.

## Third-party software

WinFsp 2.2 (GPL-3.0); bit7z 4.1 (MPL-2.0); 7-Zip (LGPL/BSD with unRAR restriction); libzip (BSD-3-Clause); zlib (Zlib); Microsoft Windows App SDK and C++/WinRT.
