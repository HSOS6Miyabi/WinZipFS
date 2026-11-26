# WinZipFS

WinZipFS mounts archives as Windows drives using WinFsp, with a native C++20 core, the 7-Zip engine through bit7z, and a WinUI 3 desktop interface.

## Features

- Mount several archives simultaneously; open the drive in File Explorer.
- Modify, create, rename and remove files on writable volumes.
- Cache changes locally; save automatically after a period without open handles, or enable batch mode and explicitly save or eject.
- Configure compression level, method, thread count, solid compression, writeback delay and idle eject timeout for individual mounted archives.
- Manage archives from the notification area. Closing the main window hides it; Exit saves and ejects all mounted archives.
- Register common archive extensions in Windows Settings. Choose WinZipFS as the default app for the extensions you want to open by double-clicking.

### Format support

| Read/write | Read-only |
| --- | --- |
| ZIP, ZIPX, 7z, TAR, TAR.GZ/TGZ, TAR.BZ2/TBZ/TBZ2, TAR.XZ/TXZ | TAR.ZST/TZST, RAR, CAB, ARJ, LZH/LHA, CPIO, DEB, RPM, ISO, WIM, DMG, XAR, standalone GZIP/BZIP2/XZ/ZSTD and other 7-Zip-readable formats |

Formats are detected by the archive engine. Multi-volume archives, archives with metadata that cannot be safely rewritten, and files without write access are read-only. Archives with encrypted contents, unsafe Windows names, conflicting paths, symbolic links or special entries are rejected. Standalone single-stream formats and disk images cannot be edited as general multi-file archives.

### Writeback and recovery

Files are extracted into a private per-session directory under `%LOCALAPPDATA%\WinZipFS\cache`. Changes are committed only when WinFsp reports that no file handles remain open. In automatic mode, writeback starts after the configured delay; batch mode defers it until explicit Save changes or Eject. Idle eject is disabled by default. Idle eject saves pending changes, including in batch mode.

A replacement archive is generated beside the original, tested using the 7-Zip engine, flushed to storage and then used to replace the original. An external change to the original file causes writeback to stop and retain the cache. During archive generation, opening new handles on the drive temporarily fails. If save fails, the session stays mounted and cached files are retained. The replacement operation creates a backup of the original; if replacement fails, remaining backup and replacement files are retained for recovery. On unexpected process termination, use Open recovery cache to inspect session directories and their archive.txt manifests.

## Build

Prerequisites: Windows x64, Visual Studio 2022 with C++ desktop and Windows SDK, CMake, vcpkg, WinFsp 2.2 with developer files, and the full 7-Zip 26.06 DLL. The workflow in .github/workflows/windows.yml builds and tests the native core and WinUI 3 application.

```powershell
cmake -S . -B build -A x64 -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" -DVCPKG_TARGET_TRIPLET=x64-windows
cmake --build build --config Release
msbuild ui/WinZipFS.vcxproj /restore /p:Configuration=Release /p:Platform=x64
Copy-Item build/Release/WinZipFSCore.dll artifacts/app/
Copy-Item 'C:\Program Files\7-Zip\7z.dll' artifacts/app/
```

Run `artifacts/app/WinZipFS.exe` or pass an archive path as the first argument. Windows App SDK is included in the application output. Install WinFsp before mounting drives.

## Third-party software

WinFsp 2.2 (GPL-3.0); bit7z 4.1 (MPL-2.0); 7-Zip (LGPL/BSD with unRAR restriction); Microsoft Windows App SDK and C++/WinRT.
