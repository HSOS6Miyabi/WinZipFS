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
    eject(drive);
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
int wmain(int argc,wchar_t** argv) {
    try {
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
            readonly(root/L"readonly.cab");
            fs::copy_file(root/L"ui.zip",root/L"conflict.zip",fs::copy_options::overwrite_existing);
            conflict(root/L"conflict.zip");idle(root/L"idle.zip");
        }
        require(wz_shutdown()==1,"shutdown");
        std::cout<<"Native tests passed\n";return 0;
    }catch(std::exception const& e) {std::cerr<<e.what()<<"\n";wz_shutdown();return 1;}
}
