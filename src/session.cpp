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
thread_local bool passwordRequired=false;
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
struct FileHandle {
    HANDLE value=INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE handle):value(handle) {}
    FileHandle(const FileHandle&)=delete;
    FileHandle& operator=(const FileHandle&)=delete;
    ~FileHandle() {if(value!=INVALID_HANDLE_VALUE) CloseHandle(value);}
};
struct Secret {
    std::string value;
    explicit Secret(const wchar_t* password):value(password?utf8(password):std::string{}) {}
    ~Secret() {if(!value.empty()) SecureZeroMemory(value.data(),value.size());}
};
inline bool ordinaryDirectory(const fs::path& path) {
    auto attr=GetFileAttributesW(path.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES && (attr&FILE_ATTRIBUTE_DIRECTORY) && !(attr&FILE_ATTRIBUTE_REPARSE_POINT);
}
inline void marker(const fs::path& path) {
    std::ofstream out(path,std::ios::binary|std::ios::trunc);
    out<<"1";
    out.close();
    if(!out) throw std::runtime_error("Cannot write session recovery metadata");
}
inline std::unique_ptr<FileHandle> lockCache(const fs::path& cache) {
    if(!ordinaryDirectory(cache)) return {};
    auto lock=std::make_unique<FileHandle>(CreateFileW((cache/L"session.lock").c_str(),
        GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(lock->value==INVALID_HANDLE_VALUE) return {};
    BY_HANDLE_FILE_INFORMATION info{};
    if(!GetFileInformationByHandle(lock->value,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)) return {};
    return lock;
}
struct Fingerprint {
    BY_HANDLE_FILE_INFORMATION info{};
    explicit Fingerprint(HANDLE h) {
        if(h==INVALID_HANDLE_VALUE || !GetFileInformationByHandle(h,&info))
            throw std::runtime_error("Cannot identify original archive");
    }
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
inline void exportTree(const fs::path& root,const fs::path& destination,WzOptions options) {
    auto target=fs::absolute(destination);
    auto ext=lower(target.extension().wstring());
    Format copyFormat{ext==L".zip"?&BitFormat::Zip:ext==L".7z"?&BitFormat::SevenZip:nullptr};
    if(!copyFormat.inner) throw std::runtime_error("Choose a .zip or .7z destination");
    if(fs::exists(target)) throw std::runtime_error("The destination already exists");
    if(!ordinaryDirectory(target.parent_path())) throw std::runtime_error("The destination directory does not exist");
    auto temp=target.parent_path()/(L".winzipfs-"+guid()+L".tmp"+target.extension().wstring());
    TemporaryFile cleanup(temp);
    options.method=0;
    pack(lib(),root,temp,root.parent_path()/L"copy.tar",copyFormat,options);
    {
        FileHandle out(CreateFileW(temp.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(out.value==INVALID_HANDLE_VALUE||!FlushFileBuffers(out.value))
            throw std::runtime_error("Cannot flush the exported archive");
    }
    if(!MoveFileExW(temp.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot create the requested archive copy");
}
struct Session {
    fs::path original, cache, root;
    Fingerprint fingerprint;
    WzOptions options;
    Format format;
    Ptfs ptfs;
    Fsp::FileSystemHost host;
    std::wstring drive,status=L"Ready";
    bool mounted=false,protectedArchive=false;
    std::unique_ptr<FileHandle> lease;
    unsigned long long retryAfter=0;
    std::wstring notifiedError;
    Session(const fs::path& p,const WzOptions& o,bool forceReadOnly,const wchar_t* password):original(fs::canonical(p)),fingerprint(original),options(o),host(ptfs) {
        checkOptions(o);
        cache=local()/guid(); root=cache/L"files";
        fs::create_directories(root);
        try {
            lease=lockCache(cache);
            if(!lease) throw std::runtime_error("Cannot lock the archive cache");
            Secret secret(password);
            protectedArchive=!secret.value.empty();
            if(protectedArchive) marker(cache/L"sensitive");
            std::ofstream manifest(cache/L"archive.txt",std::ios::binary);
            manifest<<path8(original)<<"\n"; manifest.close();
            if(!manifest) throw std::runtime_error("Cannot write session metadata");
            format=extract(lib(),original,root,secret.value);
            if(!(fingerprint==Fingerprint(original))) throw std::runtime_error("Original archive changed while opening");
            if(forceReadOnly) format={};
            ptfs.readOnly=!format.inner;
            ptfs.label=original.filename().wstring();
            marker(cache/L"ready");
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
            if(ptfs.readOnly) status=protectedArchive?L"Read-only · protected":L"Read-only";
        } catch(...) {
            if(mounted) host.Unmount();
            lease.reset();
            std::error_code ec;fs::remove_all(cache,ec);throw;
        }
    }
    ~Session() {
        if(mounted) host.Unmount();
        lease.reset();
        if(!ptfs.dirty) { std::error_code ec;fs::remove_all(cache,ec); }
    }
    void commitFrozen() {
        if(!ptfs.dirty) return;
        if(ptfs.readOnly) throw std::runtime_error("Read-only archive");
        FileHandle source(CreateFileW(original.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                                     nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(source.value==INVALID_HANDLE_VALUE)
            throw std::runtime_error("The original archive is open for writing or unavailable. Close the other application and retry.");
        if(!(fingerprint==Fingerprint(source.value)) || !(fingerprint==Fingerprint(original)))
            throw std::runtime_error("Original archive changed; cache retained for recovery");
        auto tmp=original.parent_path()/(L".winzipfs-"+guid()+L".tmp"+original.extension().wstring());
        auto backup=original.parent_path()/(L".winzipfs-"+guid()+L".bak");
        auto work=cache/L"repack.tar";
        bool replacing=false;
        try {
            std::error_code ec;fs::remove(work,ec);
            pack(lib(),root,tmp,work,format,options);
            HANDLE out=CreateFileW(tmp.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            if(out==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot flush new archive");
            BOOL flushed=FlushFileBuffers(out);CloseHandle(out);
            if(!flushed) throw std::runtime_error("Cannot flush new archive");
            if(!(fingerprint==Fingerprint(original))) throw std::runtime_error("Original archive changed during compression");
            replacing=true;
            if(!ReplaceFileW(original.c_str(),tmp.c_str(),backup.c_str(),0,nullptr,nullptr)) {
                DWORD code=GetLastError();
                if(GetFileAttributesW(original.c_str())==INVALID_FILE_ATTRIBUTES &&
                   GetFileAttributesW(backup.c_str())!=INVALID_FILE_ATTRIBUTES)
                    MoveFileExW(backup.c_str(),original.c_str(),MOVEFILE_WRITE_THROUGH);
                throw std::runtime_error("Cannot replace original archive (Windows error "+std::to_string(code)+"); cache retained; replacement: "+path8(tmp)+"; backup: "+path8(backup));
            }
            if(!(fingerprint==Fingerprint(backup))) {
                auto packed=original.parent_path()/(L".winzipfs-"+guid()+L".recovered"+original.extension().wstring());
                ReplaceFileW(original.c_str(),backup.c_str(),packed.c_str(),0,nullptr,nullptr);
                throw std::runtime_error("Original archive was replaced concurrently; cached files and replacement backups retained");
            }
            fingerprint=Fingerprint(original);
            ptfs.dirty=false;
            fs::remove(backup,ec);
            fs::remove(work,ec);
            status=L"Saved";
        } catch(...) {
            std::error_code ec;
            if(!replacing) fs::remove(tmp,ec);
            fs::remove(work,ec);throw;
        }
    }
    bool idle(unsigned long long now,unsigned long long delay) const {
        auto last=static_cast<unsigned long long>(ptfs.lastActivity.load());
        return ptfs.handles==0 && now>=last && now-last>=delay;
    }
    void freeze() {
        std::lock_guard lock(ptfs.gate);
        if(ptfs.handles!=0) throw std::runtime_error("Files are still open; close them before saving or ejecting");
        ptfs.closing=true;
    }
    void exportCopy(const fs::path& destination) {
        if(protectedArchive) throw std::runtime_error("A protected archive cannot be exported without encryption");
        freeze();
        try {
            exportTree(root,destination,options);
            ptfs.Touch();ptfs.closing=false;
        } catch(...) {ptfs.closing=false;ptfs.Touch();throw;}
    }
    void discard() {
        freeze();
        host.Unmount();mounted=false;ptfs.dirty=false;
    }
    void save(bool eject) {
        freeze();
        try {
            commitFrozen();
            if(eject) {host.Unmount();mounted=false;}
            else {ptfs.Touch();ptfs.closing=false;}
            retryAfter=0;notifiedError.clear();status=ptfs.readOnly?(protectedArchive?L"Read-only · protected":L"Read-only"):L"Saved";
        } catch(const std::exception& e) {status=wide(e.what());ptfs.closing=false;ptfs.Touch();throw;}
        catch(...) {ptfs.closing=false;ptfs.Touch();throw;}
    }
};
std::vector<std::unique_ptr<Session>> sessions;
Session& find(const wchar_t* d) {
    if(!d) throw std::runtime_error("Missing mount point");
    for(auto& s:sessions) if(same(s->drive,d)) return *s;
    throw std::runtime_error("Mounted archive not found");
}
template<class F> int call(F&& f) {
    std::lock_guard lock(manager);
    error.clear();passwordRequired=false;
    try {f();return 1;}
    catch(const PasswordRequired& e) {passwordRequired=true;error=wide(e.what());return 0;}
    catch(const std::exception& e) {error=wide(e.what());return 0;}
    catch(...) {error=L"Unexpected filesystem error";return 0;}
}
inline void copy(wchar_t* out,unsigned size,std::wstring_view value) {
    if(out&&size) {auto n=(std::min)(size_t(size-1),value.size());std::copy_n(value.data(),n,out);out[n]=0;}
}
inline bool inside(const fs::path& path,const fs::path& parent) {
    auto i=path.begin(),j=parent.begin();
    for(;j!=parent.end();++i,++j) {
        if(i==path.end()||!same(i->wstring(),j->wstring())) return false;
    }
    return true;
}
inline void independentPath(const fs::path& p) {
    for(const auto& session:sessions) {
        if(same(p.root_name().wstring(),fs::path(session->drive).root_name().wstring())||inside(p,session->cache))
            throw std::runtime_error("Choose a path outside mounted archive drives and their caches");
    }
    if(inside(p,local())) throw std::runtime_error("Choose a path outside the recovery cache");
}
inline void mountImpl(const wchar_t* path,const WzOptions* o,wchar_t* mount,unsigned size,bool ro,const wchar_t* password=nullptr) {
    if(!path||!o||!mount||size<3) throw std::runtime_error("Missing archive path, options or mount buffer");
    auto abs=fs::absolute(path);
    independentPath(abs);
    auto p=fs::canonical(abs);
    independentPath(p);
    auto id=Fingerprint(p);
    for(const auto& session:sessions) {
        if(same(session->original.wstring(),p.wstring())||id==session->fingerprint) {
            if(ro&&!session->ptfs.readOnly) throw std::runtime_error("The archive is already mounted read/write. Eject it before mounting read-only.");
            copy(mount,size,session->drive);return;
        }
    }
    if(!NT_SUCCESS(Fsp::Initialize())) throw std::runtime_error("WinFsp is not installed");
    auto session=std::make_unique<Session>(p,*o,ro,password);
    copy(mount,size,session->drive);sessions.push_back(std::move(session));
}
inline bool validId(std::wstring_view id) {
    if(id.size()!=38||id.front()!=L'{'||id.back()!=L'}') return false;
    for(size_t i=1;i<37;++i) {
        if(i==9||i==14||i==19||i==24) {if(id[i]!=L'-') return false;}
        else if(!iswxdigit(id[i])) return false;
    }
    return true;
}
inline bool ordinaryFile(const fs::path& file) {
    auto attr=GetFileAttributesW(file.c_str());
    return attr!=INVALID_FILE_ATTRIBUTES && !(attr&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT));
}
inline bool validRecovery(const fs::path& cache) {
    return validId(cache.filename().wstring())&&ordinaryDirectory(cache)&&
           ordinaryDirectory(cache/L"files")&&ordinaryFile(cache/L"ready")&&
           ordinaryFile(cache/L"archive.txt")&&!fs::exists(cache/L"sensitive");
}
inline std::vector<fs::path> recoveries() {
    std::vector<fs::path> result;
    auto directory=local();
    if(!ordinaryDirectory(directory)) return result;
    std::error_code ec;
    for(const auto& entry:fs::directory_iterator(directory,ec)) {
        auto path=entry.path();
        if(!validId(path.filename().wstring())||!ordinaryDirectory(path)) continue;
        auto lease=lockCache(path);
        if(!lease) continue;
        // Password-protected volumes are read-only. A terminated session cannot
        // have unsaved archive modifications, so remove its decrypted cache.
        if(ordinaryFile(path/L"sensitive")) {
            lease.reset();fs::remove_all(path,ec);ec.clear();continue;
        }
        if(validRecovery(path)) result.push_back(path);
    }
    std::sort(result.begin(),result.end());
    return result;
}
inline fs::path recoveryPath(const wchar_t* id) {
    if(!id||!validId(id)) throw std::runtime_error("Invalid recovery session");
    auto path=local()/id;
    if(!validRecovery(path)) throw std::runtime_error("Recovery session unavailable");
    return path;
}
}
extern "C" {
void wz_default_options(WzOptions* o) {if(o)*o={0,5,0,5,0,0,1};}
const wchar_t* wz_extensions() {return L".zip;.zipx;.7z;.rar;.tar;.gz;.gzip;.tgz;.bz2;.bzip2;.tbz;.tbz2;.xz;.txz;.zst;.tzst;.cab;.arj;.lha;.lzh;.z;.cpio;.deb;.rpm;.iso;.wim;.swm;.esd;.dmg;.xar;.ar;.001;.chm;.lzma;.udf;.squashfs";}
int wz_mount(const wchar_t* path,const WzOptions* o,wchar_t* mount,unsigned size) {
    return wz::call([&]{wz::mountImpl(path,o,mount,size,false);});
}
int wz_mount_password(const wchar_t* path,const WzOptions* o,const wchar_t* password,wchar_t* mount,unsigned size) {
    return wz::call([&]{
        if(!password||!*password) throw wz::PasswordRequired();
        wz::mountImpl(path,o,mount,size,true,password);
    });
}
int wz_password_required() {return wz::passwordRequired?1:0;}
int wz_recovery_count() {
    int count=0;
    if(!wz::call([&]{count=static_cast<int>(wz::recoveries().size());})) return -1;
    return count;
}
int wz_recovery_get(int index,WzRecovery* out) {
    return wz::call([&]{
        auto paths=wz::recoveries();
        if(!out||index<0||index>=int(paths.size())) throw std::runtime_error("Invalid recovery index");
        auto path=paths[index];auto lease=wz::lockCache(path);
        if(!lease) throw std::runtime_error("Recovery session is active");
        std::ifstream manifest(path/L"archive.txt",std::ios::binary);
        std::string source;
        std::getline(manifest,source);
        if(source.empty()||source.size()>131072) throw std::runtime_error("Invalid recovery metadata");
        *out={};
        wz::copy(out->id,64,path.filename().wstring());
        wz::copy(out->archive,32768,wz::wide(source));
        wz::copy(out->folder,32768,(path/L"files").wstring());
    });
}
int wz_recovery_export(const wchar_t* id,const wchar_t* destination) {
    return wz::call([&]{
        if(!destination||!*destination) throw std::runtime_error("Choose an export destination");
        auto path=wz::recoveryPath(id);auto lease=wz::lockCache(path);
        if(!lease) throw std::runtime_error("Recovery session is active");
        auto target=wz::fs::absolute(destination);auto parent=wz::fs::canonical(target.parent_path());
        wz::independentPath(target);wz::independentPath(parent);
        WzOptions o{};wz_default_options(&o);
        wz::exportTree(path/L"files",parent/target.filename(),o);
    });
}
int wz_recovery_discard(const wchar_t* id) {
    return wz::call([&]{
        auto path=wz::recoveryPath(id);auto lease=wz::lockCache(path);
        if(!lease) throw std::runtime_error("Recovery session is active");
        // Move the session outside the recovery namespace before deletion.
        lease.reset();
        auto removed=path.parent_path()/(L"discarded-"+wz::guid());
        if(!MoveFileExW(path.c_str(),removed.c_str(),MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("Cannot remove the recovery session");
        wz::fs::remove_all(removed);
    });
}
int wz_mount_readonly(const wchar_t* path,const WzOptions* o,wchar_t* mount,unsigned size) {
    return wz::call([&]{wz::mountImpl(path,o,mount,size,true);});
}
int wz_save_copy(const wchar_t* drive,const wchar_t* destination) {
    return wz::call([&]{
        if(!destination||!*destination) throw std::runtime_error("Choose an export destination");
        auto p=wz::fs::absolute(destination);
        auto parent=wz::fs::canonical(p.parent_path());
        wz::independentPath(p);wz::independentPath(parent);
        wz::find(drive).exportCopy(parent/p.filename());
    });
}
int wz_discard(const wchar_t* drive) {
    return wz::call([&]{
        auto& session=wz::find(drive);session.discard();
        std::erase_if(wz::sessions,[&](const auto& p){return p.get()==&session;});
    });
}
int wz_count() {std::lock_guard lock(wz::manager);return (int)wz::sessions.size();}
int wz_get(int i,WzVolume* out) {
    return wz::call([&]{
        if(!out||i<0||i>=int(wz::sessions.size())) throw std::runtime_error("Invalid volume index");
        auto& s=*wz::sessions[i]; *out={};
        wz::copy(out->archive,32768,s.original.wstring());wz::copy(out->mount,16,s.drive);
        wz::copy(out->status,512,s.status);
        out->readOnly=s.ptfs.readOnly;out->dirty=s.ptfs.dirty;out->protectedArchive=s.protectedArchive;out->handles=s.ptfs.handles;out->options=s.options;
        wz::copy(out->sessionId,64,s.cache.filename().wstring());
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
    std::wstring failures;
    for(auto i=wz::sessions.begin();i!=wz::sessions.end();) {
        auto& s=**i;
        try {
            if(now<s.retryAfter) {++i;continue;}
            if(s.options.idleMinutes && s.idle(now,60000ULL*s.options.idleMinutes)) {s.save(true);i=wz::sessions.erase(i);continue;}
            if(!s.options.batch&&s.ptfs.dirty&&s.idle(now,1000ULL*s.options.delaySeconds)) s.save(false);
        } catch(const std::exception& e) {
            s.status=wz::wide(e.what());s.retryAfter=GetTickCount64()+30000;
            if(s.notifiedError!=s.status) {
                s.notifiedError=s.status;
                if(!failures.empty()) failures+=L"; ";
                failures+=s.drive+L" "+s.status;
            }
        }
        ++i;
    }
    if(!failures.empty()) throw std::runtime_error(wz::utf8(failures));
});}
int wz_shutdown() {return wz::call([&]{
    while(!wz::sessions.empty()) {wz::sessions.back()->save(true);wz::sessions.pop_back();}
});}
void wz_error(wchar_t* out,unsigned size) {wz::copy(out,size,wz::error);}
}
