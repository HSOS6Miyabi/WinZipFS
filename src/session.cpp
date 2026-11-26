#define WZ_BUILD
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
#include <memory>
#include <mutex>
#include <vector>
#include <exception>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include "archive.hpp"
#include "filesystem.hpp"

namespace wz {
thread_local std::wstring error;
std::mutex manager;
std::unique_ptr<Bit7zLibrary> engine;
inline std::wstring guid() {
    GUID id{}; if(FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create cache identifier");
    wchar_t s[40]{}; StringFromGUID2(id,s,40); return s;
}
inline fs::path local() {
    PWSTR s=nullptr;
    if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&s))) throw std::runtime_error("Cannot locate LocalAppData");
    fs::path p(s); CoTaskMemFree(s); return p/L"WinZipFS"/L"cache";
}
inline fs::path moduleDir() {
    HMODULE mod=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&wz_mount),&mod);
    wchar_t p[32768]; DWORD n=GetModuleFileNameW(mod,p,32768);
    if(!n || n>=32768) throw std::runtime_error("Cannot locate archive engine");
    return fs::path(p).parent_path();
}
inline const Bit7zLibrary& lib() {
    if(!engine) engine=std::make_unique<Bit7zLibrary>(path8(moduleDir()/L"7z.dll"));
    return *engine;
}
struct Fingerprint {
    BY_HANDLE_FILE_INFORMATION info{};
    explicit Fingerprint(const fs::path& p) {
        HANDLE h=CreateFileW(p.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open original archive");
        BOOL ok=GetFileInformationByHandle(h,&info);CloseHandle(h);
        if(!ok) throw std::runtime_error("Cannot identify original archive");
    }
    bool operator==(const Fingerprint& b) const {
        return info.dwVolumeSerialNumber==b.info.dwVolumeSerialNumber &&
          info.nFileIndexHigh==b.info.nFileIndexHigh && info.nFileIndexLow==b.info.nFileIndexLow &&
          info.nFileSizeHigh==b.info.nFileSizeHigh && info.nFileSizeLow==b.info.nFileSizeLow &&
          CompareFileTime(&info.ftLastWriteTime,&b.info.ftLastWriteTime)==0;
    }
};
inline void checkOptions(const WzOptions& o) {
    if(o.delaySeconds<1||o.delaySeconds>3600||o.idleMinutes<0||o.idleMinutes>1440||
       (o.level!=0&&o.level!=1&&o.level!=3&&o.level!=5&&o.level!=7&&o.level!=9)||
       o.method<0||o.method>4||o.threads<0||o.threads>256) throw std::runtime_error("Invalid compression or cache settings");
}
struct Session {
    fs::path original, cache, root;
    Fingerprint fingerprint;
    WzOptions options;
    Format format;
    Ptfs ptfs;
    Fsp::FileSystemHost host;
    std::wstring drive,status=L"Ready";
    bool mounted=false;
    Session(const fs::path& p,const WzOptions& o):original(fs::canonical(p)),fingerprint(original),options(o),host(ptfs) {
        checkOptions(o);
        cache=local()/guid(); root=cache/L"files";
        fs::create_directories(root);
        try {
            std::ofstream manifest(cache/L"archive.txt",std::ios::binary);
            manifest<<path8(original)<<"\n"; manifest.close();
            format=extract(lib(),original,root);
            if(!(fingerprint==Fingerprint(original))) throw std::runtime_error("Original archive changed while opening");
            ptfs.readOnly=!format.inner;
            ptfs.label=original.filename().wstring();
            auto s=root.wstring();
            if(!NT_SUCCESS(ptfs.SetPath(s.data()))) throw std::runtime_error("Cannot open cache directory");
            DWORD used=GetLogicalDrives();
            for(wchar_t letter=L'Z';letter>=L'D';--letter) {
                if(used & (1u<<(letter-L'A'))) continue;
                wchar_t d[]={letter,L':',0};
                if(NT_SUCCESS(host.Mount(d))) {drive=host.MountPoint();mounted=true;break;}
            }
            if(!mounted) throw std::runtime_error("WinFsp mount failed. Check driver and available drive letters.");
            ptfs.Touch();
            if(ptfs.readOnly) status=L"Read-only";
        } catch(...) {
            if(mounted) host.Unmount();
            std::error_code ec;fs::remove_all(cache,ec);throw;
        }
    }
    ~Session() {
        if(mounted) host.Unmount();
        if(!ptfs.dirty) { std::error_code ec;fs::remove_all(cache,ec); }
    }
    void commitFrozen() {
        if(!ptfs.dirty) return;
        if(ptfs.readOnly) throw std::runtime_error("Read-only archive");
        if(!(fingerprint==Fingerprint(original))) throw std::runtime_error("Original archive changed; cache retained for recovery");
        auto tmp=original.parent_path()/(L".winzipfs-"+guid()+L".tmp");
        auto work=cache/L"repack.tar";
        try {
            std::error_code ec;fs::remove(work,ec);
            pack(lib(),root,tmp,work,format,options);
            HANDLE out=CreateFileW(tmp.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(out==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush new archive");
            BOOL flushed=FlushFileBuffers(out);CloseHandle(out);
            if(!flushed) throw std::runtime_error("Cannot flush new archive");
            if(!(fingerprint==Fingerprint(original))) throw std::runtime_error("Original archive changed during compression");
            if(!ReplaceFileW(original.c_str(),tmp.c_str(),nullptr,0,nullptr,nullptr))
                throw std::runtime_error("Cannot replace original archive; cache retained");
            fingerprint=Fingerprint(original);
            ptfs.dirty=false;
            fs::remove(work,ec);
            status=L"Saved";
        } catch(...) {
            std::error_code ec;fs::remove(tmp,ec);fs::remove(work,ec);throw;
        }
    }
    bool idle(unsigned long long now,unsigned long long delay) const {
        return ptfs.handles==0 && now-static_cast<unsigned long long>(ptfs.lastActivity.load())>=delay;
    }
    void freeze() {
        std::lock_guard lock(ptfs.gate);
        if(ptfs.handles!=0) throw std::runtime_error("Files are still open; close them before saving or ejecting");
        ptfs.closing=true;
    }
    void save(bool eject) {
        freeze();
        try {
            commitFrozen();
            if(eject) {host.Unmount();mounted=false;}
            else {ptfs.Touch();ptfs.closing=false;}
        } catch(...) {ptfs.closing=false;ptfs.Touch();throw;}
    }
};
std::vector<std::unique_ptr<Session>> sessions;
Session& find(const wchar_t* d) {
    for(auto& s:sessions) if(same(s->drive,d)) return *s;
    throw std::runtime_error("Mounted archive not found");
}
template<class F> int call(F&& f) {
    std::lock_guard lock(manager);
    error.clear();
    try {f();return 1;}
    catch(const std::exception& e) {error=wide(e.what());return 0;}
    catch(...) {error=L"Unexpected filesystem error";return 0;}
}
inline void copy(wchar_t* out,unsigned size,std::wstring_view value) {
    if(out&&size) {auto n=(std::min)(size_t(size-1),value.size());std::copy_n(value.data(),n,out);out[n]=0;}
}
}
extern "C" {
void wz_default_options(WzOptions* o) {if(o)*o={0,5,0,5,0,0,1};}
const wchar_t* wz_extensions() {return L".zip;.zipx;.7z;.rar;.tar;.gz;.gzip;.tgz;.bz2;.bzip2;.tbz;.tbz2;.xz;.txz;.zst;.tzst;.cab;.arj;.lha;.lzh;.z;.cpio;.deb;.rpm;.iso;.wim;.swm;.esd;.dmg;.xar;.ar;.001;.chm;.lzma;.udf;.squashfs";}
int wz_mount(const wchar_t* path,const WzOptions* o,wchar_t* mount,unsigned size) {
    return wz::call([&]{
        if(!path||!o) throw std::runtime_error("Missing archive path or options");
        auto p=wz::fs::canonical(path);
        for(const auto& s:wz::sessions) {
            if(wz::same(s->original.wstring(),p.wstring())) {wz::copy(mount,size,s->drive);return;}
            if(wz::Fingerprint(p)==s->fingerprint) {wz::copy(mount,size,s->drive);return;}
        }
        if(!NT_SUCCESS(FspLoad(nullptr))) throw std::runtime_error("WinFsp is not installed");
        auto s=std::make_unique<wz::Session>(p,*o);
        wz::copy(mount,size,s->drive);wz::sessions.push_back(std::move(s));
    });
}
int wz_count() {std::lock_guard lock(wz::manager);return (int)wz::sessions.size();}
int wz_get(int i,WzVolume* out) {
    return wz::call([&]{
        if(!out||i<0||i>=int(wz::sessions.size())) throw std::runtime_error("Invalid volume index");
        auto& s=*wz::sessions[i]; *out={};
        wz::copy(out->archive,32768,s.original.wstring());wz::copy(out->mount,16,s.drive);
        wz::copy(out->status,512,s.status);
        out->readOnly=s.ptfs.readOnly;out->dirty=s.ptfs.dirty;out->handles=s.ptfs.handles;out->options=s.options;
    });
}
int wz_options(const wchar_t* d,const WzOptions* o) {
    return wz::call([&]{
        if(!o) throw std::runtime_error("Missing options");
        wz::checkOptions(*o);auto& s=wz::find(d);
        if(o->method==3 && s.format.inner!=&bit7z::BitFormat::SevenZip)
            throw std::runtime_error("LZMA2 is available for 7z only");
        if(o->method==2 && s.format.inner!=&bit7z::BitFormat::Zip)
            throw std::runtime_error("Deflate is available for ZIP only");
        if(o->method==4 && s.format.inner!=&bit7z::BitFormat::Zip && s.format.inner!=&bit7z::BitFormat::SevenZip)
            throw std::runtime_error("BZip2 is available for ZIP and 7z only");
        s.options=*o;
    });
}
int wz_flush(const wchar_t* d) {return wz::call([&]{wz::find(d).save(false);});}
int wz_eject(const wchar_t* d) {return wz::call([&]{
    auto& s=wz::find(d);s.save(true);
    std::erase_if(wz::sessions,[&](const auto& p){return p.get()==&s;});
});}
int wz_tick() {return wz::call([&]{
    auto now=GetTickCount64();
    for(auto i=wz::sessions.begin();i!=wz::sessions.end();) {
        auto& s=**i;
        try {
            if(s.options.idleMinutes && s.idle(now,60000ULL*s.options.idleMinutes) &&
                !(s.options.batch && s.ptfs.dirty)) {s.save(true);i=wz::sessions.erase(i);continue;}
            if(!s.options.batch&&s.ptfs.dirty&&s.idle(now,1000ULL*s.options.delaySeconds)) s.save(false);
        } catch(const std::exception& e) {s.status=wz::wide(e.what());}
        ++i;
    }
});}
int wz_shutdown() {return wz::call([&]{
    while(!wz::sessions.empty()) {wz::sessions.back()->save(true);wz::sessions.pop_back();}
});}
void wz_error(wchar_t* out,unsigned size) {wz::copy(out,size,wz::error);}
}
