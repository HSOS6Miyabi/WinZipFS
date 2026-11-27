#include "../src/api.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <map>
#include <cstdlib>
#include <array>
namespace fs=std::filesystem;
using namespace std::chrono_literals;
void require(bool condition,const char* what) {
    if(!condition) {
        wchar_t error[512]{};wz_error(error,512);
        std::wcerr<<error<<L"\n";
        throw std::runtime_error(what);
    }
}
std::string read(fs::path const& path) {
    std::ifstream in(path,std::ios::binary);
    require(bool(in),"open for reading");
    return {std::istreambuf_iterator<char>(in),std::istreambuf_iterator<char>()};
}
void write(fs::path const& path,std::string value) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<value;out.close();require(bool(out),"write file");
}
std::wstring mount(fs::path const& archive,WzOptions o) {
    wchar_t drive[16]{};
    require(wz_mount(archive.c_str(),&o,drive,16)==1,"mount");
    return drive;
}
WzVolume volume(std::wstring const& drive) {
    for(int i=0;i<wz_count();++i) {
        WzVolume v{};if(wz_get(i,&v)&&drive==v.mount)return v;
    }
    throw std::runtime_error("volume not found");
}
void waitClosed(std::wstring const& drive) {
    for(int i=0;i<300;++i) {
        if(volume(drive).handles==0)return;
        std::this_thread::sleep_for(100ms);
    }
    throw std::runtime_error("WinFsp handles did not close");
}
void eject(std::wstring const& drive) {
    waitClosed(drive);require(wz_eject(drive.c_str())==1,"eject");
    for(int i=0;i<100 && (GetLogicalDrives()&(1u<<(drive[0]-L'A')));++i) std::this_thread::sleep_for(50ms);
    require((GetLogicalDrives()&(1u<<(drive[0]-L'A')))==0,"drive still mounted");
}
void roundtrip(fs::path const& archive) {
    std::wcout<<L"Testing "<<archive<<std::endl;
    WzOptions o{};wz_default_options(&o);o.batch=1;
    auto original=read(archive);
    auto drive=mount(archive,o);
    auto root=fs::path(drive+L"\\");
    require(!volume(drive).readOnly,"writable volume");
    require(read(root/L"sample.txt")=="original","original contents");
    require(fs::is_directory(root/L"empty"),"empty directory");
    require(mount(archive,o)==drive&&wz_count()==1,"duplicate mount");
    auto file=root/L"sample.txt";
    HANDLE open=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,0,nullptr);
    require(open!=INVALID_HANDLE_VALUE,"native open");
    require(volume(drive).handles>0,"handle count");
    require(wz_eject(drive.c_str())==0,"cannot eject open file");
    CloseHandle(open);
    fs::create_directory(root/L"new folder");
    write(root/L"new folder"/L"\u6d4b\u8bd5.txt","abcdef");
    fs::resize_file(root/L"new folder"/L"\u6d4b\u8bd5.txt",3);
    fs::rename(root/L"new folder"/L"\u6d4b\u8bd5.txt",root/L"renamed.txt");
    fs::remove(root/L"new folder");fs::remove(file);
    write(root/L"created.txt","updated");
    waitClosed(drive);
    require(volume(drive).dirty,"dirty state");
    require(wz_tick()==1&&read(archive)==original,"batch retained");
    require(wz_flush(drive.c_str())==1,"save archive");
    require(!volume(drive).dirty&&read(archive)!=original,"commit changed original");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(read(root/L"created.txt")=="updated","persisted create");
    require(read(root/L"renamed.txt")=="abc","persisted truncate and rename");
    require(!fs::exists(root/L"sample.txt"),"persisted deletion");
    require(fs::is_directory(root/L"empty"),"persisted empty directory");
    for(const auto& p:std::vector<fs::path>{root/L"created.txt",root/L"renamed.txt",root/L"empty"})
        require(fs::remove(p),"remove final entries");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(fs::is_empty(root),"empty archive persisted");
    write(root/L"recreated.txt","recreated");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(read(root/L"recreated.txt")=="recreated","write to previously empty archive");
    eject(drive);
}
void safety(fs::path const& archive,const fs::path& directory) {
    WzOptions o{};wz_default_options(&o);o.batch=1;
    const auto before=read(archive);
    wchar_t buffer[16]{};
    require(wz_mount_readonly(archive.c_str(),&o,buffer,16)==1,"forced read-only");
    std::wstring drive=buffer;auto root=fs::path(drive+L"\\");
    require(volume(drive).readOnly,"forced read-only flag");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(wz_mount_readonly(archive.c_str(),&o,buffer,16)==0,"reject conflicting mount mode");
    auto file=root/L"sample.txt";
    HANDLE h=CreateFileW(file.c_str(),DELETE|GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"open for cancellable deletion");
    FILE_DISPOSITION_INFO disposition{TRUE};
    require(SetFileInformationByHandle(h,FileDispositionInfo,&disposition,sizeof disposition)!=0,"mark delete");
    disposition.DeleteFile=FALSE;
    require(SetFileInformationByHandle(h,FileDispositionInfo,&disposition,sizeof disposition)!=0,"cancel delete");
    CloseHandle(h);waitClosed(drive);
    require(read(file)=="original","cancelled deletion retained file");
    write(root/L"cached.txt","unsaved");
    waitClosed(drive);
    h=CreateFileW(archive.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"hold external writer");
    require(wz_flush(drive.c_str())==0,"reject active external writer");
    CloseHandle(h);
    require(read(archive)==before&&volume(drive).dirty,"original and pending data preserved");
    auto copy=directory/L"export-copy.7z";
    require(wz_save_copy(drive.c_str(),copy.c_str())==1,"export unsaved data");
    require(wz_save_copy(drive.c_str(),copy.c_str())==0,"do not overwrite export");
    require(read(archive)==before,"export does not alter original");
    require(wz_discard(drive.c_str())==1,"discard pending modifications");
    require(read(archive)==before,"discard does not alter original");
    drive=mount(copy,o);root=fs::path(drive+L"\\");
    require(read(root/L"cached.txt")=="unsaved","exported contents");
    require(read(root/L"sample.txt")=="original","exported original data");
    fs::copy_file(archive,root/L"nested.zip");
    require(wz_mount((root/L"nested.zip").c_str(),&o,buffer,16)==0,"reject recursive mount");
    waitClosed(drive);require(wz_discard(drive.c_str())==1,"discard copy");
}
void password(fs::path const& archive) {
    WzOptions o{};wz_default_options(&o);
    const auto before=read(archive);wchar_t mountPoint[16]{};
    require(wz_mount(archive.c_str(),&o,mountPoint,16)==0&&wz_password_required(),"password prompt required");
    require(wz_mount_password(archive.c_str(),&o,L"wrong-password",mountPoint,16)==0,"wrong password rejected");
    require(wz_count()==0,"wrong password leaves no mount");
    require(wz_mount_password(archive.c_str(),&o,L"TestFixturePassword",mountPoint,16)==1,"password mount");
    std::wstring drive=mountPoint;
    require(volume(drive).readOnly,"protected archive read-only");
    require(read(fs::path(drive+L"\\")/L"sample.txt")=="original","decrypted data");
    eject(drive);
    require(read(archive)==before,"protected archive unchanged");
}
fs::path cachePath() {
    auto value=_wgetenv(L"LOCALAPPDATA");
    require(value!=nullptr,"local application data");
    return fs::path(value)/L"WinZipFS"/L"cache";
}
std::map<std::wstring,uint64_t> cacheFiles() {
    std::map<std::wstring,uint64_t> result;
    auto path=cachePath();
    if(fs::is_directory(path))for(auto const& entry:fs::recursive_directory_iterator(path))
        if(entry.is_regular_file())result.emplace(entry.path().wstring(),entry.file_size());
    return result;
}
void recovery(fs::path const& archive,const fs::path& output) {
    auto before=read(archive);
    auto cacheBefore=cacheFiles();
    wchar_t exe[32768]{};GetModuleFileNameW(nullptr,exe,32768);
    std::wstring command=L"\""+std::wstring(exe)+L"\" --crash-fixture \""+archive.wstring()+L"\"";
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION child{};
    require(CreateProcessW(exe,command.data(),nullptr,nullptr,FALSE,0,nullptr,nullptr,&startup,&child)!=0,"start recovery fixture");
    CloseHandle(child.hThread);
    DWORD wait=WaitForSingleObject(child.hProcess,30000),code=1;
    GetExitCodeProcess(child.hProcess,&code);CloseHandle(child.hProcess);
    require(wait==WAIT_OBJECT_0&&code==0,"recovery fixture completed");
    require(read(archive)==before&&cacheFiles()==cacheBefore,"terminated RAM session never wrote archive or cache");
    WzOptions o{};wz_default_options(&o);
    auto drive=mount(archive,o);
    require(!fs::exists(fs::path(drive+L"\\")/L"recovered.txt"),"uncommitted memory data not present after crash");
    eject(drive);

    // The recovery manager can still export a saved recovery directory.
    auto saved=cachePath()/L"{e8415f0b-15a1-4765-bf8a-ae4f17469326}";
    fs::create_directories(saved/L"files");
    write(saved/L"archive.txt",archive.string()+"\n");
    write(saved/L"ready","1");write(saved/L"files"/L"recovered.txt","saved recovery data");
    int n=wz_recovery_count();require(n>0,"saved recovery found");
    WzRecovery found{};bool match=false;
    for(int i=0;i<n;++i) {
        WzRecovery item{};require(wz_recovery_get(i,&item)==1,"read recovery entry");
        if(fs::path(item.archive)==fs::canonical(archive)){found=item;match=true;break;}
    }
    require(match,"recovery manifest matches");
    require(wz_recovery_export(found.id,output.c_str())==1,"export recovery");
    require(wz_recovery_export(found.id,output.c_str())==0,"do not overwrite recovery");
    drive=mount(output,o);
    require(read(fs::path(drive+L"\\")/L"recovered.txt")=="saved recovery data","recovered contents");
    eject(drive);
    require(wz_recovery_discard(found.id)==1,"discard exported recovery session");
    require(wz_recovery_discard(found.id)==0,"removed recovery unavailable");
}
void readonly(fs::path const& archive) {
    WzOptions o{};wz_default_options(&o);
    auto drive=mount(archive,o);auto root=drive+L"\\";
    require(volume(drive).readOnly,"read-only session");
    DWORD flags=0;
    require(GetVolumeInformationW(root.c_str(),nullptr,0,nullptr,nullptr,&flags,nullptr,0)!=0,
            "query volume information");
    require((flags&FILE_READ_ONLY_VOLUME)!=0,"Windows volume is read-only");
    HANDLE h=CreateFileW((root+L"new.txt").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
    if(h!=INVALID_HANDLE_VALUE)CloseHandle(h);
    require(h==INVALID_HANDLE_VALUE,"write rejected");
    require(read(fs::path(root)/L"sample.txt")=="original","read-only data");
    eject(drive);
}
void conflict(fs::path const& archive) {
    WzOptions o{};wz_default_options(&o);o.batch=1;
    auto original=read(archive);
    FILETIME modified{};
    HANDLE h=CreateFileW(archive.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                        nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"open original attributes");
    BOOL ok=GetFileTime(h,nullptr,nullptr,&modified);CloseHandle(h);
    require(ok!=0,"original timestamp");
    auto drive=mount(archive,o);
    auto path=fs::path(drive+L"\\")/L"conflict.txt";
    write(path,"cached modification");waitClosed(drive);
    auto external=original+"external modification";
    write(archive,external);
    require(wz_flush(drive.c_str())==0,"external modification rejected");
    require(read(archive)==external,"external archive preserved");
    require(volume(drive).dirty&&read(path)=="cached modification","cached edits retained");
    waitClosed(drive);
    write(archive,original);
    h=CreateFileW(archive.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                  nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"restore original attributes");
    ok=SetFileTime(h,nullptr,nullptr,&modified);CloseHandle(h);
    require(ok!=0,"restore original timestamp");
    require(wz_flush(drive.c_str())==1,"recover after external conflict");
    eject(drive);
    drive=mount(archive,o);
    require(read(fs::path(drive+L"\\")/L"conflict.txt")=="cached modification","recovered edits persisted");
    eject(drive);
}
template<class Predicate>
void waitUntil(Predicate&& condition,std::chrono::seconds limit,const char* what,std::wstring const& drive) {
    const auto end=std::chrono::steady_clock::now()+limit;
    do {
        require(wz_tick()==1,"periodic maintenance");
        if(condition()) return;
        std::this_thread::sleep_for(250ms);
    } while(std::chrono::steady_clock::now()<end);
    try {
        auto v=volume(drive);
        std::wcerr<<L"Session status: "<<v.status<<L"; handles="<<v.handles<<L"; dirty="<<v.dirty<<std::endl;
    }catch(...) {}
    throw std::runtime_error(what);
}
void idle(fs::path const& archive) {
    WzOptions o{};wz_default_options(&o);o.idleMinutes=1;o.delaySeconds=1;
    auto original=read(archive);
    auto drive=mount(archive,o);auto file=fs::path(drive+L"\\")/L"auto.txt";
    write(file,"auto");
    HANDLE h=CreateFileW(file.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"open file during automatic writeback");
    std::this_thread::sleep_for(2s);
    require(wz_tick()==1,"maintenance with an open handle");
    require(volume(drive).dirty&&read(archive)==original,"writeback deferred while handle is open");
    CloseHandle(h);
    waitClosed(drive);
    waitUntil([&]{return !volume(drive).dirty;},30s,"automatic writeback did not complete",drive);
    require(read(archive)!=original,"automatic writeback changed original");
    waitUntil([]{return wz_count()==0;},90s,"automatic idle eject did not complete",drive);
    drive=mount(archive,o);require(read(fs::path(drive+L"\\")/L"auto.txt")=="auto","auto commit survived");
    eject(drive);
}
void incremental(fs::path const& archive) {
    std::cout<<"Testing incremental ZIP update"<<std::endl;
    WzOptions o{};wz_default_options(&o);o.batch=1;o.level=9;o.method=2;
    auto drive=mount(archive,o);auto root=fs::path(drive+L"\\");
    auto path=root/L"changed.txt";
    HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"read original timestamp");
    FILETIME modified{};
    require(GetFileTime(h,nullptr,nullptr,&modified)!=0,"get original timestamp");
    CloseHandle(h);
    write(path,std::string(12288,'B'));
    h=CreateFileW(path.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"restore file timestamp");
    require(SetFileTime(h,nullptr,nullptr,&modified)!=0,"restore modified time");
    CloseHandle(h);
    waitClosed(drive);
    require(wz_flush(drive.c_str())==1,"incremental save");
    require(std::wstring_view(volume(drive).status).find(L"incremental")!=std::wstring_view::npos,"incremental status");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(read(root/L"changed.txt")==std::string(12288,'B'),"same-size timestamp-restored change persisted");
    require(read(root/L"unchanged.bin")==std::string(1048576,'A'),"unchanged contents");
    eject(drive);
    fs::copy_file(archive,archive.parent_path()/L"incremental-before-recompress.zip",fs::copy_options::overwrite_existing);
    drive=mount(archive,o);waitClosed(drive);
    require(wz_recompress(drive.c_str())==1,"full recompression");
    require(std::wstring_view(volume(drive).status).find(L"recompressed")!=std::wstring_view::npos,"recompression status");
    eject(drive);
}
std::string rangeRead(HANDLE h,uint64_t offset,DWORD size) {
    LARGE_INTEGER position{};position.QuadPart=static_cast<LONGLONG>(offset);
    require(SetFilePointerEx(h,position,nullptr,FILE_BEGIN)!=0,"random seek");
    std::string result(size,'\0');DWORD count=0;
    require(ReadFile(h,result.data(),size,&count,nullptr)!=0&&count==size,"random range read");
    return result;
}
void randomAccess(fs::path const& archive,bool direct) {
    constexpr uint64_t MiB=1024ull*1024;
    constexpr uint64_t length=64*MiB;
    auto disk=cacheFiles();
    require(wz_memory_limit(16)==1,"set bounded cache");
    WzMemoryStats before{},after{};wz_memory_stats(&before);
    WzOptions o{};wz_default_options(&o);o.batch=1;
    auto drive=mount(archive,o);auto root=fs::path(drive+L"\\");
    require(fs::file_size(root/L"large.bin")==length,"logical large file size");
    wz_memory_stats(&after);
    require(after.decodedBytes==before.decodedBytes&&after.cleanBytes==before.cleanBytes,"mount and metadata only read index");
    require(cacheFiles()==disk,"mount creates no disk cache");
    HANDLE h=CreateFileW((root/L"large.bin").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"open large archive item");
    for(uint64_t offset:{length-8192,2*MiB+137,39*MiB+73,8*MiB+13,length-8192}) {
        WzMemoryStats previous{};wz_memory_stats(&previous);
        auto bytes=rangeRead(h,offset,4096);
        if(!direct&&offset==39*MiB+73) {
            WzMemoryStats current{};wz_memory_stats(&current);
            require(current.decodedBytes-previous.decodedBytes<20*MiB,"DEFLATE random read resumes from indexed block");
        }
        for(size_t i=0;i<bytes.size();++i)require(static_cast<unsigned char>(bytes[i])==((offset+i)/MiB)%251,"random offset data");
    }
    wz_memory_stats(&after);
    require(after.cleanBytes+after.dirtyBytes<=after.limitBytes,"memory limit respected");
    if(direct)require(after.directReadBytes>before.directReadBytes&&after.decodedBytes==before.decodedBytes,"direct stream does not decompress payload");
    LARGE_INTEGER offset{};offset.QuadPart=static_cast<LONGLONG>(31*MiB+99);
    require(SetFilePointerEx(h,offset,nullptr,FILE_BEGIN)!=0,"random write seek");
    DWORD written=0;const char edit[]="random edit";
    require(WriteFile(h,edit,sizeof(edit)-1,&written,nullptr)!=0&&written==sizeof(edit)-1,"random write");
    require(rangeRead(h,31*MiB+99,sizeof(edit)-1)==edit,"overlay read");
    require(rangeRead(h,31*MiB+90,9)==std::string(9,char(31)),"partial write preserves previous bytes");
    offset.QuadPart=static_cast<LONGLONG>(length+8*MiB);
    require(SetFilePointerEx(h,offset,nullptr,FILE_BEGIN)!=0&&WriteFile(h,edit,sizeof(edit)-1,&written,nullptr)!=0,"sparse extension");
    require(rangeRead(h,length+MiB,4096)==std::string(4096,'\0'),"sparse range zero");
    CloseHandle(h);waitClosed(drive);
    wz_memory_stats(&after);
    require(after.dirtyBytes<=MiB&&after.cleanBytes+after.dirtyBytes<=after.limitBytes,"only edited pages allocated");
    require(cacheFiles()==disk,"read and write create no disk cache");
    require(wz_flush(drive.c_str())==1,"save memory overlay");
    require(cacheFiles()==disk,"save creates no extracted cache");
    eject(drive);
    wz_memory_stats(&after);
    require(after.cleanBytes==0&&after.dirtyBytes==0,"release all archive pages");
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    h=CreateFileW((root/L"large.bin").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                 nullptr,OPEN_EXISTING,0,nullptr);
    require(h!=INVALID_HANDLE_VALUE,"reopen modified archive");
    require(rangeRead(h,31*MiB+99,sizeof(edit)-1)==edit,"saved random edit");
    require(rangeRead(h,length+8*MiB,sizeof(edit)-1)==edit,"saved sparse extension");
    CloseHandle(h);eject(drive);
    require(wz_memory_limit(512)==1,"restore cache limit");
}
void memoryPressure(fs::path const& archive) {
    constexpr uint64_t MiB=1024ull*1024;
    auto disk=cacheFiles();
    require(wz_memory_limit(16)==1,"limit before memory pressure");
    WzOptions o{};wz_default_options(&o);o.batch=1;
    auto drive=mount(archive,o);auto root=fs::path(drive+L"\\");
    HANDLE file=CreateFileW((root/L"large.bin").c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                            nullptr,OPEN_EXISTING,FILE_FLAG_WRITE_THROUGH,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"open compressed file under memory pressure");
    LARGE_INTEGER offset{};offset.QuadPart=20*MiB+99;
    require(SetFilePointerEx(file,offset,nullptr,FILE_BEGIN)!=0,"seek compressed file");
    DWORD written=0;char patch='Q';
    require(WriteFile(file,&patch,1,&written,nullptr)&&written==1,"dirty partial compressed page");
    CloseHandle(file);
    file=CreateFileW((root/L"pressure.bin").c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_FLAG_WRITE_THROUGH,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"open pressure file");
    std::vector<char> block(256*1024,'P');
    uint64_t size=0;bool exhausted=false;
    for(unsigned i=0;i<64;++i) {
        written=0;
        if(!WriteFile(file,block.data(),static_cast<DWORD>(block.size()),&written,nullptr)) {
            require(GetLastError()==ERROR_DISK_FULL||GetLastError()==ERROR_HANDLE_DISK_FULL,"memory limit reports capacity");
            exhausted=true;break;
        }
        require(written==block.size(),"complete pressure page");
        size+=written;
    }
    CloseHandle(file);waitClosed(drive);
    require(exhausted&&size>0,"dirty write budget enforced");
    WzMemoryStats stats{};wz_memory_stats(&stats);
    require(stats.dirtyBytes<=8*MiB&&stats.cleanBytes+stats.dirtyBytes<=stats.limitBytes,"decoder reserve retained");
    require(cacheFiles()==disk,"memory pressure never spills to disk");
    require(wz_flush(drive.c_str())==1,"save dirty archive after write budget exhaustion");
    eject(drive);
    drive=mount(archive,o);root=fs::path(drive+L"\\");
    require(fs::file_size(root/L"pressure.bin")==size,"all accepted writes persisted");
    file=CreateFileW((root/L"large.bin").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                     nullptr,OPEN_EXISTING,0,nullptr);
    require(file!=INVALID_HANDLE_VALUE,"reopen after pressure save");
    require(rangeRead(file,20*MiB+99,1)=="Q","partial compressed edit persisted");
    CloseHandle(file);eject(drive);
    require(cacheFiles()==disk,"save under memory pressure creates no extracted cache");
    require(wz_memory_limit(512)==1,"restore cache limit");
}
void uncMode(fs::path const& fixtures) {
    std::wcout<<L"Testing UNC on-demand namespace"<<std::endl;
    auto first=fixtures/L"unc first.zip",second=fixtures/L"unc second.7z";
    fs::copy_file(fixtures/L"ui.zip",first,fs::copy_options::overwrite_existing);
    fs::copy_file(fixtures/L"tray.7z",second,fs::copy_options::overwrite_existing);
    WzOptions o{};wz_default_options(&o);o.batch=1;
    DWORD drives=GetLogicalDrives();
    require(wz_unc_start(&o,2)==1,"start UNC namespace");
    wchar_t root[WZ_MOUNT_CAPACITY]{},a[WZ_MOUNT_CAPACITY]{},b[WZ_MOUNT_CAPACITY]{},cab[WZ_MOUNT_CAPACITY]{};
    require(wz_unc_root(root,WZ_MOUNT_CAPACITY)==1,"get UNC root");
    require(wz_unc_path(first.c_str(),a,WZ_MOUNT_CAPACITY)==1&&wz_unc_path(second.c_str(),b,WZ_MOUNT_CAPACITY)==1,"generate UNC addresses");
    require(wz_unc_path((fixtures/L"readonly.cab").c_str(),cab,WZ_MOUNT_CAPACITY)==1,"generate readonly UNC");
    wchar_t explicitRo[WZ_MOUNT_CAPACITY]{};
    require(wz_unc_path_readonly(first.c_str(),explicitRo,WZ_MOUNT_CAPACITY)==1,"generate explicit readonly address");
    require(wz_count()==0&&GetLogicalDrives()==drives,"no eager archive session or drive letter");
    auto disk=cacheFiles();
    wchar_t large[WZ_MOUNT_CAPACITY]{};
    require(wz_unc_path((fixtures/L"random.zip").c_str(),large,WZ_MOUNT_CAPACITY)==1,"large ZIP UNC path");
    HANDLE largeFile=CreateFileW((fs::path(large)/L"large.bin").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    require(largeFile!=INVALID_HANDLE_VALUE,"open large UNC archive item");
    require(rangeRead(largeFile,39ull*1024*1024+73,4096)==std::string(4096,char(39)),"UNC random offset data");
    CloseHandle(largeFile);waitClosed(large);
    require(cacheFiles()==disk,"UNC random access creates no disk cache");
    require(wz_eject(large)==1&&wz_count()==0,"close large UNC archive");
    require(std::wstring_view(a).starts_with(L"\\\\WinZipFS\\"),"UNC address");
    auto aa=fs::path(a),bb=fs::path(b);
    require(read(aa/L"sample.txt")=="original","direct UNC read creates a session");
    require(volume(a).uncMode&&wz_count()==1,"first lazy session");
    auto alias=fixtures/L"unc hardlink.zip";
    fs::create_hard_link(first,alias);
    auto originalTime=fs::last_write_time(first);
    fs::last_write_time(first,originalTime+std::chrono::seconds(4));
    wchar_t aliasDrive[16]{};
    require(wz_mount(alias.c_str(),&o,aliasDrive,16)==0,"reject hardlink alias after external timestamp change");
    fs::last_write_time(first,originalTime);
    fs::remove(alias);
    require(read(bb/L"sample.txt")=="original","second direct UNC read");
    require(wz_count()==2&&GetLogicalDrives()==drives,"concurrent UNC archives no drive letters");
    auto before=read(first);
    write(aa/L"edited.txt","UNC batch");
    fs::create_directory(aa/L"folder");
    write(aa/L"folder"/L"temporary.txt","rename");
    fs::rename(aa/L"folder"/L"temporary.txt",aa/L"renamed.txt");
    fs::remove(aa/L"folder");
    require(read(aa/L"renamed.txt")=="rename","UNC rename");
    require(read(first)==before,"batch deferred");
    HANDLE held=CreateFileW((aa/L"edited.txt").c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                           nullptr,OPEN_EXISTING,0,nullptr);
    require(held!=INVALID_HANDLE_VALUE,"hold UNC handle");
    std::this_thread::sleep_for(3s);
    require(wz_tick()==1&&volume(a).handles>0,"open handle prevents expiry");
    wchar_t drive[16]{};
    require(wz_mount(first.c_str(),&o,drive,16)==0,"reject duplicate drive mount");
    CloseHandle(held);waitClosed(a);
    for(int i=0;i<100&&wz_count();++i){std::this_thread::sleep_for(100ms);require(wz_tick()==1,"UNC idle save");}
    require(wz_count()==0&&read(first)!=before,"UNC idle expiry saved and released");
    require(read(aa/L"edited.txt")=="UNC batch","same UNC path reopens saved archive");
    require(GetLogicalDrives()==drives,"never allocates a drive letter");
    waitClosed(a);require(wz_eject(a)==1,"explicit UNC close");
    require(wz_count()==0,"UNC session released");
    require(read(fs::path(explicitRo)/L"sample.txt")=="original"&&volume(explicitRo).readOnly,"explicit read-only address");
    waitClosed(explicitRo);require(wz_eject(explicitRo)==1,"release explicit read-only");
    require(read(fs::path(explicitRo)/L"sample.txt")=="original"&&volume(explicitRo).readOnly,"read-only persists across idle reopens");
    require((GetFileAttributesW((fs::path(explicitRo)/L"sample.txt").c_str())&FILE_ATTRIBUTE_READONLY)!=0,"read-only file attributes");
    waitClosed(explicitRo);require(wz_eject(explicitRo)==1,"release read-only reopening");
    require(read(fs::path(cab)/L"sample.txt")=="original"&&volume(cab).readOnly,"UNC readonly format");
    HANDLE denied=CreateFileW((fs::path(cab)/L"new.txt").c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,0,nullptr);
    require(denied==INVALID_HANDLE_VALUE,"UNC readonly write rejected");
    waitClosed(cab);require(wz_eject(cab)==1,"close readonly UNC");
    o.batch=0;o.delaySeconds=1;
    require(wz_unc_settings(&o,6)==1,"configure automatic writeback and idle expiry");
    write(aa/L"automatic.txt","automatic UNC save");
    waitClosed(a);
    auto closedAt=std::chrono::steady_clock::now();
    std::this_thread::sleep_until(closedAt+1500ms);
    require(wz_tick()==1&&wz_count()==1&&!volume(a).dirty,"automatic UNC writeback before expiry");
    std::this_thread::sleep_until(closedAt+6300ms);
    require(wz_tick()==1&&wz_count()==0,"writeback does not extend UNC idle lifetime");
    require(read(aa/L"automatic.txt")=="automatic UNC save","automatic writeback persisted");
    waitClosed(a);require(wz_eject(a)==1,"release automatic writeback session");
    require(wz_shutdown()==1&&GetLogicalDrives()==drives,"stop UNC namespace");
}
int wmain(int argc,wchar_t** argv) {
    try {
        if(argc==3&&std::wstring_view(argv[1])==L"--crash-fixture") {
            WzOptions o{};wz_default_options(&o);o.batch=1;
            auto drive=mount(argv[2],o);
            write(fs::path(drive+L"\\")/L"recovered.txt","unsaved after termination");
            waitClosed(drive);
            TerminateProcess(GetCurrentProcess(),0);
            return 1;
        }
        WzOptions o{};wz_default_options(&o);
        require(o.delaySeconds==5 && o.level==5 && wz_count()==0,"defaults");
        wchar_t drive[16]{},error[512]{};
        require(wz_mount(L"Z:\\file-that-does-not-exist.zip",&o,drive,16)==0,"missing archive");
        wz_error(error,512);require(*error&&wz_count()==0,"error state");
        require(wz_eject(L"Z:")==0&&wz_flush(L"Z:")==0,"unknown volume");
        if(argc==2) {
            fs::path root=argv[1];
            for(auto ext:{L".zip",L".7z",L".tar",L".tar.gz",L".tar.bz2",L".tar.xz"})roundtrip(root/(std::wstring(L"core")+ext));
            for(auto name:{L"traversal.zip",L"collision.zip",L"reserved.zip",L"encrypted.zip"}) {
                wchar_t rejected[16]{};
                require(wz_mount((root/name).c_str(),&o,rejected,16)==0,"unsafe or encrypted archive rejected");
                require(wz_count()==0,"rejected archive left no mounted volume");
            }
            for(auto name:{L"readonly.cab",L"sample.txt.gz",L"sample.txt.bz2",L"sample.txt.xz",L"sample.txt.zst",L"core.tar.zst"}) {
                std::wcout<<L"Testing read-only "<<name<<std::endl;
                readonly(root/name);
            }
            fs::copy_file(root/L"ui.zip",root/L"conflict.zip",fs::copy_options::overwrite_existing);
            std::cout<<"Testing external conflict"<<std::endl;
            conflict(root/L"conflict.zip");
            std::cout<<"Testing password-protected archives"<<std::endl;
            password(root/L"encrypted.zip");password(root/L"encrypted.7z");
            std::cout<<"Testing recovery after termination"<<std::endl;
            recovery(root/L"ui.zip",root/L"recovered.zip");
            fs::copy_file(root/L"ui.zip",root/L"safety.zip",fs::copy_options::overwrite_existing);
            std::cout<<"Testing safety operations"<<std::endl;
            safety(root/L"safety.zip",root);
            std::cout<<"Testing disk volume idle expiry"<<std::endl;
            idle(root/L"idle.zip");
            incremental(root/L"incremental.zip");
            std::cout<<"Testing bounded random ZIP reads and writes"<<std::endl;
            randomAccess(root/L"random.zip",false);
            std::cout<<"Testing memory pressure and reserved writeback capacity"<<std::endl;
            memoryPressure(root/L"random.zip");
            std::cout<<"Testing direct TAR random access"<<std::endl;
            randomAccess(root/L"random.tar",true);
            std::cout<<"Testing direct stored ZIP random access"<<std::endl;
            randomAccess(root/L"random-stored.zip",true);
            uncMode(root);
        }
        require(wz_shutdown()==1,"shutdown");
        std::cout<<"Native tests passed\n";return 0;
    }catch(std::exception const& e) {std::cout<<"FAILED: "<<e.what()<<std::endl;wz_shutdown();return 1;}
}
