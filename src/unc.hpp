#pragma once
// Included within namespace wz after Session and path helpers.

inline void copyExact(wchar_t* out,unsigned size,std::wstring_view value) {
    if(!out || size<=value.size()) throw std::runtime_error("UNC path buffer is too small");
    copy(out,size,value);
}
inline bool prefixPath(std::wstring_view path,std::wstring_view prefix) {
    return path.size()>=prefix.size() &&
        CompareStringOrdinal(path.data(),static_cast<int>(prefix.size()),prefix.data(),static_cast<int>(prefix.size()),TRUE)==CSTR_EQUAL &&
        (path.size()==prefix.size()||path[prefix.size()]==L'\\');
}
inline std::vector<BYTE> tokenInfo(HANDLE token,TOKEN_INFORMATION_CLASS kind) {
    DWORD size=0;
    GetTokenInformation(token,kind,nullptr,0,&size);
    if(!size) throw std::runtime_error("Cannot identify UNC caller");
    std::vector<BYTE> data(size);
    if(!GetTokenInformation(token,kind,data.data(),size,&size)) throw std::runtime_error("Cannot identify UNC caller");
    return data;
}
inline DWORD tokenSession(HANDLE token) {
    DWORD session=0,size=0;
    if(!GetTokenInformation(token,TokenSessionId,&session,sizeof(session),&size))
        throw std::runtime_error("Cannot identify Windows session");
    return session;
}
inline DWORD integrity(HANDLE token) {
    auto data=tokenInfo(token,TokenIntegrityLevel);
    auto sid=reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data())->Label.Sid;
    return *GetSidSubAuthority(sid,*GetSidSubAuthorityCount(sid)-1);
}
inline bool localDrive(wchar_t letter) {
    wchar_t root[]={letter,L':',L'\\',0};
    auto type=GetDriveTypeW(root);
    if(type!=DRIVE_FIXED&&type!=DRIVE_REMOVABLE&&type!=DRIVE_CDROM) return false;
    for(auto const& session:sessions)
        if(!session->uncMode&&!session->drive.empty()&&towupper(session->drive[0])==letter) return false;
    return true;
}
inline bool archiveSuffix(const fs::path& path) {
    auto ext=lower(path.extension().wstring());
    std::wstring_view extensions=wz_extensions();
    for(size_t i=0;i<extensions.size();) {
        size_t j=extensions.find(L';',i);if(j==std::wstring_view::npos)j=extensions.size();
        if(ext==extensions.substr(i,j-i))return true;
        i=j+1;
    }
    return false;
}
// Local physical drive paths only. Reject reparse components before traversing them,
// so a request cannot recurse into the network namespace or into another archive.
inline fs::path uncSourcePath(const fs::path& input) {
    auto path=fs::absolute(input).lexically_normal();
    auto root=path.root_name().wstring();
    if(root.size()!=2||root[1]!=L':'||!localDrive(towupper(root[0]))||!path.is_absolute())
        throw std::runtime_error("UNC archive paths require a local physical drive");
    independentPath(path);
    fs::path current=path.root_path();
    for(const auto& component:path.relative_path()) {
        safeName(component.wstring(),false);
        current/=component;
        DWORD a=GetFileAttributesW(current.c_str());
        if(a==INVALID_FILE_ATTRIBUTES||(a&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("UNC source does not exist or contains a reparse point");
    }
    auto attrs=GetFileAttributesW(path.c_str());
    if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_DIRECTORY)||!archiveSuffix(path))
        throw std::runtime_error("Choose a supported archive file");
    return fs::canonical(path);
}
class UncFs final : public Fsp::FileSystemBase {
    Ptfs shadow;
    Fsp::FileSystemHost host;
    fs::path shadowPath;
    std::vector<BYTE> user;
    DWORD session=0,level=0;
    bool started=false;
    struct Route {Ptfs* fs=nullptr;std::wstring relative;};
    static Ptfs* owner(PVOID desc) {return static_cast<PtfsFileDesc*>(desc)->owner;}
    bool allowed() const noexcept {
        try {
            auto context=FspFileSystemGetOperationContext();
            if(!context||!context->Request)return false;
            auto request=context->Request;
            UINT64 tokenValue=0;
            if(request->Kind==FspFsctlTransactCreateKind) tokenValue=request->Req.Create.AccessToken;
            else if(request->Kind==FspFsctlTransactSetInformationKind &&
                    (request->Req.SetInformation.FileInformationClass==10||request->Req.SetInformation.FileInformationClass==65))
                tokenValue=request->Req.SetInformation.Info.Rename.AccessToken;
            if(!tokenValue)return false;
            HANDLE token=FSP_FSCTL_TRANSACT_REQ_TOKEN_HANDLE(tokenValue);
            auto other=tokenInfo(token,TokenUser);
            return EqualSid(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid,
                            reinterpret_cast<const TOKEN_USER*>(other.data())->User.Sid) &&
                   tokenSession(token)==session && integrity(token)>=level;
        }catch(...){return false;}
    }
    Route route(PWSTR raw) {
        if(uncStopping) throw std::runtime_error("UNC namespace is stopping");
        std::wstring name(raw?raw:L"");
        if(name==L"\\") return {&shadow,L"\\"};
        if(name.empty()||name.front()!=L'\\')throw std::runtime_error("Invalid UNC path");
        safeName(name.substr(1),false);
        // Reuse the same archive cache even if the source has changed externally.
        for(auto const& item:sessions) if(item->uncMode) {
            auto relative=item->drive.substr(address.size());
            if(prefixPath(name,relative)) {
                item->ptfs.Touch();
                return {&item->ptfs,name.size()==relative.size()?L"\\":name.substr(relative.size())};
            }
        }
        if(name.size()<2||(name.size()>2&&name[2]!=L'\\'))throw std::runtime_error("Invalid UNC drive component");
        wchar_t letter=towupper(name[1]);
        if(letter<L'A'||letter>L'Z'||!localDrive(letter)) throw std::runtime_error("Invalid local drive");
        fs::path source(std::wstring{letter,L':',L'\\'});
        if(name.size()==2) {
            fs::create_directories(shadowPath/name.substr(1));
            return {&shadow,name};
        }
        for(size_t pos=3;pos<name.size();) {
            size_t end=name.find(L'\\',pos);if(end==std::wstring::npos)end=name.size();
            source/=name.substr(pos,end-pos);
            independentPath(source);
            DWORD attrs=GetFileAttributesW(source.c_str());
            if(attrs==INVALID_FILE_ATTRIBUTES||(attrs&FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("UNC source not found");
            if(!(attrs&FILE_ATTRIBUTE_DIRECTORY)) {
                if(!archiveSuffix(source)) throw std::runtime_error("The UNC source is not an archive");
                auto canonical=uncSourcePath(source);
                auto rootName=name.substr(0,end);
                auto& opened=openSession(canonical,address+rootName,uncDefaults,false,nullptr);
                return {&opened.ptfs,end==name.size()?L"\\":name.substr(end)};
            }
            pos=end+1;
        }
        // The namespace is not a general-purpose share of local disks. Only
        // virtual directory placeholders are exposed, never ordinary local files.
        fs::create_directories(shadowPath/name.substr(1));
        return {&shadow,name};
    }
    template<class Fn> NTSTATUS named(PWSTR name,Fn&& callback) {
        if(!allowed())return STATUS_ACCESS_DENIED;
        std::lock_guard lock(manager);
        try {auto r=route(name);return callback(r);}
        catch(const PasswordRequired&) {return STATUS_ACCESS_DENIED;}
        catch(...) {return STATUS_OBJECT_PATH_NOT_FOUND;}
    }
    NTSTATUS Init(PVOID value) override {
        auto result=shadow.Init(value);
        auto h=static_cast<Fsp::FileSystemHost*>(value);
        h->SetReadOnlyVolume(FALSE);
        h->SetFileInfoTimeout(0);
        h->SetPrefix(address.data()+1);
        return result;
    }
    NTSTATUS GetVolumeInfo(VolumeInfo* info) override {
        auto result=shadow.GetVolumeInfo(info);
        if(NT_SUCCESS(result)) {
            ULARGE_INTEGER freeBytes{};
            if(GetDiskFreeSpaceExW(shadowPath.c_str(),&freeBytes,nullptr,nullptr))info->FreeSize=freeBytes.QuadPart;
        }
        return result;
    }
    NTSTATUS GetSecurityByName(PWSTR name,PUINT32 attr,PSECURITY_DESCRIPTOR sd,SIZE_T* size) override {
        return named(name,[&](Route& r){return r.fs->GetSecurityByName(r.relative.data(),attr,sd,size);});
    }
    NTSTATUS Open(PWSTR name,UINT32 flags,UINT32 access,PVOID* node,PVOID* desc,OpenFileInfo* info) override {
        return named(name,[&](Route& r){
            auto result=r.fs->Open(r.relative.data(),flags,access,node,desc,info);
            if(NT_SUCCESS(result))static_cast<PtfsFileDesc*>(*desc)->owner=r.fs;
            return result;
        });
    }
    NTSTATUS Create(PWSTR name,UINT32 flags,UINT32 access,UINT32 attrs,PSECURITY_DESCRIPTOR sd,UINT64 size,
                    PVOID* node,PVOID* desc,OpenFileInfo* info) override {
        return named(name,[&](Route& r){
            if(r.fs==&shadow||r.relative==L"\\")return STATUS_MEDIA_WRITE_PROTECTED;
            auto result=r.fs->Create(r.relative.data(),flags,access,attrs,sd,size,node,desc,info);
            if(NT_SUCCESS(result))static_cast<PtfsFileDesc*>(*desc)->owner=r.fs;
            return result;
        });
    }
    NTSTATUS Overwrite(PVOID n,PVOID d,UINT32 a,BOOLEAN replace,UINT64 size,FileInfo* i) override {
        return owner(d)->Overwrite(n,d,a,replace,size,i);
    }
    VOID Cleanup(PVOID n,PVOID d,PWSTR name,ULONG flags) override {owner(d)->Cleanup(n,d,name,flags);}
    VOID Close(PVOID n,PVOID d) override {owner(d)->Close(n,d);}
    NTSTATUS Read(PVOID n,PVOID d,PVOID b,UINT64 o,ULONG l,PULONG done) override {return owner(d)->Read(n,d,b,o,l,done);}
    NTSTATUS Write(PVOID n,PVOID d,PVOID b,UINT64 o,ULONG l,BOOLEAN end,BOOLEAN constrained,PULONG done,FileInfo* i) override {
        return owner(d)->Write(n,d,b,o,l,end,constrained,done,i);
    }
    NTSTATUS Flush(PVOID n,PVOID d,FileInfo* i) override {return d?owner(d)->Flush(n,d,i):STATUS_SUCCESS;}
    NTSTATUS GetFileInfo(PVOID n,PVOID d,FileInfo* i) override {return owner(d)->GetFileInfo(n,d,i);}
    NTSTATUS SetBasicInfo(PVOID n,PVOID d,UINT32 a,UINT64 c,UINT64 r,UINT64 w,UINT64 x,FileInfo* i) override {
        return owner(d)->SetBasicInfo(n,d,a,c,r,w,x,i);
    }
    NTSTATUS SetFileSize(PVOID n,PVOID d,UINT64 size,BOOLEAN allocation,FileInfo* i) override {
        return owner(d)->SetFileSize(n,d,size,allocation,i);
    }
    NTSTATUS SetDelete(PVOID n,PVOID d,PWSTR name,BOOLEAN remove) override {
        auto p=owner(d);
        if(p==&shadow)return STATUS_MEDIA_WRITE_PROTECTED;
        std::lock_guard lock(manager);
        for(auto const& item:sessions) if(&item->ptfs==p&&item->uncMode) {
            auto base=item->drive.substr(address.size());
            if(!name||!prefixPath(name,base))return STATUS_ACCESS_DENIED;
            auto relative=std::wstring_view(name).substr(base.size());
            std::wstring rel=relative.empty()?L"\\":std::wstring(relative);
            return p->SetDelete(n,d,rel.data(),remove);
        }
        return STATUS_ACCESS_DENIED;
    }
    NTSTATUS Rename(PVOID n,PVOID d,PWSTR from,PWSTR to,BOOLEAN replace) override {
        if(!allowed())return STATUS_ACCESS_DENIED;
        auto p=owner(d);
        if(p==&shadow)return STATUS_MEDIA_WRITE_PROTECTED;
        std::lock_guard lock(manager);
        for(auto const& item:sessions) if(&item->ptfs==p&&item->uncMode) {
            auto base=item->drive.substr(address.size());
            if(!prefixPath(from,base)||!prefixPath(to,base)) return STATUS_NOT_SAME_DEVICE;
            std::wstring a=std::wstring(from).substr(base.size()),b=std::wstring(to).substr(base.size());
            if(a.empty()||b.empty())return STATUS_ACCESS_DENIED;
            return p->Rename(n,d,a.data(),b.data(),replace);
        }
        return STATUS_ACCESS_DENIED;
    }
    NTSTATUS GetSecurity(PVOID n,PVOID d,PSECURITY_DESCRIPTOR sd,SIZE_T* size) override {return owner(d)->GetSecurity(n,d,sd,size);}
    NTSTATUS SetSecurity(PVOID n,PVOID d,SECURITY_INFORMATION i,PSECURITY_DESCRIPTOR sd) override {return owner(d)->SetSecurity(n,d,i,sd);}
    NTSTATUS ReadDirectory(PVOID n,PVOID d,PWSTR p,PWSTR m,PVOID b,ULONG l,PULONG done) override {
        return owner(d)->ReadDirectory(n,d,p,m,b,l,done);
    }
public:
    std::wstring address;
    UncFs():host(*this) {
        HANDLE raw=nullptr;
        if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&raw))throw std::runtime_error("Cannot identify Windows user");
        FileHandle token(raw);
        user=tokenInfo(raw,TokenUser);session=tokenSession(raw);level=integrity(raw);
        PWSTR sid=nullptr;
        if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid,&sid))
            throw std::runtime_error("Cannot construct UNC namespace name");
        address=L"\\\\WinZipFS\\Archives-"+std::wstring(sid)+L"-"+std::to_wstring(session);
        LocalFree(sid);
        shadowPath=local()/(L"namespace-"+guid());
        fs::create_directories(shadowPath);
        try {
            shadow.readOnly=true;shadow.label=L"WinZipFS archives";
            auto path=shadowPath.wstring();
            if(!NT_SUCCESS(shadow.SetPath(path.data())))throw std::runtime_error("Cannot initialize UNC namespace");
            wchar_t noMount[]={0};
            NTSTATUS status=host.Mount(noMount);
            if(!NT_SUCCESS(status))throw std::runtime_error("Cannot register UNC namespace (NTSTATUS "+std::to_string(static_cast<unsigned>(status))+")");
            started=true;
        }catch(...){removeCache(shadowPath);throw;}
    }
    ~UncFs() {if(started)host.Unmount();removeCache(shadowPath);}
    std::wstring addressFor(fs::path const& source) const {
        auto path=source.wstring();
        return address+L"\\"+std::wstring(1,towupper(path[0]))+path.substr(2);
    }
    Session& openSession(fs::path const& source,std::wstring const& requested,WzOptions const& options,bool readOnly,const wchar_t* password) {
        if(uncStopping)throw std::runtime_error("UNC namespace is stopping");
        Fingerprint id(source);
        for(auto const& item:sessions) if(same(item->original.wstring(),source.wstring())||id==item->fingerprint) {
            if(!item->uncMode)throw std::runtime_error("Archive is mounted as a drive letter");
            if(!same(item->drive,requested))throw std::runtime_error("Archive already open through another UNC path");
            if(readOnly&&!item->ptfs.readOnly)throw std::runtime_error("Archive is already open read/write");
            item->ptfs.Touch();return *item;
        }
        auto opened=std::make_unique<Session>(source,options,readOnly,password,requested);
        // Keep only directory names in the virtual namespace, never archive data.
        fs::create_directories(shadowPath/requested.substr(address.size()+1));
        auto& result=*opened;sessions.push_back(std::move(opened));return result;
    }
};
std::unique_ptr<UncFs> unc;
