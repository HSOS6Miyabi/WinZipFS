#pragma once
#include "filesystem.hpp"
#include <cstdio>

namespace wz {
struct LoadedArchive {
    std::shared_ptr<ArchiveSource> source;
    Format format;
    bool standalone=false;
};
inline LoadedArchive openArchive(Bit7zLibrary const& lib,fs::path const& path,
                                 std::string const& password={},bool forceReadOnly=false) {
    try {
        const bool writable=!forceReadOnly&&password.empty()&&isWritable(path);
        auto source=ArchiveSource::open(lib,path,password);
        auto const& outer=source->reader();
        auto format=formatFor(path,outer.detectedFormat());
        auto name=lower(path.filename().wstring());
        bool zstdTar=(ends(name,L".tar.zst")||ends(name,L".tzst"))&&outer.detectedFormat()==BitFormat::Zstd;
        bool standalone=singleStream(outer.detectedFormat())&&!format.outer&&!zstdTar;
        bool encrypted=outer.hasEncryptedItems();
        if(encrypted&&password.empty())throw PasswordRequired();
        if(encrypted) {
            // Verify the supplied password before presenting a mounted volume.
            for(auto const& item:outer)if(!item.isDir()&&item.isEncrypted()) {
                try{outer.testItem(item.index());}catch(BitException const&){throw PasswordRequired();}
                break;
            }
        }
        bool restricted=archiveReadOnly(outer);
        if(format.outer||zstdTar) {
            source=ArchiveSource::tar(lib,source);
            restricted=restricted||archiveReadOnly(source->reader())||zstdTar;
        }
        restricted=validateArchive(source->reader(),!password.empty(),standalone)||restricted;
        if(!writable||restricted)format={};
        return {std::move(source),format,standalone};
    }catch(BitException const& e) {
        if(e.code()==BitFailureSource::WrongPassword||
           (password.empty()&&BitArchiveReader::isHeaderEncrypted(lib,path8(path),BitFormat::Auto)))
            throw PasswordRequired();
        throw;
    }
}

// bit7z exposes the update callbacks through BitOutputArchive's virtual
// methods. Entries are supplied by seekable streams over the live archive
// and the memory overlay; directory records require no filesystem objects.
class MemoryWriter final : public BitArchiveWriter {
    struct Record {NodePtr node;bool old;};
    std::map<uint32_t,Record> records;
    std::vector<std::unique_ptr<RangeStream>> streams;
    static inline const buffer_t empty{};
    const MemoryFs& fs_;
    void add(NodePtr const& node,uint32_t index) {
        if(node->directory)addFile(empty,utf8(node->name));
        else {
            auto stream=std::make_unique<RangeStream>(node->size,[this,node](uint64_t off,void* data,size_t n){
                return fs_.readData(*node,off,data,n);
            });
            addFile(*stream,utf8(node->name));
            streams.push_back(std::move(stream));
        }
        records.emplace(index,Record{node,false});
    }
    void populate(bool incremental) {
        uint32_t oldCount=incremental?inputArchiveItemsCount():0,next=oldCount;
        std::set<uint32_t> retained;
        for(auto const& [name,node]:fs_.entries()) {
            if(name.empty())continue;
            if(incremental&&node->sourceIndex&&!node->dataDirty&&*node->sourceIndex<oldCount) {
                auto index=*node->sourceIndex;
                if(!retained.insert(index).second)throw std::runtime_error("Duplicate source item");
                records.emplace(index,Record{node,true});
            }else {
                if(next==UINT32_MAX)throw std::runtime_error("Too many archive entries");
                add(node,next++);
            }
        }
        for(uint32_t i=0;i<oldCount;++i)if(!retained.contains(i))setDeletedIndex(i);
    }
protected:
    BitPropVariant itemProperty(InputIndex index,BitProperty property) const override {
        auto const& record=records.at(static_cast<uint32_t>(index));
        auto const& n=*record.node;
        if(record.old&&!n.propertiesDirty)return inputArchive()->itemProperty(static_cast<uint32_t>(index),property);
        switch(property) {
        case BitProperty::Path:return BitPropVariant(n.name);
        case BitProperty::IsDir:return BitPropVariant(n.directory);
        case BitProperty::Size:return BitPropVariant(n.directory?uint64_t(0):n.size);
        case BitProperty::Attrib:return BitPropVariant(static_cast<uint32_t>(
            n.attributes|(n.directory?FILE_ATTRIBUTE_DIRECTORY:0)|
            (n.posixMode?((*n.posixMode&0xffffu)<<16)|0x8000u:0u)));
        case BitProperty::CTime:return BitPropVariant(fileTime(n.created));
        case BitProperty::ATime:return BitPropVariant(fileTime(n.accessed));
        case BitProperty::MTime:return BitPropVariant(fileTime(n.modified));
        default:
            return record.old?inputArchive()->itemProperty(static_cast<uint32_t>(index),property):BitPropVariant{};
        }
    }
    HRESULT itemStream(InputIndex index,ISequentialInStream** out) const override {
        *out=nullptr;
        auto const& record=records.at(static_cast<uint32_t>(index));
        if(record.node->directory||record.old)return S_OK;
        return BitOutputArchive::itemStream(index,out);
    }
    bool hasNewData(uint32_t index) const noexcept override {
        return !records.at(static_cast<uint32_t>(itemInputIndex(index))).old;
    }
    bool hasNewProperties(uint32_t index) const noexcept override {
        auto const& record=records.at(static_cast<uint32_t>(itemInputIndex(index)));
        return !record.old||record.node->propertiesDirty;
    }
public:
    MemoryWriter(Bit7zLibrary const& lib,BitInOutFormat const& format,MemoryFs const& fs,WzOptions const& options)
        :BitArchiveWriter(lib,format),fs_(fs) {configure(*this,format,options);populate(false);}
    MemoryWriter(Bit7zLibrary const& lib,BitInOutFormat const& format,MemoryFs const& fs,WzOptions const& options,
                 fs::path const& original)
        :BitArchiveWriter(lib,path8(original),format),fs_(fs) {
        configure(*this,format,options);setUpdateMode(UpdateMode::Append);populate(true);
    }
};

// Generate a seekable TAR byte stream directly from metadata and file streams.
// The outer compressor can consume this stream without buffering the entire TAR.
class TarLayout {
    struct Segment {
        uint64_t offset=0,length=0;
        std::vector<unsigned char> header;
        NodePtr node;
    };
    const MemoryFs& fs_;
    std::vector<Segment> segments;
    uint64_t size_=0;
    static void number(unsigned char* field,size_t length,uint64_t value) {
        uint64_t max=1;
        for(size_t i=1;i<length;++i)max*=8;
        if(value<max) {
            memset(field,'0',length);field[length-1]=0;
            for(size_t i=length-1;i>0&&value;){field[--i]=static_cast<unsigned char>('0'+(value&7));value>>=3;}
        }else {
            memset(field,0,length);
            for(size_t i=length;i>0&&value;){field[--i]=static_cast<unsigned char>(value&255);value>>=8;}
            field[0]|=0x80;
        }
    }
    static std::vector<unsigned char> header(std::string const& name,uint64_t size,char type,uint64_t time,
                                              uint32_t mode,uint32_t uid=0,uint32_t gid=0,
                                              std::string_view user={},std::string_view group={}) {
        if(name.size()>100)throw std::runtime_error("Invalid TAR header name");
        std::vector<unsigned char> h(512,0);
        memcpy(h.data(),name.data(),name.size());
        number(h.data()+100,8,mode&07777u);
        number(h.data()+108,8,uid);number(h.data()+116,8,gid);number(h.data()+124,12,size);
        if(!user.empty()&&user.size()<=32)memcpy(h.data()+265,user.data(),user.size());
        if(!group.empty()&&group.size()<=32)memcpy(h.data()+297,group.data(),group.size());
        constexpr uint64_t epoch=116444736000000000ull;
        number(h.data()+136,12,time>epoch?(time-epoch)/10000000:0);
        memset(h.data()+148,' ',8);h[156]=static_cast<unsigned char>(type);
        memcpy(h.data()+257,"ustar",5);h[263]='0';h[264]='0';
        uint64_t sum=0;for(auto c:h)sum+=c;
        char check[8]{};std::snprintf(check,sizeof(check),"%06o",static_cast<unsigned>(sum));
        memcpy(h.data()+148,check,6);h[154]=0;h[155]=' ';
        return h;
    }
    void append(Segment segment) {
        if(segment.length>uint64_t(INT64_MAX)-size_)throw std::runtime_error("TAR archive too large");
        if(!segment.length)return;
        segment.offset=size_;size_+=segment.length;segments.push_back(std::move(segment));
    }
    void bytes(std::vector<unsigned char> value) {
        auto n=value.size();append({0,n,std::move(value),{}});
    }
    void zeros(uint64_t n){append({0,n,{},{}});}
    void padding(uint64_t n){zeros((512-(n%512))%512);}
    static std::string paxRecord(std::string_view key,std::string const& value) {
        auto body=std::string(" ")+std::string(key)+"="+value+"\n";
        size_t n=body.size()+1;
        for(;;){auto next=body.size()+std::to_string(n).size();if(next==n)break;n=next;}
        return std::to_string(n)+body;
    }
    static std::string unixTime(uint64_t time) {
        constexpr uint64_t epoch=116444736000000000ull;
        bool negative=time<epoch;
        uint64_t delta=negative?epoch-time:time-epoch;
        auto result=(negative?std::string("-"):std::string{})+std::to_string(delta/10000000);
        if(auto fraction=delta%10000000) {
            auto digits=std::to_string(fraction);
            result+="."+std::string(7-digits.size(),'0')+digits;
        }
        return result;
    }
public:
    explicit TarLayout(MemoryFs const& fs):fs_(fs) {
        unsigned id=0;
        for(auto const& [name,node]:fs.entries()) {
            if(name.empty())continue;
            auto path=utf8(name);std::replace(path.begin(),path.end(),'\\','/');
            if(node->directory)path+='/';
            auto fallback=path;
            std::string pax;
            if(path.size()>100) {
                pax+=paxRecord("path",path);
                fallback="Entry/"+std::to_string(id+1);
            }
            if(node->user.size()>32)pax+=paxRecord("uname",node->user);
            if(node->group.size()>32)pax+=paxRecord("gname",node->group);
            constexpr uint64_t epoch=116444736000000000ull;
            if(node->modified<epoch||(node->modified-epoch)%10000000)
                pax+=paxRecord("mtime",unixTime(node->modified));
            if(node->hasAccessTime)pax+=paxRecord("atime",unixTime(node->accessed));
            if(node->hasCreationTime)pax+=paxRecord("ctime",unixTime(node->created));
            if(!pax.empty()) {
                auto tag="PaxHeaders/"+std::to_string(++id);
                bytes(header(tag,pax.size(),'x',node->modified,0644));
                bytes(std::vector<unsigned char>(pax.begin(),pax.end()));padding(pax.size());
            }
            auto length=node->directory?0:node->size;
            uint32_t mode=node->posixMode.value_or(node->directory?0755u:(node->attributes&FILE_ATTRIBUTE_READONLY)?0444u:0644u);
            bytes(header(fallback,length,node->directory?'5':'0',node->modified,mode,node->uid,node->gid,node->user,node->group));
            if(length)append({0,length,{},node});
            padding(length);
        }
        zeros(1024);
    }
    uint64_t size() const {return size_;}
    size_t read(uint64_t off,void* out,size_t count) const {
        if(off>=size_)return 0;
        count=static_cast<size_t>(std::min<uint64_t>(count,size_-off));
        size_t done=0;
        auto it=std::upper_bound(segments.begin(),segments.end(),off,
            [](uint64_t pos,Segment const& s){return pos<s.offset;});
        if(it!=segments.begin())--it;
        while(done<count&&it!=segments.end()) {
            auto within=off+done-it->offset;
            auto n=static_cast<size_t>(std::min<uint64_t>(count-done,it->length-within));
            auto target=static_cast<unsigned char*>(out)+done;
            if(it->node) {
                if(fs_.readData(*it->node,within,target,n)!=n)throw std::runtime_error("Short TAR input");
            }else if(!it->header.empty())memcpy(target,it->header.data()+static_cast<size_t>(within),n);
            else memset(target,0,n);
            done+=n;++it;
        }
        return done;
    }
};
inline void verifyArchive(SeekableReader const& reader,MemoryFs const& fs,bool incremental) {
    std::set<std::wstring,CaseLess> seen,explicitNames;
    for(auto const& item:reader) {
        auto name=safeName(item.rawPath(),item.isDir());
        if(name.empty())continue;
        auto expected=fs.entries().find(name);
        if(expected==fs.entries().end()||expected->first!=name||expected->second->directory!=item.isDir()||
           (!item.isDir()&&expected->second->size!=item.size())||!explicitNames.insert(name).second)
            throw std::runtime_error("Generated archive does not match requested files");
        seen.insert(name);
        auto const& node=*expected->second;
        if(!node.directory) {
            if(incremental&&node.sourceIndex&&!node.dataDirty) {
                auto original=fs.source()->reader().itemProperty(*node.sourceIndex,BitProperty::CRC);
                auto actual=reader.itemProperty(item.index(),BitProperty::CRC);
                if(original.isUInt32()&&(!actual.isUInt32()||original.getUInt32()!=actual.getUInt32()))
                    throw std::runtime_error("Unchanged archive entry CRC mismatch: "+utf8(name));
            }else {
                uint64_t offset=0;
                std::exception_ptr failure;
                std::array<unsigned char,65536> expectedBytes{};
                try {
                    reader.extractTo(RawDataCallback([&](const byte_t* data,size_t count) {
                        try {
                            if(count>node.size-offset)throw std::runtime_error("Generated file is longer than expected");
                            size_t done=0;
                            while(done<count) {
                                auto n=std::min(expectedBytes.size(),count-done);
                                if(fs.readData(node,offset+done,expectedBytes.data(),n)!=n||
                                   memcmp(expectedBytes.data(),reinterpret_cast<const unsigned char*>(data)+done,n))
                                    throw std::runtime_error("Generated archive data mismatch: "+utf8(name));
                                done+=n;
                            }
                            offset+=count;return true;
                        }catch(...){failure=std::current_exception();return false;}
                    }),item.index());
                }catch(...){if(failure)std::rethrow_exception(failure);throw;}
                if(failure)std::rethrow_exception(failure);
                if(offset!=node.size)throw std::runtime_error("Generated file is shorter than expected");
            }
        }
        for(auto parent=parentName(name);!parent.empty();parent=parentName(parent))seen.insert(parent);
    }
    for(auto const& [name,node]:fs.entries())if(!name.empty()&&!seen.contains(name))
        throw std::runtime_error("Generated archive is missing "+utf8(name));
    if(reader.itemsCount())reader.test();
}
inline void packMemory(Bit7zLibrary const& lib,MemoryFs const& fs,fs::path const& output,Format fmt,
                       WzOptions const& options,fs::path const* original=nullptr) {
    if(!fmt.inner)throw std::runtime_error("Archive is read-only");
    if(fmt.outer) {
        TarLayout tar(fs);
        RangeStream input(tar.size(),[&](uint64_t off,void* data,size_t n){return tar.read(off,data,n);});
        BitArchiveWriter writer(lib,*fmt.outer);
        configure(writer,*fmt.outer,options);
        writer.addFile(input,"archive.tar");
        writer.compressTo(path8(output));
    }else if(fmt.inner==&BitFormat::Tar) {
        TarLayout tar(fs);
        std::ofstream file(output,std::ios::binary|std::ios::trunc);
        if(!file)throw std::runtime_error("Cannot create TAR archive");
        std::vector<char> bytes(1024*1024);
        for(uint64_t offset=0;offset<tar.size();) {
            auto n=tar.read(offset,bytes.data(),bytes.size());
            if(!n)throw std::runtime_error("Short TAR input");
            file.write(bytes.data(),static_cast<std::streamsize>(n));offset+=n;
            if(!file)throw std::runtime_error("Cannot write TAR archive");
        }
        file.close();if(!file)throw std::runtime_error("Cannot close TAR archive");
    }else if(original&&incrementalFormat(fmt)) {
        MemoryWriter writer(lib,*fmt.inner,fs,options,*original);
        writer.compressTo(path8(output));
    }else {
        MemoryWriter writer(lib,*fmt.inner,fs,options);
        writer.compressTo(path8(output));
    }
    auto result=ArchiveSource::open(lib,output,{},fmt.outer?static_cast<const BitInFormat*>(fmt.outer):fmt.inner);
    if(fmt.outer)result=ArchiveSource::tar(lib,result);
    verifyArchive(result->reader(),fs,original&&incrementalFormat(fmt));
}
}
