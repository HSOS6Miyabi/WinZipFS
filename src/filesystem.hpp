/**
 * @file passthrough-cpp.cpp
 *
 * @copyright 2015-2026 Bill Zissimopoulos
 */
/*
 * This file is part of WinFsp.
 *
 * You can redistribute it and/or modify it under the terms of the GNU
 * General Public License version 3 as published by the Free Software
 * Foundation.
 *
 * Licensees holding a valid commercial license may use this software
 * in accordance with the commercial license agreement provided in
 * conjunction with the software.  The terms and conditions of any such
 * commercial license agreement shall govern, supersede, and render
 * ineffective any application of the GPLv3 license to this software,
 * notwithstanding of any reference thereto in the software or
 * associated repository.
 */

#include <winfsp/winfsp.hpp>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <atomic>
#include <string>
#include <strsafe.h>

#define PROGNAME                        "passthrough-cpp"

#define ALLOCATION_UNIT                 4096
#define FULLPATH_SIZE                   (MAX_PATH + FSP_FSCTL_TRANSACT_PATH_SIZEMAX / sizeof(WCHAR))

#define info(format, ...)               Service::Log(EVENTLOG_INFORMATION_TYPE, format, __VA_ARGS__)
#define warn(format, ...)               Service::Log(EVENTLOG_WARNING_TYPE, format, __VA_ARGS__)
#define fail(format, ...)               Service::Log(EVENTLOG_ERROR_TYPE, format, __VA_ARGS__)

#define ConcatPath(FN, FP)              (0 == StringCbPrintfW(FP, sizeof FP, L"%s%s", _Path, FN))
#define HandleFromFileDesc(FD)          ((PtfsFileDesc *)(FD))->Handle

using namespace Fsp;

class Ptfs : public FileSystemBase
{
public:
    Ptfs();
    ~Ptfs();
    NTSTATUS SetPath(PWSTR Path);
    std::mutex gate;
    std::atomic<unsigned> handles{0};
    std::atomic<bool> dirty{false};
    std::atomic<bool> closing{false};
    std::atomic<long long> lastActivity{0};
    bool readOnly = false;
    std::wstring label = L"WinZipFS";
    void Touch() { lastActivity = GetTickCount64(); }
    void Changed() { dirty = true; Touch(); }

protected:
    static NTSTATUS GetFileInfoInternal(HANDLE Handle, FileInfo *FileInfo);
    NTSTATUS Init(PVOID Host);
    NTSTATUS GetVolumeInfo(
        VolumeInfo *VolumeInfo);
    NTSTATUS GetSecurityByName(
        PWSTR FileName,
        PUINT32 PFileAttributes/* or ReparsePointIndex */,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T *PSecurityDescriptorSize);
    NTSTATUS Create(
        PWSTR FileName,
        UINT32 CreateOptions,
        UINT32 GrantedAccess,
        UINT32 FileAttributes,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        UINT64 AllocationSize,
        PVOID *PFileNode,
        PVOID *PFileDesc,
        OpenFileInfo *OpenFileInfo);
    NTSTATUS Open(
        PWSTR FileName,
        UINT32 CreateOptions,
        UINT32 GrantedAccess,
        PVOID *PFileNode,
        PVOID *PFileDesc,
        OpenFileInfo *OpenFileInfo);
    NTSTATUS Overwrite(
        PVOID FileNode,
        PVOID FileDesc,
        UINT32 FileAttributes,
        BOOLEAN ReplaceFileAttributes,
        UINT64 AllocationSize,
        FileInfo *FileInfo);
    VOID Cleanup(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName,
        ULONG Flags);
    VOID Close(
        PVOID FileNode,
        PVOID FileDesc);
    NTSTATUS Read(
        PVOID FileNode,
        PVOID FileDesc,
        PVOID Buffer,
        UINT64 Offset,
        ULONG Length,
        PULONG PBytesTransferred);
    NTSTATUS Write(
        PVOID FileNode,
        PVOID FileDesc,
        PVOID Buffer,
        UINT64 Offset,
        ULONG Length,
        BOOLEAN WriteToEndOfFile,
        BOOLEAN ConstrainedIo,
        PULONG PBytesTransferred,
        FileInfo *FileInfo);
    NTSTATUS Flush(
        PVOID FileNode,
        PVOID FileDesc,
        FileInfo *FileInfo);
    NTSTATUS GetFileInfo(
        PVOID FileNode,
        PVOID FileDesc,
        FileInfo *FileInfo);
    NTSTATUS SetBasicInfo(
        PVOID FileNode,
        PVOID FileDesc,
        UINT32 FileAttributes,
        UINT64 CreationTime,
        UINT64 LastAccessTime,
        UINT64 LastWriteTime,
        UINT64 ChangeTime,
        FileInfo *FileInfo);
    NTSTATUS SetFileSize(
        PVOID FileNode,
        PVOID FileDesc,
        UINT64 NewSize,
        BOOLEAN SetAllocationSize,
        FileInfo *FileInfo);
    NTSTATUS CanDelete(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName);
    NTSTATUS Rename(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR FileName,
        PWSTR NewFileName,
        BOOLEAN ReplaceIfExists);
    NTSTATUS GetSecurity(
        PVOID FileNode,
        PVOID FileDesc,
        PSECURITY_DESCRIPTOR SecurityDescriptor,
        SIZE_T *PSecurityDescriptorSize);
    NTSTATUS SetSecurity(
        PVOID FileNode,
        PVOID FileDesc,
        SECURITY_INFORMATION SecurityInformation,
        PSECURITY_DESCRIPTOR ModificationDescriptor);
    NTSTATUS ReadDirectory(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR Pattern,
        PWSTR Marker,
        PVOID Buffer,
        ULONG Length,
        PULONG PBytesTransferred);
    NTSTATUS ReadDirectoryEntry(
        PVOID FileNode,
        PVOID FileDesc,
        PWSTR Pattern,
        PWSTR Marker,
        PVOID *PContext,
        DirInfo *DirInfo);

private:
    PWSTR _Path;
    UINT64 _CreationTime;
};

struct PtfsFileDesc
{
    PtfsFileDesc() : Handle(INVALID_HANDLE_VALUE), DirBuffer()
    {
    }
    ~PtfsFileDesc()
    {
        if (Handle != INVALID_HANDLE_VALUE) CloseHandle(Handle);
        Ptfs::DeleteDirectoryBuffer(&DirBuffer);
    }
    HANDLE Handle;
    PVOID DirBuffer;
};

Ptfs::Ptfs() : FileSystemBase(), _Path()
{
}

Ptfs::~Ptfs()
{
    delete[] _Path;
}

NTSTATUS Ptfs::SetPath(PWSTR Path)
{
    WCHAR FullPath[FULLPATH_SIZE];
    ULONG Length;
    HANDLE Handle;
    FILETIME CreationTime;
    DWORD LastError;

    Handle = CreateFileW(
        Path, FILE_READ_ATTRIBUTES, 0, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (INVALID_HANDLE_VALUE == Handle)
        return NtStatusFromWin32(GetLastError());

    Length = GetFinalPathNameByHandleW(Handle, FullPath, FULLPATH_SIZE - 1, 0);
    if (0 == Length || Length >= FULLPATH_SIZE - 1)
    {
        LastError = GetLastError();
        CloseHandle(Handle);
        return NtStatusFromWin32(LastError);
    }
    if (L'\\' == FullPath[Length - 1])
        FullPath[--Length] = L'\0';

    if (!GetFileTime(Handle, &CreationTime, 0, 0))
    {
        LastError = GetLastError();
        CloseHandle(Handle);
        return NtStatusFromWin32(LastError);
    }

    CloseHandle(Handle);

    Length++;
    delete[] _Path;
    _Path = new WCHAR[Length];
    memcpy(_Path, FullPath, Length * sizeof(WCHAR));

    _CreationTime = ((PLARGE_INTEGER)&CreationTime)->QuadPart;

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::GetFileInfoInternal(HANDLE Handle, FileInfo *FileInfo)
{
    BY_HANDLE_FILE_INFORMATION ByHandleFileInfo;

    if (!GetFileInformationByHandle(Handle, &ByHandleFileInfo))
        return NtStatusFromWin32(GetLastError());

    FileInfo->FileAttributes = ByHandleFileInfo.dwFileAttributes;
    FileInfo->ReparseTag = 0;
    FileInfo->FileSize =
        ((UINT64)ByHandleFileInfo.nFileSizeHigh << 32) | (UINT64)ByHandleFileInfo.nFileSizeLow;
    FileInfo->AllocationSize = (FileInfo->FileSize + ALLOCATION_UNIT - 1)
        / ALLOCATION_UNIT * ALLOCATION_UNIT;
    FileInfo->CreationTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftCreationTime)->QuadPart;
    FileInfo->LastAccessTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastAccessTime)->QuadPart;
    FileInfo->LastWriteTime = ((PLARGE_INTEGER)&ByHandleFileInfo.ftLastWriteTime)->QuadPart;
    FileInfo->ChangeTime = FileInfo->LastWriteTime;
    FileInfo->IndexNumber = 0;
    FileInfo->HardLinks = 0;

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::Init(PVOID Host0)
{
    FileSystemHost *Host = (FileSystemHost *)Host0;
    Host->SetSectorSize(ALLOCATION_UNIT);
    Host->SetSectorsPerAllocationUnit(1);
    Host->SetFileInfoTimeout(1000);
    Host->SetCaseSensitiveSearch(FALSE);
    Host->SetCasePreservedNames(TRUE);
    Host->SetUnicodeOnDisk(TRUE);
    Host->SetPersistentAcls(FALSE);
    Host->SetReadOnlyVolume(readOnly);
    Host->SetFileSystemName(const_cast<PWSTR>(L"WinZipFS"));
    Host->SetPostCleanupWhenModifiedOnly(TRUE);
    Host->SetPassQueryDirectoryPattern(TRUE);
    Host->SetVolumeCreationTime(_CreationTime);
    Host->SetVolumeSerialNumber(0);
    Host->SetFlushAndPurgeOnCleanup(TRUE);
    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::GetVolumeInfo(
    VolumeInfo *VolumeInfo)
{
    WCHAR Root[MAX_PATH];
    ULARGE_INTEGER TotalSize, FreeSize;

    if (!GetVolumePathName(_Path, Root, MAX_PATH))
        return NtStatusFromWin32(GetLastError());

    if (!GetDiskFreeSpaceEx(Root, 0, &TotalSize, &FreeSize))
        return NtStatusFromWin32(GetLastError());

    VolumeInfo->TotalSize = TotalSize.QuadPart;
    VolumeInfo->FreeSize = readOnly ? 0 : FreeSize.QuadPart;
    VolumeInfo->VolumeLabelLength = static_cast<UINT16>((std::min)(label.size(), size_t(31)) * sizeof(WCHAR));
    memcpy(VolumeInfo->VolumeLabel, label.data(), VolumeInfo->VolumeLabelLength);

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::GetSecurityByName(
    PWSTR FileName,
    PUINT32 PFileAttributes/* or ReparsePointIndex */,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T *PSecurityDescriptorSize)
{
    WCHAR FullPath[FULLPATH_SIZE];
    HANDLE Handle;
    FILE_ATTRIBUTE_TAG_INFO AttributeTagInfo;
    DWORD SecurityDescriptorSizeNeeded;
    NTSTATUS Result;

    if (!ConcatPath(FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    Handle = CreateFileW(FullPath,
        FILE_READ_ATTRIBUTES | READ_CONTROL, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, 0);
    if (INVALID_HANDLE_VALUE == Handle)
    {
        Result = NtStatusFromWin32(GetLastError());
        goto exit;
    }

    if (0 != PFileAttributes)
    {
        if (!GetFileInformationByHandleEx(Handle,
            FileAttributeTagInfo, &AttributeTagInfo, sizeof AttributeTagInfo))
        {
            Result = NtStatusFromWin32(GetLastError());
            goto exit;
        }

        *PFileAttributes = AttributeTagInfo.FileAttributes;
    }

    if (0 != PSecurityDescriptorSize)
    {
        if (!GetKernelObjectSecurity(Handle,
            OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
            SecurityDescriptor, (DWORD)*PSecurityDescriptorSize, &SecurityDescriptorSizeNeeded))
        {
            *PSecurityDescriptorSize = SecurityDescriptorSizeNeeded;
            Result = NtStatusFromWin32(GetLastError());
            goto exit;
        }

        *PSecurityDescriptorSize = SecurityDescriptorSizeNeeded;
    }

    Result = STATUS_SUCCESS;

exit:
    if (INVALID_HANDLE_VALUE != Handle)
        CloseHandle(Handle);

    return Result;
}

NTSTATUS Ptfs::Create(
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    UINT32 FileAttributes,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    UINT64 AllocationSize,
    PVOID *PFileNode,
    PVOID *PFileDesc,
    OpenFileInfo *OpenFileInfo)
{
    std::lock_guard lock(gate);
    if (closing) return STATUS_DEVICE_NOT_READY;
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    WCHAR FullPath[FULLPATH_SIZE];
    SECURITY_ATTRIBUTES SecurityAttributes;
    ULONG CreateFlags;
    PtfsFileDesc *FileDesc;

    if (!ConcatPath(FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    FileDesc = new PtfsFileDesc;

    SecurityAttributes.nLength = sizeof SecurityAttributes;
    SecurityAttributes.lpSecurityDescriptor = SecurityDescriptor;
    SecurityAttributes.bInheritHandle = FALSE;

    CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
    if (CreateOptions & FILE_DELETE_ON_CLOSE)
        CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;

    if (CreateOptions & FILE_DIRECTORY_FILE)
    {
        /*
         * It is not widely known but CreateFileW can be used to create directories!
         * It requires the specification of both FILE_FLAG_BACKUP_SEMANTICS and
         * FILE_FLAG_POSIX_SEMANTICS. It also requires that FileAttributes has
         * FILE_ATTRIBUTE_DIRECTORY set.
         */
        CreateFlags |= FILE_FLAG_POSIX_SEMANTICS;
        FileAttributes |= FILE_ATTRIBUTE_DIRECTORY;
    }
    else
        FileAttributes &= ~FILE_ATTRIBUTE_DIRECTORY;

    if (0 == FileAttributes)
        FileAttributes = FILE_ATTRIBUTE_NORMAL;

    FileDesc->Handle = CreateFileW(FullPath,
        GrantedAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, &SecurityAttributes,
        CREATE_NEW, CreateFlags | FileAttributes, 0);
    if (INVALID_HANDLE_VALUE == FileDesc->Handle)
    {
        delete FileDesc;
        return NtStatusFromWin32(GetLastError());
    }

    NTSTATUS result = GetFileInfoInternal(FileDesc->Handle, &OpenFileInfo->FileInfo);
    if (!NT_SUCCESS(result)) { delete FileDesc; return result; }
    *PFileDesc = FileDesc;
    ++handles; Changed();
    return result;
}

NTSTATUS Ptfs::Open(
    PWSTR FileName,
    UINT32 CreateOptions,
    UINT32 GrantedAccess,
    PVOID *PFileNode,
    PVOID *PFileDesc,
    OpenFileInfo *OpenFileInfo)
{
    std::lock_guard lock(gate);
    if (closing) return STATUS_DEVICE_NOT_READY;
    if (readOnly && ((GrantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA | DELETE | WRITE_DAC | WRITE_OWNER)) || (CreateOptions & FILE_DELETE_ON_CLOSE))) return STATUS_MEDIA_WRITE_PROTECTED;
    WCHAR FullPath[FULLPATH_SIZE];
    ULONG CreateFlags;
    PtfsFileDesc *FileDesc;

    if (!ConcatPath(FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    FileDesc = new PtfsFileDesc;

    CreateFlags = FILE_FLAG_BACKUP_SEMANTICS;
    if (CreateOptions & FILE_DELETE_ON_CLOSE)
        CreateFlags |= FILE_FLAG_DELETE_ON_CLOSE;

    FileDesc->Handle = CreateFileW(FullPath,
        GrantedAccess, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, 0,
        OPEN_EXISTING, CreateFlags, 0);
    if (INVALID_HANDLE_VALUE == FileDesc->Handle)
    {
        delete FileDesc;
        return NtStatusFromWin32(GetLastError());
    }

    NTSTATUS result = GetFileInfoInternal(FileDesc->Handle, &OpenFileInfo->FileInfo);
    if (!NT_SUCCESS(result)) { delete FileDesc; return result; }
    *PFileDesc = FileDesc;
    ++handles; Touch();
    return result;
}

NTSTATUS Ptfs::Overwrite(
    PVOID FileNode,
    PVOID FileDesc,
    UINT32 FileAttributes,
    BOOLEAN ReplaceFileAttributes,
    UINT64 AllocationSize,
    FileInfo *FileInfo)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    FILE_BASIC_INFO BasicInfo = { 0 };
    FILE_ALLOCATION_INFO AllocationInfo = { 0 };
    FILE_ATTRIBUTE_TAG_INFO AttributeTagInfo;

    if (ReplaceFileAttributes)
    {
        if (0 == FileAttributes)
            FileAttributes = FILE_ATTRIBUTE_NORMAL;

        BasicInfo.FileAttributes = FileAttributes;
        if (!SetFileInformationByHandle(Handle,
            FileBasicInfo, &BasicInfo, sizeof BasicInfo))
            return NtStatusFromWin32(GetLastError());
    }
    else if (0 != FileAttributes)
    {
        if (!GetFileInformationByHandleEx(Handle,
            FileAttributeTagInfo, &AttributeTagInfo, sizeof AttributeTagInfo))
            return NtStatusFromWin32(GetLastError());

        BasicInfo.FileAttributes = FileAttributes | AttributeTagInfo.FileAttributes;
        if (BasicInfo.FileAttributes ^ FileAttributes)
        {
            if (!SetFileInformationByHandle(Handle,
                FileBasicInfo, &BasicInfo, sizeof BasicInfo))
                return NtStatusFromWin32(GetLastError());
        }
    }

    if (!SetFileInformationByHandle(Handle,
        FileAllocationInfo, &AllocationInfo, sizeof AllocationInfo))
        return NtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

VOID Ptfs::Cleanup(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR FileName,
    ULONG Flags)
{
    HANDLE Handle = HandleFromFileDesc(FileDesc);

    if (!readOnly && (Flags & CleanupDelete))
    {
        Changed();
        CloseHandle(Handle);

        /* this will make all future uses of Handle to fail with STATUS_INVALID_HANDLE */
        HandleFromFileDesc(FileDesc) = INVALID_HANDLE_VALUE;
    }
}

VOID Ptfs::Close(
    PVOID FileNode,
    PVOID FileDesc0)
{
    std::lock_guard lock(gate);
    PtfsFileDesc *FileDesc = (PtfsFileDesc *)FileDesc0;

    delete FileDesc;
    --handles; Touch();
}

NTSTATUS Ptfs::Read(
    PVOID FileNode,
    PVOID FileDesc,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    PULONG PBytesTransferred)
{
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    OVERLAPPED Overlapped = { 0 };

    Overlapped.Offset = (DWORD)Offset;
    Overlapped.OffsetHigh = (DWORD)(Offset >> 32);

    if (!ReadFile(Handle, Buffer, Length, PBytesTransferred, &Overlapped))
        return NtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::Write(
    PVOID FileNode,
    PVOID FileDesc,
    PVOID Buffer,
    UINT64 Offset,
    ULONG Length,
    BOOLEAN WriteToEndOfFile,
    BOOLEAN ConstrainedIo,
    PULONG PBytesTransferred,
    FileInfo *FileInfo)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    LARGE_INTEGER FileSize;
    OVERLAPPED Overlapped = { 0 };

    if (ConstrainedIo)
    {
        if (!GetFileSizeEx(Handle, &FileSize))
            return NtStatusFromWin32(GetLastError());

        if (Offset >= (UINT64)FileSize.QuadPart)
            return STATUS_SUCCESS;
        if (Offset + Length > (UINT64)FileSize.QuadPart)
            Length = (ULONG)((UINT64)FileSize.QuadPart - Offset);
    }

    if (WriteToEndOfFile && !ConstrainedIo) { Overlapped.Offset = MAXDWORD; Overlapped.OffsetHigh = MAXDWORD; }
    else { Overlapped.Offset = (DWORD)Offset; Overlapped.OffsetHigh = (DWORD)(Offset >> 32); }

    if (!WriteFile(Handle, Buffer, Length, PBytesTransferred, &Overlapped))
        return NtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Ptfs::Flush(
    PVOID FileNode,
    PVOID FileDesc,
    FileInfo *FileInfo)
{
    if (nullptr == FileDesc) return STATUS_SUCCESS;
    HANDLE Handle = HandleFromFileDesc(FileDesc);

    if (0 == Handle)
        return STATUS_SUCCESS;

    if (!FlushFileBuffers(Handle))
        return NtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Ptfs::GetFileInfo(
    PVOID FileNode,
    PVOID FileDesc,
    FileInfo *FileInfo)
{
    HANDLE Handle = HandleFromFileDesc(FileDesc);

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Ptfs::SetBasicInfo(
    PVOID FileNode,
    PVOID FileDesc,
    UINT32 FileAttributes,
    UINT64 CreationTime,
    UINT64 LastAccessTime,
    UINT64 LastWriteTime,
    UINT64 ChangeTime,
    FileInfo *FileInfo)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    FILE_BASIC_INFO BasicInfo = { 0 };

    if (INVALID_FILE_ATTRIBUTES == FileAttributes)
        FileAttributes = 0;
    else if (0 == FileAttributes)
        FileAttributes = FILE_ATTRIBUTE_NORMAL;

    BasicInfo.FileAttributes = FileAttributes;
    BasicInfo.CreationTime.QuadPart = CreationTime;
    BasicInfo.LastAccessTime.QuadPart = LastAccessTime;
    BasicInfo.LastWriteTime.QuadPart = LastWriteTime;
    //BasicInfo.ChangeTime = ChangeTime;

    if (!SetFileInformationByHandle(Handle,
        FileBasicInfo, &BasicInfo, sizeof BasicInfo))
        return NtStatusFromWin32(GetLastError());

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Ptfs::SetFileSize(
    PVOID FileNode,
    PVOID FileDesc,
    UINT64 NewSize,
    BOOLEAN SetAllocationSize,
    FileInfo *FileInfo)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    FILE_ALLOCATION_INFO AllocationInfo;
    FILE_END_OF_FILE_INFO EndOfFileInfo;

    if (SetAllocationSize)
    {
        /*
         * This file system does not maintain AllocationSize, although NTFS clearly can.
         * However it must always be FileSize <= AllocationSize and NTFS will make sure
         * to truncate the FileSize if it sees an AllocationSize < FileSize.
         *
         * If OTOH a very large AllocationSize is passed, the call below will increase
         * the AllocationSize of the underlying file, although our file system does not
         * expose this fact. This AllocationSize is only temporary as NTFS will reset
         * the AllocationSize of the underlying file when it is closed.
         */

        AllocationInfo.AllocationSize.QuadPart = NewSize;

        if (!SetFileInformationByHandle(Handle,
            FileAllocationInfo, &AllocationInfo, sizeof AllocationInfo))
            return NtStatusFromWin32(GetLastError());
    }
    else
    {
        EndOfFileInfo.EndOfFile.QuadPart = NewSize;

        if (!SetFileInformationByHandle(Handle,
            FileEndOfFileInfo, &EndOfFileInfo, sizeof EndOfFileInfo))
            return NtStatusFromWin32(GetLastError());
    }

    return GetFileInfoInternal(Handle, FileInfo);
}

NTSTATUS Ptfs::CanDelete(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR FileName)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    FILE_DISPOSITION_INFO DispositionInfo;

    DispositionInfo.DeleteFile = TRUE;

    if (!SetFileInformationByHandle(Handle,
        FileDispositionInfo, &DispositionInfo, sizeof DispositionInfo))
        return NtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::Rename(
    PVOID FileNode,
    PVOID FileDesc,
    PWSTR FileName,
    PWSTR NewFileName,
    BOOLEAN ReplaceIfExists)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    WCHAR FullPath[FULLPATH_SIZE], NewFullPath[FULLPATH_SIZE];

    if (!ConcatPath(FileName, FullPath))
        return STATUS_OBJECT_NAME_INVALID;

    if (!ConcatPath(NewFileName, NewFullPath))
        return STATUS_OBJECT_NAME_INVALID;

    if (!MoveFileExW(FullPath, NewFullPath, ReplaceIfExists ? MOVEFILE_REPLACE_EXISTING : 0))
        return NtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::GetSecurity(
    PVOID FileNode,
    PVOID FileDesc,
    PSECURITY_DESCRIPTOR SecurityDescriptor,
    SIZE_T *PSecurityDescriptorSize)
{
    HANDLE Handle = HandleFromFileDesc(FileDesc);
    DWORD SecurityDescriptorSizeNeeded;

    if (!GetKernelObjectSecurity(Handle,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        SecurityDescriptor, (DWORD)*PSecurityDescriptorSize, &SecurityDescriptorSizeNeeded))
    {
        *PSecurityDescriptorSize = SecurityDescriptorSizeNeeded;
        return NtStatusFromWin32(GetLastError());
    }

    *PSecurityDescriptorSize = SecurityDescriptorSizeNeeded;

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::SetSecurity(
    PVOID FileNode,
    PVOID FileDesc,
    SECURITY_INFORMATION SecurityInformation,
    PSECURITY_DESCRIPTOR ModificationDescriptor)
{
    if (readOnly) return STATUS_MEDIA_WRITE_PROTECTED;
    Changed();
    HANDLE Handle = HandleFromFileDesc(FileDesc);

    if (!SetKernelObjectSecurity(Handle, SecurityInformation, ModificationDescriptor))
        return NtStatusFromWin32(GetLastError());

    return STATUS_SUCCESS;
}

NTSTATUS Ptfs::ReadDirectory(
    PVOID FileNode,
    PVOID FileDesc0,
    PWSTR Pattern,
    PWSTR Marker,
    PVOID Buffer,
    ULONG Length,
    PULONG PBytesTransferred)
{
    PtfsFileDesc *FileDesc = (PtfsFileDesc *)FileDesc0;
    return BufferedReadDirectory(&FileDesc->DirBuffer,
        FileNode, FileDesc, Pattern, Marker, Buffer, Length, PBytesTransferred);
}

NTSTATUS Ptfs::ReadDirectoryEntry(
    PVOID FileNode,
    PVOID FileDesc0,
    PWSTR Pattern,
    PWSTR Marker,
    PVOID *PContext,
    DirInfo *DirInfo)
{
    PtfsFileDesc *FileDesc = (PtfsFileDesc *)FileDesc0;
    HANDLE Handle = FileDesc->Handle;
    WCHAR FullPath[FULLPATH_SIZE];
    ULONG Length, PatternLength;
    HANDLE FindHandle;
    WIN32_FIND_DATAW FindData;

    if (0 == *PContext)
    {
        WCHAR AllEntries[] = L"*";
        if (0 == Pattern)
            Pattern = AllEntries;
        PatternLength = (ULONG)wcslen(Pattern);

        Length = GetFinalPathNameByHandleW(Handle, FullPath, FULLPATH_SIZE - 1, 0);
        if (0 == Length)
            return NtStatusFromWin32(GetLastError());
        if (Length + 1 + PatternLength >= FULLPATH_SIZE)
            return STATUS_OBJECT_NAME_INVALID;

        if (L'\\' != FullPath[Length - 1])
            FullPath[Length++] = L'\\';
        memcpy(FullPath + Length, Pattern, PatternLength * sizeof(WCHAR));
        FullPath[Length + PatternLength] = L'\0';

        FindHandle = FindFirstFileW(FullPath, &FindData);
        if (INVALID_HANDLE_VALUE == FindHandle)
            return STATUS_NO_MORE_FILES;

        *PContext = FindHandle;
    }
    else
    {
        FindHandle = *PContext;
        if (!FindNextFileW(FindHandle, &FindData))
        {
            FindClose(FindHandle);
            return STATUS_NO_MORE_FILES;
        }
    }

    memset(DirInfo, 0, sizeof *DirInfo);
    Length = (ULONG)wcslen(FindData.cFileName);
    DirInfo->Size = (UINT16)(FIELD_OFFSET(Ptfs::DirInfo, FileNameBuf) + Length * sizeof(WCHAR));
    DirInfo->FileInfo.FileAttributes = FindData.dwFileAttributes;
    DirInfo->FileInfo.ReparseTag = 0;
    DirInfo->FileInfo.FileSize =
        ((UINT64)FindData.nFileSizeHigh << 32) | (UINT64)FindData.nFileSizeLow;
    DirInfo->FileInfo.AllocationSize = (DirInfo->FileInfo.FileSize + ALLOCATION_UNIT - 1)
        / ALLOCATION_UNIT * ALLOCATION_UNIT;
    DirInfo->FileInfo.CreationTime = ((PLARGE_INTEGER)&FindData.ftCreationTime)->QuadPart;
    DirInfo->FileInfo.LastAccessTime = ((PLARGE_INTEGER)&FindData.ftLastAccessTime)->QuadPart;
    DirInfo->FileInfo.LastWriteTime = ((PLARGE_INTEGER)&FindData.ftLastWriteTime)->QuadPart;
    DirInfo->FileInfo.ChangeTime = DirInfo->FileInfo.LastWriteTime;
    DirInfo->FileInfo.IndexNumber = 0;
    DirInfo->FileInfo.HardLinks = 0;
    memcpy(DirInfo->FileNameBuf, FindData.cFileName, Length * sizeof(WCHAR));

    return STATUS_SUCCESS;
}

