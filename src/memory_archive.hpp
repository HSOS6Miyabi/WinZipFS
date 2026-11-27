#pragma once
#include "archive.hpp"
#include <7zip/IStream.h>
#include <Common/MyCom.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <list>
#include <tuple>
#include <functional>
#include <streambuf>
#include <istream>
#include <limits>
#include <cstring>
#include <cstdio>

namespace wz {
constexpr uint64_t MiB=1024ull*1024;
constexpr size_t PageSize=256*1024;
constexpr uint64_t WindowSize=4*MiB;
inline std::atomic<uint64_t> cleanBytes{0},dirtyBytes{0},directReadBytes{0},decodedBytes{0},decodeCalls{0};
struct MemoryFull : std::runtime_error {
    MemoryFull():std::runtime_error("The configured memory cache is full. Save changes or increase the memory limit.") {}
};
struct Page {
    std::vector<unsigned char> data;
    bool dirty;
    explicit Page(size_t size,bool modified):data(size),dirty(modified) {
        (dirty?dirtyBytes:cleanBytes).fetch_add(size,std::memory_order_relaxed);
    }
    ~Page() {
        if(!data.empty())SecureZeroMemory(data.data(),data.size());
        (dirty?dirtyBytes:cleanBytes).fetch_sub(data.size(),std::memory_order_relaxed);
    }
};
using PagePtr=std::shared_ptr<Page>;
using CacheKey=std::tuple<uint64_t,uint32_t,uint64_t>;
class MemoryPool {
    using Record=std::pair<CacheKey,PagePtr>;
    std::mutex mutex;
    std::list<Record> lru;
    std::map<CacheKey,std::list<Record>::iterator> index;
    std::atomic<uint64_t> limit{512*MiB};
    static constexpr uint64_t DecodeReserve=2*WindowSize;
    bool evict() {
        for(auto it=lru.end();it!=lru.begin();) {
            --it;
            if(it->second.use_count()!=1)continue;
            index.erase(it->first);lru.erase(it);return true;
        }
        return false;
    }
public:
    uint64_t capacity() const noexcept {return limit.load();}
    uint64_t used() const noexcept {return cleanBytes.load()+dirtyBytes.load();}
    uint64_t writable() const noexcept {
        auto cap=capacity(),dirty=dirtyBytes.load();
        return cap>DecodeReserve&&dirty<cap-DecodeReserve?cap-DecodeReserve-dirty:0;
    }
    void setLimit(uint64_t bytes) {
        if(bytes<16*MiB||bytes>64*1024*MiB)throw std::runtime_error("Memory limit must be 16–65536 MiB");
        std::lock_guard lock(mutex);
        if(dirtyBytes.load()>bytes-DecodeReserve)throw MemoryFull();
        while(used()>bytes&&evict()){}
        if(used()>bytes)throw MemoryFull();
        limit=bytes;
    }
    PagePtr allocate(size_t size,bool modified) {
        std::lock_guard lock(mutex);
        auto cap=limit.load();
        if(size>cap||(modified&&size>writable()))throw MemoryFull();
        while(used()>cap-size&&evict()){}
        if(used()>cap-size)throw MemoryFull();
        try{return std::make_shared<Page>(size,modified);}
        catch(const std::bad_alloc&){throw MemoryFull();}
    }
    PagePtr lookup(CacheKey const& key) {
        std::lock_guard lock(mutex);
        auto it=index.find(key);
        if(it==index.end())return {};
        lru.splice(lru.begin(),lru,it->second);
        return it->second->second;
    }
    void publish(CacheKey const& key,PagePtr page) {
        std::lock_guard lock(mutex);
        if(index.contains(key))return;
        lru.emplace_front(key,std::move(page));
        try{index.emplace(key,lru.begin());}catch(...){lru.pop_front();throw;}
    }
    void erase(CacheKey const& key) noexcept {
        std::lock_guard lock(mutex);
        auto it=index.find(key);
        if(it!=index.end()){lru.erase(it->second);index.erase(it);}
    }
    void drop(uint64_t source) noexcept {
        std::lock_guard lock(mutex);
        auto it=index.lower_bound(CacheKey{source,0,0});
        while(it!=index.end()&&std::get<0>(it->first)==source) {
            lru.erase(it->second);it=index.erase(it);
        }
    }
};
inline MemoryPool memoryPool;
inline std::atomic<uint64_t> sourceIds{1};

// A seekable std::istream over a logical byte source. No temporary file is used.
class RangeBuffer final : public std::streambuf {
    uint64_t size_,position=0;
    std::function<size_t(uint64_t,void*,size_t)> read_;
protected:
    std::streamsize xsgetn(char* out,std::streamsize n) override {
        if(n<=0||position>=size_)return 0;
        auto count=static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(n),size_-position));
        auto got=read_(position,out,count);
        if(got>count)throw std::runtime_error("Invalid input stream length");
        position+=got;return static_cast<std::streamsize>(got);
    }
    int_type underflow() override {
        if(position>=size_)return traits_type::eof();
        unsigned char ch=0;
        if(read_(position,&ch,1)!=1)return traits_type::eof();
        return traits_type::to_int_type(static_cast<char>(ch));
    }
    int_type uflow() override {
        auto ch=underflow();
        if(!traits_type::eq_int_type(ch,traits_type::eof()))++position;
        return ch;
    }
    pos_type seekoff(off_type off,std::ios_base::seekdir dir,std::ios_base::openmode mode) override {
        if(!(mode&std::ios_base::in))return pos_type(off_type(-1));
        uint64_t base=dir==std::ios_base::beg?0:dir==std::ios_base::cur?position:dir==std::ios_base::end?size_:UINT64_MAX;
        if(base==UINT64_MAX)return pos_type(off_type(-1));
        if(off<0) {
            uint64_t delta=uint64_t(-(off+1))+1;
            if(delta>base)return pos_type(off_type(-1));
            base-=delta;
        }else {
            if(uint64_t(off)>uint64_t(INT64_MAX)-base)return pos_type(off_type(-1));
            base+=uint64_t(off);
        }
        position=base;return pos_type(static_cast<off_type>(position));
    }
    pos_type seekpos(pos_type pos,std::ios_base::openmode mode) override {
        return seekoff(static_cast<off_type>(pos),std::ios_base::beg,mode);
    }
public:
    RangeBuffer(uint64_t size,std::function<size_t(uint64_t,void*,size_t)> read)
        :size_(size),read_(std::move(read)) {
        if(size>uint64_t(INT64_MAX))throw std::runtime_error("Input stream is too large");
    }
};
class RangeStream final : public std::istream {
    RangeBuffer buffer;
public:
    RangeStream(uint64_t size,std::function<size_t(uint64_t,void*,size_t)> read)
        :std::istream(nullptr),buffer(size,std::move(read)) {rdbuf(&buffer);}
};

// 7-Zip accepts an empty TAR by filename, but its unnamed stream interface
// requires at least one header. Expose an empty root directory virtually.
inline std::unique_ptr<RangeStream> tarInput(uint64_t size,
    std::function<size_t(uint64_t,void*,size_t)> read) {
    std::array<unsigned char,1024> probe{};
    if(size>=1024&&size%512==0&&read(0,probe.data(),probe.size())==probe.size()&&
       std::all_of(probe.begin(),probe.end(),[](auto ch){return ch==0;})) {
        for(uint64_t off=1024;off<size;) {
            auto n=static_cast<size_t>(std::min<uint64_t>(probe.size(),size-off));
            if(read(off,probe.data(),n)!=n||
               !std::all_of(probe.begin(),probe.begin()+n,[](auto ch){return ch==0;}))
                throw std::runtime_error("Invalid empty TAR");
            off+=n;
        }
        auto bytes=std::make_shared<std::array<unsigned char,1536>>();
        auto h=bytes->data();
        memcpy(h,"./",2);
        memcpy(h+100,"0000755",7);memcpy(h+108,"0000000",7);memcpy(h+116,"0000000",7);
        memcpy(h+124,"00000000000",11);memcpy(h+136,"00000000000",11);
        memset(h+148,' ',8);h[156]='5';memcpy(h+257,"ustar",5);h[263]='0';h[264]='0';
        unsigned sum=0;for(size_t i=0;i<512;++i)sum+=h[i];
        char check[8]{};std::snprintf(check,sizeof(check),"%06o",sum);
        memcpy(h+148,check,6);h[154]=0;h[155]=' ';
        return std::make_unique<RangeStream>(bytes->size(),[bytes](uint64_t off,void* out,size_t n) {
            memcpy(out,bytes->data()+static_cast<size_t>(off),n);return n;
        });
    }
    return std::make_unique<RangeStream>(size,std::move(read));
}

class DiskInput {
    HANDLE file=INVALID_HANDLE_VALUE;
    BY_HANDLE_FILE_INFORMATION initial{};
    std::mutex io;
public:
    explicit DiskInput(fs::path const& path) {
        file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_RANDOM_ACCESS,nullptr);
        if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot open compressed archive");
        if(!GetFileInformationByHandle(file,&initial)) {
            CloseHandle(file);file=INVALID_HANDLE_VALUE;
            throw std::runtime_error("Cannot identify compressed archive");
        }
    }
    ~DiskInput(){if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);}
    DiskInput(const DiskInput&)=delete;
    DiskInput& operator=(const DiskInput&)=delete;
    uint64_t size() const {return (uint64_t(initial.nFileSizeHigh)<<32)|initial.nFileSizeLow;}
    void verify() const {
        BY_HANDLE_FILE_INFORMATION info{};
        if(!GetFileInformationByHandle(file,&info)||
           info.nFileSizeHigh!=initial.nFileSizeHigh||info.nFileSizeLow!=initial.nFileSizeLow||
           CompareFileTime(&info.ftLastWriteTime,&initial.ftLastWriteTime)!=0)
            throw std::runtime_error("Original archive changed during the session");
    }
    size_t read(uint64_t offset,void* out,size_t count) {
        std::lock_guard lock(io);
        if(offset>=size())return 0;
        count=static_cast<size_t>(std::min<uint64_t>(count,size()-offset));
        LARGE_INTEGER seek{};seek.QuadPart=static_cast<LONGLONG>(offset);
        if(!SetFilePointerEx(file,seek,nullptr,FILE_BEGIN))throw std::runtime_error("Archive seek failed");
        size_t total=0;
        while(total<count) {
            DWORD done=0,n=static_cast<DWORD>(std::min<size_t>(count-total,MAXDWORD));
            if(!ReadFile(file,static_cast<unsigned char*>(out)+total,n,&done,nullptr)||!done)
                throw std::runtime_error("Archive read failed");
            total+=done;
        }
        return total;
    }
};
}
#include "zip_access.hpp"
namespace wz {
class SeekableReader final : public BitAbstractArchiveOpener,public BitInputArchive {
public:
    SeekableReader(Bit7zLibrary const& lib,std::istream& stream,BitInFormat const& format,std::string const& password)
        :BitAbstractArchiveOpener(lib,format,password),BitInputArchive(*this,stream) {}
    SeekableReader(Bit7zLibrary const& lib,fs::path const& path,BitInFormat const& format,std::string const& password)
        :BitAbstractArchiveOpener(lib,format,password),BitInputArchive(*this,path8(path)) {}
    bool hasEncryptedItems() const {
        for(auto const& item:*this)if(!item.isDir()&&item.isEncrypted())return true;
        return false;
    }
    bool isMultiVolume() const {
        return detectedFormat()==BitFormat::Split||ends(wide(archivePath()),L".001")||
               present(archiveProperty(BitProperty::IsVolume));
    }
    CMyComPtr<IInStream> stream(uint32_t index) const {return getSubfileStream(index);}
};
inline std::optional<uint64_t> numberProperty(BitPropVariant const& prop) {
    if(prop.isUInt64())return prop.getUInt64();
    if(prop.isUInt32())return prop.getUInt32();
    return {};
}

class ArchiveSource : public std::enable_shared_from_this<ArchiveSource> {
    const uint64_t id=sourceIds.fetch_add(1);
    std::mutex mutex;
    std::shared_ptr<DiskInput> file;
    std::shared_ptr<ArchiveSource> parent;
    std::unique_ptr<RangeStream> input;
    std::unique_ptr<SeekableReader> reader_;
    std::unique_ptr<ZipAccess> zip;
    std::map<uint32_t,uint64_t> sizes;
    std::set<uint32_t> sequentialOnly;
    bool protected_=false;
    std::wstring standaloneName_;
    static void readExact(IInStream* stream,uint64_t offset,void* out,size_t count) {
        UInt64 position=0;
        if(stream->Seek(static_cast<Int64>(offset),STREAM_SEEK_SET,&position)!=S_OK||position!=offset)
            throw std::runtime_error("Archive item seek failed");
        size_t total=0;
        while(total<count) {
            UInt32 done=0,n=static_cast<UInt32>(std::min<size_t>(count-total,MAXDWORD));
            if(stream->Read(static_cast<unsigned char*>(out)+total,n,&done)!=S_OK||!done||done>n)
                throw std::runtime_error("Archive item read failed");
            total+=done;
        }
    }
    uint64_t sizeLocked(uint32_t index) {
        if(auto it=sizes.find(index);it!=sizes.end())return it->second;
        auto property=numberProperty(reader_->itemProperty(index,BitProperty::Size));
        if(property&&!singleStream(reader_->detectedFormat()))return sizes[index]=*property;
        // Single-stream handlers may report a missing or modulo-2^32 size.
        uint64_t total=0;
        bool overflow=false;
        reader_->extractTo(RawDataCallback([&](const byte_t*,size_t bytes){
            if(bytes>uint64_t(INT64_MAX)-total){overflow=true;return false;}
            total+=bytes;decodedBytes.fetch_add(bytes,std::memory_order_relaxed);return true;
        }),index);
        if(overflow)throw std::runtime_error("Decompressed stream is too large");
        return sizes[index]=total;
    }
    PagePtr window(uint32_t index,uint64_t base,uint64_t size) {
        CacheKey key{id,index,base};
        if(auto hit=memoryPool.lookup(key))return hit;
        auto count=static_cast<size_t>(std::min<uint64_t>(WindowSize,size-base));
        auto page=memoryPool.allocate(count,false);
        if(zip&&zip->deflated(index)) {
            ++decodeCalls;
            zip->readDeflated(index,base,*page);
            memoryPool.publish(key,page);return page;
        }
        uint64_t offset=0,received=0;
        bool stop=false,badLength=false;
        ++decodeCalls;
        try {
            reader_->extractTo(RawDataCallback([&](const byte_t* bytes,size_t length) {
                decodedBytes.fetch_add(length,std::memory_order_relaxed);
                if(length>size-offset){badLength=true;return false;}
                uint64_t next=offset+length;
                uint64_t begin=std::max(offset,base),end=std::min(next,base+count);
                if(begin<end) {
                    auto copied=static_cast<size_t>(end-begin);
                    memcpy(page->data.data()+static_cast<size_t>(begin-base),
                           reinterpret_cast<const unsigned char*>(bytes)+static_cast<size_t>(begin-offset),copied);
                    received+=copied;
                }
                offset=next;
                // Stop a sequential decoder after the requested window.
                // A complete item decode still checks its archive checksum.
                if(received==count&&offset<size){stop=true;return false;}
                return true;
            }),index);
        }catch(const BitException&) {if(!stop||badLength||received!=count)throw;}
        if(badLength||received!=count)throw std::runtime_error("Archive item length mismatch");
        memoryPool.publish(key,page);return page;
    }
public:
    ArchiveSource()=default;
    ~ArchiveSource(){memoryPool.drop(id);}
    SeekableReader const& reader() const {return *reader_;}
    std::wstring const& standaloneName() const noexcept {return standaloneName_;}
    void verify() const {
        if(file)file->verify();
        if(parent)parent->verify();
    }
    uint64_t itemSize(uint32_t index) {
        std::lock_guard lock(mutex);verify();return sizeLocked(index);
    }
    size_t read(uint32_t index,uint64_t offset,void* out,size_t count) {
        std::lock_guard lock(mutex);
        verify();
        auto size=sizeLocked(index);
        if(offset>=size||!count)return 0;
        count=static_cast<size_t>(std::min<uint64_t>(count,size-offset));
        if(zip&&zip->stored(index)) {
            zip->readStored(index,offset,out,count);
            directReadBytes.fetch_add(count,std::memory_order_relaxed);return count;
        }
        if(!protected_&&!(zip&&zip->deflated(index))&&!sequentialOnly.contains(index)) {
            CMyComPtr<IInStream> stream;
            try {stream=reader_->stream(index);}
            catch(const BitException&){sequentialOnly.insert(index);}
            if(stream) {
                readExact(stream,offset,out,count);
                directReadBytes.fetch_add(count,std::memory_order_relaxed);
                return count;
            }
            sequentialOnly.insert(index);
        }
        size_t done=0;
        while(done<count) {
            uint64_t pos=offset+done,base=pos/WindowSize*WindowSize;
            auto page=window(index,base,size);
            auto within=static_cast<size_t>(pos-base);
            auto n=std::min(count-done,page->data.size()-within);
            memcpy(static_cast<unsigned char*>(out)+done,page->data.data()+within,n);
            done+=n;
        }
        return done;
    }
    static std::shared_ptr<ArchiveSource> open(Bit7zLibrary const& lib,fs::path const& path,
                                               std::string const& password,BitInFormat const* expected=nullptr) {
        auto source=std::make_shared<ArchiveSource>();
        source->file=std::make_shared<DiskInput>(path);
        const BitInFormat* format=expected;
        bool multi=false;
        if(!format) {
            BitArchiveReader probe(lib,path8(path),BitFormat::Auto,password);
            format=&probe.detectedFormat();multi=probe.isMultiVolume();
            if(singleStream(*format)&&probe.itemsCount()==1)
                source->standaloneName_=probe.itemAt(0).rawPath();
            if(probe.hasEncryptedItems()&&password.empty())throw PasswordRequired();
        }
        source->protected_=!password.empty();
        if(multi)source->reader_=std::make_unique<SeekableReader>(lib,path,*format,password);
        else {
            auto file=source->file;
            auto read=[file](uint64_t off,void* data,size_t n){return file->read(off,data,n);};
            if(*format==BitFormat::Tar)source->input=tarInput(file->size(),read);
            else source->input=std::make_unique<RangeStream>(file->size(),read);
            source->reader_=std::make_unique<SeekableReader>(lib,*source->input,*format,password);
        }
        if(*format==BitFormat::Zip&&!multi&&!source->protected_) {
            source->zip=std::make_unique<ZipAccess>(source->file);
            for(auto const& item:*source->reader_)if(!item.isDir()&&!item.isEncrypted())
                source->zip->add(item.index(),item.rawPath(),item.size(),item.crc());
        }
        return source;
    }
    static std::shared_ptr<ArchiveSource> tar(Bit7zLibrary const& lib,std::shared_ptr<ArchiveSource> outer) {
        if(outer->reader().itemsCount()!=1||outer->reader().isItemFolder(0))
            throw std::runtime_error("Invalid compressed TAR");
        auto source=std::make_shared<ArchiveSource>();
        source->parent=outer;
        auto size=outer->itemSize(0);
        source->input=tarInput(size,[outer](uint64_t off,void* data,size_t n){
            return outer->read(0,off,data,n);
        });
        source->reader_=std::make_unique<SeekableReader>(lib,*source->input,BitFormat::Tar,std::string{});
        return source;
    }
};
}
