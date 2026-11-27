#pragma once
#include <winfsp/winfsp.hpp>
#include <sddl.h>
#include "memory_archive.hpp"
#include <7zip/PropID.h>

namespace wz {
inline std::atomic<uint64_t> nodeIds{1};
constexpr DWORD basicAttributes=FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|
    FILE_ATTRIBUTE_ARCHIVE|FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
constexpr DWORD unsupportedAttributes=FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_ENCRYPTED|
    FILE_ATTRIBUTE_SPARSE_FILE|FILE_ATTRIBUTE_COMPRESSED;
struct MemoryNode {
    std::wstring name;
    bool directory=false;
    uint64_t id=nodeIds.fetch_add(1),size=0,baseLimit=0,allocation=0;
    uint64_t created=nowTime(),accessed=created,modified=created,changed=created;
    DWORD attributes=0;
    std::optional<uint32_t> posixMode;
    uint32_t uid=0,gid=0;
    std::string user,group;
    bool hasAccessTime=false,hasCreationTime=false;
    std::optional<uint32_t> sourceIndex;
    bool dataDirty=false,propertiesDirty=false;
    std::map<uint64_t,PagePtr> pages;
};
using NodePtr=std::shared_ptr<MemoryNode>;
using NodeMap=std::map<std::wstring,NodePtr,CaseLess>;
inline std::wstring parentName(std::wstring_view name) {
    auto pos=name.rfind(L'\\');
    return pos==std::wstring_view::npos?L"":std::wstring(name.substr(0,pos));
}
inline std::wstring baseName(std::wstring_view name) {
    auto pos=name.rfind(L'\\');
    return std::wstring(pos==std::wstring_view::npos?name:name.substr(pos+1));
}
inline uint64_t alignedSize(uint64_t size) {
    return (size+4095)&~uint64_t(4095);
}
inline std::wstring volumeName(PWSTR name) {
    if(!name||*name!=L'\\')throw std::invalid_argument("Invalid filesystem path");
    if(!name[1])return {};
    return safeName(name+1,true);
}
inline std::vector<BYTE> volumeSecurity() {
    HANDLE token=nullptr;
    if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("Cannot identify volume owner");
    PWSTR sid=nullptr,group=nullptr;
    PSECURITY_DESCRIPTOR sd=nullptr;
    try {
        auto sidString=[&](TOKEN_INFORMATION_CLASS kind, PWSTR* result) {
            DWORD size=0;GetTokenInformation(token,kind,nullptr,0,&size);
            std::vector<BYTE> data(size);
            if(!size||!GetTokenInformation(token,kind,data.data(),size,&size))
                throw std::runtime_error("Cannot identify volume owner");
            auto value=kind==TokenUser?reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid:
                reinterpret_cast<TOKEN_PRIMARY_GROUP*>(data.data())->PrimaryGroup;
            if(!ConvertSidToStringSidW(value,result))throw std::runtime_error("Cannot construct volume security");
        };
        sidString(TokenUser,&sid);sidString(TokenPrimaryGroup,&group);
        std::wstring s=L"O:"+std::wstring(sid)+L"G:"+group+L"D:P(A;;FA;;;"+sid+L")(A;;FA;;;SY)(A;;FA;;;BA)";
        if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(s.c_str(),SDDL_REVISION_1,&sd,nullptr))
            throw std::runtime_error("Cannot construct volume security");
        auto length=GetSecurityDescriptorLength(sd);
        auto start=static_cast<const BYTE*>(sd);
        std::vector<BYTE> bytes(start,start+length);
        LocalFree(sd);LocalFree(sid);LocalFree(group);CloseHandle(token);
        return bytes;
    }catch(...){if(sd)LocalFree(sd);if(sid)LocalFree(sid);if(group)LocalFree(group);CloseHandle(token);throw;}
}
class MemoryFs;
struct MemoryFileDesc {
    NodePtr node;
    MemoryFs* owner=nullptr;
    PVOID DirBuffer=nullptr;
    explicit MemoryFileDesc(NodePtr n,MemoryFs* fs):node(std::move(n)),owner(fs){}
    ~MemoryFileDesc(){FspFileSystemDeleteDirectoryBuffer(&DirBuffer);}
};

class MemoryFs final : public Fsp::FileSystemBase {
    using Base=Fsp::FileSystemBase;
    NodeMap nodes;
    std::shared_ptr<ArchiveSource> source_;
    std::vector<BYTE> security=volumeSecurity();
    uint64_t creation=nowTime();
    static NodePtr directory(std::wstring name) {
        auto node=std::make_shared<MemoryNode>();
        node->directory=true;node->name=std::move(name);return node;
    }
    static void ensureParents(NodeMap& tree,std::wstring const& name) {
        auto parent=parentName(name);
        if(parent.empty())return;
        if(auto it=tree.find(parent);it!=tree.end()) {
            if(!it->second->directory||it->first!=parent)throw std::runtime_error("Conflicting archive directory names");
            return;
        }
        ensureParents(tree,parent);
        tree.emplace(parent,directory(parent));
    }
    static uint64_t propertyTime(SeekableReader const& reader,uint32_t index,BitProperty property,uint64_t fallback) {
        auto value=reader.itemProperty(index,property);
        return value.isFileTime()?timeValue(value.getFileTime()):fallback;
    }
    NodePtr find(std::wstring const& name) const {
        auto it=nodes.find(name);return it==nodes.end()?NodePtr{}:it->second;
    }
    NTSTATUS missing(std::wstring const& name) const {
        auto parent=find(parentName(name));
        return parent&&parent->directory?STATUS_OBJECT_NAME_NOT_FOUND:STATUS_OBJECT_PATH_NOT_FOUND;
    }
    static MemoryFileDesc& descriptor(PVOID p) {return *static_cast<MemoryFileDesc*>(p);}
    static bool canWrite(UINT32 access,UINT32 options) {
        return (access&(FILE_WRITE_DATA|FILE_APPEND_DATA|FILE_WRITE_ATTRIBUTES|FILE_WRITE_EA|DELETE|WRITE_DAC|WRITE_OWNER))||
            (options&FILE_DELETE_ON_CLOSE);
    }
    bool hasChildren(std::wstring const& name) const {
        if(name.empty())return nodes.size()>1;
        auto prefix=name+L"\\";
        auto it=nodes.lower_bound(prefix);
        return it!=nodes.end()&&underPath(it->first,name)&&it->first.size()>name.size();
    }
    DWORD attrs(MemoryNode const& node) const {
        DWORD value=node.attributes&basicAttributes;
        if(node.directory)value|=FILE_ATTRIBUTE_DIRECTORY;
        if(readOnly)value|=FILE_ATTRIBUTE_READONLY;
        return value?value:FILE_ATTRIBUTE_NORMAL;
    }
    void info(MemoryNode const& n,FileInfo* out) const {
        *out={};
        out->FileAttributes=attrs(n);
        out->FileSize=n.directory?0:n.size;
        out->AllocationSize=n.directory?0:std::max(n.allocation,alignedSize(n.size));
        out->CreationTime=n.created;out->LastAccessTime=n.accessed;
        out->LastWriteTime=n.modified;out->ChangeTime=n.changed;
        out->IndexNumber=n.id;out->HardLinks=0;
    }
    NTSTATUS securityInfo(PSECURITY_DESCRIPTOR sd,SIZE_T* size) const {
        if(!size)return STATUS_SUCCESS;
        if(*size<security.size()||!sd){*size=security.size();return STATUS_BUFFER_OVERFLOW;}
        memcpy(sd,security.data(),security.size());*size=security.size();return STATUS_SUCCESS;
    }
    void attach(NodePtr node,PVOID* pNode,PVOID* pDesc,OpenFileInfo* out) {
        auto desc=std::make_unique<MemoryFileDesc>(node,this);
        info(*node,&out->FileInfo);*pNode=node.get();*pDesc=desc.release();++handles;Touch();
    }
    static void setAttributes(MemoryNode& node,DWORD attributes) {
        bool wasReadOnly=(node.attributes&FILE_ATTRIBUTE_READONLY)!=0;
        bool isReadOnly=(attributes&FILE_ATTRIBUTE_READONLY)!=0;
        if(node.posixMode&&wasReadOnly!=isReadOnly) {
            if(isReadOnly)*node.posixMode&=~0222u;
            else *node.posixMode|=0200u;
        }
        node.attributes=attributes&basicAttributes;
    }
    void changed(MemoryNode& node,bool content) {
        node.changed=nowTime();
        if(content){node.modified=node.changed;node.dataDirty=true;node.attributes|=FILE_ATTRIBUTE_ARCHIVE;}
        node.propertiesDirty=true;Changed();
    }
    void resize(MemoryNode& n,uint64_t size) {
        if(size>uint64_t(INT64_MAX)-4096)throw std::runtime_error("File is too large");
        if(size==n.size)return;
        if(size<n.size) {
            n.baseLimit=std::min(n.baseLimit,size);
            auto first=(size+PageSize-1)/PageSize;
            n.pages.erase(n.pages.lower_bound(first),n.pages.end());
            if(size%PageSize)if(auto it=n.pages.find(size/PageSize);it!=n.pages.end())
                std::fill(it->second->data.begin()+static_cast<size_t>(size%PageSize),it->second->data.end(),0);
        }
        n.size=size;n.allocation=alignedSize(size);changed(n,true);
    }
    void dirRecord(MemoryNode const& node,std::wstring_view name,DirInfo* out) const {
        memset(out,0,FIELD_OFFSET(DirInfo,FileNameBuf)+name.size()*sizeof(wchar_t));
        out->Size=static_cast<UINT16>(FIELD_OFFSET(DirInfo,FileNameBuf)+name.size()*sizeof(wchar_t));
        info(node,&out->FileInfo);
        memcpy(out->FileNameBuf,name.data(),name.size()*sizeof(wchar_t));
    }
public:
    std::mutex gate;
    std::atomic<unsigned> handles{0};
    std::atomic<bool> dirty{false},closing{false};
    std::atomic<long long> lastActivity{0};
    bool readOnly=false;
    std::wstring label=L"WinZipFS";
    MemoryFs(){nodes.emplace(L"",directory(L""));}
    void Touch(){lastActivity=GetTickCount64();}
    void Changed(){dirty=true;Touch();}
    std::shared_ptr<ArchiveSource> source() const {return source_;}
    NodeMap const& entries() const {return nodes;}
    void load(std::shared_ptr<ArchiveSource> source,fs::path const& archive,bool standalone) {
        NodeMap next;next.emplace(L"",directory(L""));
        auto const& reader=source->reader();
        for(auto const& item:reader) {
            auto raw=item.rawPath();
            if(standalone)raw=source->standaloneName().empty()?archive.stem().wstring():source->standaloneName();
            auto name=safeName(raw,item.isDir());
            if(name.empty())continue;
            ensureParents(next,name);
            auto node=std::make_shared<MemoryNode>();
            node->name=name;node->directory=item.isDir();
            auto archiveAttributes=item.attributes();
            node->attributes=archiveAttributes&basicAttributes;
            auto posix=reader.itemProperty(item.index(),BitProperty::PosixAttrib);
            if(posix.isUInt32())node->posixMode=posix.getUInt32();
            else if(archiveAttributes&0x8000)node->posixMode=archiveAttributes>>16;
            if(node->posixMode&&!(node->posixMode.value()&0222)&&!node->directory)
                node->attributes|=FILE_ATTRIBUTE_READONLY;
            if(reader.detectedFormat()==BitFormat::Tar) {
                auto numeric=[&](BitProperty prop) {
                    auto v=reader.itemProperty(item.index(),prop);return v.isUInt32()?v.getUInt32():0u;
                };
                // Additional property identifiers follow the published 7-Zip SDK.
                node->uid=numeric(static_cast<BitProperty>(kpidUserId));
                node->gid=numeric(static_cast<BitProperty>(kpidGroupId));
                auto user=reader.itemProperty(item.index(),BitProperty::User);
                auto group=reader.itemProperty(item.index(),BitProperty::Group);
                if(user.isString())node->user=user.getString();
                if(group.isString())node->group=group.getString();
            }
            node->hasCreationTime=reader.itemProperty(item.index(),BitProperty::CTime).isFileTime();
            node->hasAccessTime=reader.itemProperty(item.index(),BitProperty::ATime).isFileTime();
            node->created=propertyTime(reader,item.index(),BitProperty::CTime,creation);
            node->modified=propertyTime(reader,item.index(),BitProperty::MTime,node->created);
            node->accessed=propertyTime(reader,item.index(),BitProperty::ATime,node->modified);
            node->changed=node->modified;
            node->sourceIndex=item.index();
            if(!node->directory) {
                node->size=standalone?source->itemSize(item.index()):item.size();
                if(node->size>uint64_t(INT64_MAX)-4096)throw std::runtime_error("Archive item exceeds Windows file size limit");
                node->baseLimit=node->size;node->allocation=alignedSize(node->size);
            }
            auto it=next.find(name);
            if(it!=next.end()&&(!it->second->directory||!node->directory||it->second->sourceIndex||it->first!=name))
                throw std::runtime_error("Conflicting archive entries");
            next[name]=std::move(node);
        }
        for(auto& [name,node]:next) {
            auto previous=nodes.find(name);
            if(previous!=nodes.end()&&previous->second->directory==node->directory)
                node->id=previous->second->id;
        }
        nodes.swap(next);source_=std::move(source);dirty=false;
    }
    // Directory-only namespace for UNC routing; it contains no archived data.
    void ensureDirectory(std::wstring name) {
        std::lock_guard lock(gate);
        if(name.empty())return;
        name=safeName(std::move(name),true);
        ensureParents(nodes,name);
        if(!nodes.contains(name))nodes.emplace(name,directory(name));
    }
    void releaseDirectory(std::wstring name) noexcept {
        try {
            std::lock_guard lock(gate);
            while(!name.empty()) {
                auto it=nodes.find(name);
                if(it==nodes.end()||it->second.use_count()>1||hasChildren(name))break;
                nodes.erase(it);name=parentName(name);
            }
        }catch(...){}
    }
    size_t readData(MemoryNode const& n,uint64_t offset,void* out,size_t count) const {
        if(n.directory||offset>=n.size)return 0;
        count=static_cast<size_t>(std::min<uint64_t>(count,n.size-offset));
        size_t done=0;
        while(done<count) {
            uint64_t pos=offset+done,index=pos/PageSize;
            auto within=static_cast<size_t>(pos%PageSize);
            auto take=std::min(count-done,PageSize-within);
            auto target=static_cast<unsigned char*>(out)+done;
            auto page=n.pages.find(index);
            if(page!=n.pages.end())memcpy(target,page->second->data.data()+within,take);
            else {
                size_t old=0;
                if(n.sourceIndex&&source_&&pos<n.baseLimit) {
                    old=static_cast<size_t>(std::min<uint64_t>(take,n.baseLimit-pos));
                    if(source_->read(*n.sourceIndex,pos,target,old)!=old)throw std::runtime_error("Short archive read");
                }
                if(old<take)memset(target+old,0,take-old);
            }
            done+=take;
        }
        return done;
    }
    NTSTATUS ExceptionHandler() override {
        try{throw;}
        catch(MemoryFull const&){return STATUS_DISK_FULL;}
        catch(std::bad_alloc const&){return STATUS_INSUFFICIENT_RESOURCES;}
        catch(std::invalid_argument const&){return STATUS_OBJECT_NAME_INVALID;}
        catch(BitException const&){return STATUS_DATA_ERROR;}
        catch(...){return STATUS_UNEXPECTED_IO_ERROR;}
    }
    NTSTATUS Init(PVOID host0) override {
        auto host=static_cast<Fsp::FileSystemHost*>(host0);
        host->SetSectorSize(4096);host->SetSectorsPerAllocationUnit(1);
        host->SetFileInfoTimeout(1000);host->SetCaseSensitiveSearch(FALSE);
        host->SetCasePreservedNames(TRUE);host->SetUnicodeOnDisk(TRUE);
        host->SetPersistentAcls(FALSE);host->SetReadOnlyVolume(readOnly);
        host->SetFileSystemName(const_cast<PWSTR>(L"WinZipFS"));
        host->SetPostCleanupWhenModifiedOnly(TRUE);
        host->SetPassQueryDirectoryPattern(FALSE);
        host->SetVolumeCreationTime(creation);
        host->SetVolumeSerialNumber(static_cast<UINT32>(creation^(creation>>32)^nodes.begin()->second->id));
        host->SetFlushAndPurgeOnCleanup(TRUE);
        return STATUS_SUCCESS;
    }
    NTSTATUS GetVolumeInfo(VolumeInfo* out) override {
        *out={};
        out->TotalSize=memoryPool.capacity();out->FreeSize=readOnly?0:memoryPool.writable();
        out->VolumeLabelLength=static_cast<UINT16>(std::min(label.size(),size_t(31))*sizeof(wchar_t));
        memcpy(out->VolumeLabel,label.data(),out->VolumeLabelLength);
        return STATUS_SUCCESS;
    }
    NTSTATUS GetSecurityByName(PWSTR raw,PUINT32 attributes,PSECURITY_DESCRIPTOR sd,SIZE_T* size) override {
        std::lock_guard lock(gate);
        if(closing)return STATUS_DEVICE_NOT_READY;
        auto name=volumeName(raw);auto node=find(name);
        if(!node)return missing(name);
        if(attributes)*attributes=attrs(*node);
        return securityInfo(sd,size);
    }
    NTSTATUS Open(PWSTR raw,UINT32 options,UINT32 access,PVOID* pNode,PVOID* pDesc,OpenFileInfo* out) override {
        std::lock_guard lock(gate);
        if(closing)return STATUS_DEVICE_NOT_READY;
        if(readOnly&&canWrite(access,options))return STATUS_MEDIA_WRITE_PROTECTED;
        auto name=volumeName(raw);auto node=find(name);
        if(!node)return missing(name);
        if((options&FILE_DIRECTORY_FILE)&&!node->directory)return STATUS_NOT_A_DIRECTORY;
        if((options&FILE_NON_DIRECTORY_FILE)&&node->directory)return STATUS_FILE_IS_A_DIRECTORY;
        if(!node->directory&&(node->attributes&FILE_ATTRIBUTE_READONLY)&&
           ((access&(FILE_WRITE_DATA|FILE_APPEND_DATA|DELETE))||(options&FILE_DELETE_ON_CLOSE)))
            return STATUS_ACCESS_DENIED;
        attach(node,pNode,pDesc,out);return STATUS_SUCCESS;
    }
    NTSTATUS Create(PWSTR raw,UINT32 options,UINT32 access,UINT32 attributes,PSECURITY_DESCRIPTOR,UINT64 allocation,
                    PVOID* pNode,PVOID* pDesc,OpenFileInfo* out) override {
        std::lock_guard lock(gate);
        if(closing)return STATUS_DEVICE_NOT_READY;
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        if(attributes&unsupportedAttributes)return STATUS_NOT_SUPPORTED;
        auto name=volumeName(raw);
        if(name.empty()||nodes.contains(name))return STATUS_OBJECT_NAME_COLLISION;
        auto parent=find(parentName(name));if(!parent||!parent->directory)return STATUS_OBJECT_PATH_NOT_FOUND;
        if(allocation>uint64_t(INT64_MAX)-4096)return STATUS_INVALID_PARAMETER;
        auto node=std::make_shared<MemoryNode>();
        node->name=parent->name.empty()?baseName(name):parent->name+L"\\"+baseName(name);
        node->directory=(options&FILE_DIRECTORY_FILE)!=0;
        node->attributes=attributes&basicAttributes;
        node->dataDirty=!node->directory;node->propertiesDirty=true;
        node->allocation=node->directory?0:alignedSize(allocation);
        auto it=nodes.emplace(node->name,node).first;
        try{attach(node,pNode,pDesc,out);}catch(...){nodes.erase(it);throw;}
        Changed();return STATUS_SUCCESS;
    }
    NTSTATUS Overwrite(PVOID,PVOID desc,UINT32 attributes,BOOLEAN replace,UINT64 allocation,FileInfo* out) override {
        std::lock_guard lock(gate);
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        auto& n=*descriptor(desc).node;
        if(n.directory)return STATUS_FILE_IS_A_DIRECTORY;
        if(attributes&unsupportedAttributes||allocation>uint64_t(INT64_MAX)-4096)return STATUS_INVALID_PARAMETER;
        if(n.attributes&FILE_ATTRIBUTE_READONLY)return STATUS_ACCESS_DENIED;
        resize(n,0);n.allocation=alignedSize(allocation);
        setAttributes(n,(replace?0:n.attributes)|(attributes&basicAttributes));
        changed(n,true);info(n,out);return STATUS_SUCCESS;
    }
    VOID Cleanup(PVOID,PVOID desc,PWSTR,ULONG flags) override {
        std::lock_guard lock(gate);
        if(readOnly)return;
        auto& n=*descriptor(desc).node;
        if((flags&CleanupDelete)&&!n.name.empty()&&(!n.directory||!hasChildren(n.name))) {
            auto it=nodes.find(n.name);
            if(it!=nodes.end()&&it->second.get()==&n){nodes.erase(it);Changed();}
        }
        if(flags&CleanupSetAllocationSize)n.allocation=alignedSize(n.size);
        if(flags&(CleanupSetArchiveBit|CleanupSetLastWriteTime|CleanupSetChangeTime)) {
            auto now=nowTime();
            if(flags&CleanupSetArchiveBit)n.attributes|=FILE_ATTRIBUTE_ARCHIVE;
            if(flags&CleanupSetLastWriteTime)n.modified=now;
            if(flags&CleanupSetChangeTime)n.changed=now;
            n.propertiesDirty=true;Changed();
        }
    }
    VOID Close(PVOID,PVOID desc) override {
        std::lock_guard lock(gate);
        delete static_cast<MemoryFileDesc*>(desc);Touch();--handles;
    }
    NTSTATUS Read(PVOID,PVOID desc,PVOID out,UINT64 offset,ULONG length,PULONG done) override {
        std::lock_guard lock(gate);
        *done=0;
        auto const& n=*descriptor(desc).node;
        if(n.directory)return STATUS_FILE_IS_A_DIRECTORY;
        if(offset>=n.size)return STATUS_END_OF_FILE;
        *done=static_cast<ULONG>(readData(n,offset,out,length));Touch();return STATUS_SUCCESS;
    }
    NTSTATUS Write(PVOID,PVOID desc,PVOID input,UINT64 offset,ULONG length,BOOLEAN append,BOOLEAN constrained,
                   PULONG done,FileInfo* out) override {
        std::lock_guard lock(gate);*done=0;
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        auto& n=*descriptor(desc).node;
        if(n.directory)return STATUS_FILE_IS_A_DIRECTORY;
        if(n.attributes&FILE_ATTRIBUTE_READONLY)return STATUS_ACCESS_DENIED;
        if(append&&!constrained)offset=n.size;
        if(constrained) {
            if(offset>=n.size){info(n,out);return STATUS_SUCCESS;}
            length=static_cast<ULONG>(std::min<uint64_t>(length,n.size-offset));
        }
        if(!length){info(n,out);return STATUS_SUCCESS;}
        if(offset>uint64_t(INT64_MAX)-4096-length)return STATUS_INVALID_PARAMETER;
        uint64_t end=offset+length;
        std::map<uint64_t,PagePtr> fresh;
        for(uint64_t index=offset/PageSize;index<=(end-1)/PageSize;++index) {
            if(n.pages.contains(index))continue;
            auto page=memoryPool.allocate(PageSize,true);
            uint64_t base=index*PageSize;
            if(!(offset<=base&&end>=base+PageSize)&&base<n.size)
                readData(n,base,page->data.data(),PageSize);
            fresh.emplace(index,std::move(page));
        }
        n.pages.merge(fresh);
        size_t copied=0;
        while(copied<length) {
            uint64_t pos=offset+copied;
            auto within=static_cast<size_t>(pos%PageSize);
            size_t count=std::min<size_t>(length-copied,PageSize-within);
            memcpy(n.pages.at(pos/PageSize)->data.data()+within,static_cast<unsigned char*>(input)+copied,count);
            copied+=count;
        }
        n.size=std::max(n.size,end);n.allocation=std::max(n.allocation,alignedSize(n.size));
        changed(n,true);*done=length;info(n,out);return STATUS_SUCCESS;
    }
    NTSTATUS Flush(PVOID,PVOID desc,FileInfo* out) override {
        std::lock_guard lock(gate);if(desc&&out)info(*descriptor(desc).node,out);return STATUS_SUCCESS;
    }
    NTSTATUS GetFileInfo(PVOID,PVOID desc,FileInfo* out) override {
        std::lock_guard lock(gate);info(*descriptor(desc).node,out);return STATUS_SUCCESS;
    }
    NTSTATUS SetBasicInfo(PVOID,PVOID desc,UINT32 attributes,UINT64 c,UINT64 a,UINT64 w,UINT64 x,FileInfo* out) override {
        std::lock_guard lock(gate);
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&unsupportedAttributes))return STATUS_NOT_SUPPORTED;
        auto& n=*descriptor(desc).node;
        if(attributes!=INVALID_FILE_ATTRIBUTES)setAttributes(n,attributes);
        if(c){n.created=c;n.hasCreationTime=true;}if(a){n.accessed=a;n.hasAccessTime=true;}if(w)n.modified=w;
        n.changed=x?x:nowTime();n.propertiesDirty=true;Changed();info(n,out);return STATUS_SUCCESS;
    }
    NTSTATUS SetFileSize(PVOID,PVOID desc,UINT64 size,BOOLEAN allocation,FileInfo* out) override {
        std::lock_guard lock(gate);
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        auto& n=*descriptor(desc).node;
        if(n.directory)return STATUS_FILE_IS_A_DIRECTORY;
        if(n.attributes&FILE_ATTRIBUTE_READONLY)return STATUS_ACCESS_DENIED;
        if(size>uint64_t(INT64_MAX)-4096)return STATUS_INVALID_PARAMETER;
        if(!allocation||size<n.size)resize(n,size);
        if(allocation)n.allocation=alignedSize(size);
        info(n,out);return STATUS_SUCCESS;
    }
    NTSTATUS CanDelete(PVOID n,PVOID desc,PWSTR raw) override {return SetDelete(n,desc,raw,TRUE);}
    NTSTATUS SetDelete(PVOID,PVOID desc,PWSTR,BOOLEAN remove) override {
        std::lock_guard lock(gate);
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        if(!remove)return STATUS_SUCCESS;
        auto const& n=*descriptor(desc).node;
        if(n.name.empty()||(n.attributes&FILE_ATTRIBUTE_READONLY))return STATUS_CANNOT_DELETE;
        if(n.directory&&hasChildren(n.name))return STATUS_DIRECTORY_NOT_EMPTY;
        return STATUS_SUCCESS;
    }
    NTSTATUS Rename(PVOID,PVOID desc,PWSTR,PWSTR newRaw,BOOLEAN replace) override {
        std::lock_guard lock(gate);
        if(readOnly)return STATUS_MEDIA_WRITE_PROTECTED;
        auto node=descriptor(desc).node;
        auto old=node->name,newName=volumeName(newRaw);
        if(old.empty()||newName.empty())return STATUS_ACCESS_DENIED;
        if(find(old)!=node)return STATUS_FILE_DELETED;
        auto parent=find(parentName(newName));if(!parent||!parent->directory)return STATUS_OBJECT_PATH_NOT_FOUND;
        newName=parent->name.empty()?baseName(newName):parent->name+L"\\"+baseName(newName);
        if(node->directory&&underPath(newName,old)&&!same(old,newName))return STATUS_ACCESS_DENIED;
        auto target=find(newName);
        if(target&&target!=node) {
            if(!replace)return STATUS_OBJECT_NAME_COLLISION;
            if(node->directory||target->directory||(target->attributes&FILE_ATTRIBUTE_READONLY))return STATUS_ACCESS_DENIED;
        }
        NodeMap next=nodes;
        if(target&&target!=node)next.erase(newName);
        std::vector<std::pair<NodePtr,std::wstring>> moved;
        for(auto const& [name,item]:nodes)if(underPath(name,old)) {
            moved.emplace_back(item,newName+name.substr(old.size()));
            next.erase(name);
        }
        for(auto const& [item,name]:moved)if(!next.emplace(name,item).second)return STATUS_OBJECT_NAME_COLLISION;
        for(auto& [item,name]:moved){item->name.swap(name);item->propertiesDirty=true;item->changed=nowTime();}
        nodes.swap(next);Changed();return STATUS_SUCCESS;
    }
    NTSTATUS GetSecurity(PVOID,PVOID,PSECURITY_DESCRIPTOR sd,SIZE_T* size) override {return securityInfo(sd,size);}
    NTSTATUS SetSecurity(PVOID,PVOID,SECURITY_INFORMATION,PSECURITY_DESCRIPTOR) override {
        return readOnly?STATUS_MEDIA_WRITE_PROTECTED:STATUS_NOT_SUPPORTED;
    }
    NTSTATUS ReadDirectory(PVOID,PVOID desc,PWSTR,PWSTR marker,PVOID buffer,ULONG length,PULONG done) override {
        std::lock_guard lock(gate);
        auto& d=descriptor(desc);auto const& node=*d.node;
        if(!node.directory)return STATUS_NOT_A_DIRECTORY;
        *done=0;
        NTSTATUS result=STATUS_SUCCESS;
        if(FspFileSystemAcquireDirectoryBuffer(&d.DirBuffer,!marker,&result)) {
            try {
                alignas(DirInfo) unsigned char bytes[FIELD_OFFSET(DirInfo,FileNameBuf)+256*sizeof(wchar_t)]{};
                auto record=reinterpret_cast<DirInfo*>(bytes);
                auto add=[&](MemoryNode const& n,std::wstring_view name){
                    dirRecord(n,name,record);
                    return FspFileSystemFillDirectoryBuffer(&d.DirBuffer,record,&result)!=FALSE;
                };
                bool more=true;
                if(!node.name.empty()) {
                    auto parent=find(parentName(node.name));
                    more=add(node,L".")&&parent&&add(*parent,L"..");
                }
                auto prefix=node.name.empty()?std::wstring{}:node.name+L"\\";
                for(auto it=nodes.lower_bound(prefix);more&&it!=nodes.end();++it) {
                    if(!node.name.empty()&&!underPath(it->first,node.name))break;
                    if(it->first.empty()||same(it->first,node.name)||parentName(it->first)!=node.name)continue;
                    more=add(*it->second,baseName(it->first));
                }
            }catch(...){FspFileSystemReleaseDirectoryBuffer(&d.DirBuffer);throw;}
            FspFileSystemReleaseDirectoryBuffer(&d.DirBuffer);
        }
        if(!NT_SUCCESS(result))return result;
        FspFileSystemReadDirectoryBuffer(&d.DirBuffer,marker,buffer,length,done);
        return STATUS_SUCCESS;
    }
};
}
using Ptfs=wz::MemoryFs;
using PtfsFileDesc=wz::MemoryFileDesc;
