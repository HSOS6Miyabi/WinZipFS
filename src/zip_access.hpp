#pragma once
// Included by memory_archive.hpp after DiskInput and the bounded memory pool.
#include <zip.h>
#include <zlib.h>
#include <cerrno>

namespace wz {
class ZipAccess {
    struct Source {
        std::shared_ptr<DiskInput> file;
        uint64_t pos=0;
        zip_error_t error;
        explicit Source(std::shared_ptr<DiskInput> f):file(std::move(f)){zip_error_init(&error);}
        ~Source(){zip_error_fini(&error);}
    };
    static zip_int64_t callback(void* userdata,void* data,zip_uint64_t len,zip_source_cmd_t cmd) noexcept {
        auto s=static_cast<Source*>(userdata);
        try {
            switch(cmd) {
            case ZIP_SOURCE_OPEN:s->pos=0;return 0;
            case ZIP_SOURCE_CLOSE:return 0;
            case ZIP_SOURCE_READ:{
                auto count=static_cast<size_t>(std::min<uint64_t>(len,static_cast<uint64_t>(SIZE_MAX)));
                auto n=s->file->read(s->pos,data,count);s->pos+=n;
                return static_cast<zip_int64_t>(n);
            }
            case ZIP_SOURCE_STAT:{
                if(len<sizeof(zip_stat_t)){zip_error_set(&s->error,ZIP_ER_INVAL,0);return -1;}
                auto st=static_cast<zip_stat_t*>(data);zip_stat_init(st);
                st->size=s->file->size();st->valid=ZIP_STAT_SIZE;return sizeof(*st);
            }
            case ZIP_SOURCE_SEEK:{
                auto off=zip_source_seek_compute_offset(s->pos,s->file->size(),data,len,&s->error);
                if(off<0)return -1;
                s->pos=static_cast<uint64_t>(off);return 0;
            }
            case ZIP_SOURCE_TELL:return static_cast<zip_int64_t>(s->pos);
            case ZIP_SOURCE_ERROR:return zip_error_to_data(&s->error,data,len);
            case ZIP_SOURCE_SUPPORTS:return ZIP_SOURCE_SUPPORTS_SEEKABLE;
            case ZIP_SOURCE_FREE:delete s;return 0;
            default:zip_error_set(&s->error,ZIP_ER_OPNOTSUPP,0);return -1;
            }
        }catch(...) {zip_error_set(&s->error,ZIP_ER_READ,EIO);return -1;}
    }
    struct Checkpoint {
        uint64_t compressed=0,logical=0;
        uint32_t crc=0,dictionarySize=0;
        unsigned bits=0;
        std::array<unsigned char,32768> dictionary{};
    };
    struct Entry {
        zip_uint64_t zipIndex=0;
        uint64_t size=0,compressedSize=0;
        uint32_t crc=0;
        uint16_t method=0;
        std::set<uint64_t> points;
    };
    struct File {
        zip_file_t* value=nullptr;
        File(zip_t* archive,zip_uint64_t index,zip_flags_t flags)
            :value(zip_fopen_index(archive,index,flags)){}
        ~File(){if(value)zip_fclose(value);}
        File(File const&)=delete;
        File& operator=(File const&)=delete;
        void seek(uint64_t off) {
            if(off>INT64_MAX||zip_fseek(value,static_cast<zip_int64_t>(off),SEEK_SET))
                throw std::runtime_error("ZIP compressed stream seek failed");
        }
        size_t read(void* out,size_t count) {
            auto n=zip_fread(value,out,count);
            if(n<0)throw std::runtime_error("ZIP stream read failed");
            return static_cast<size_t>(n);
        }
    };
    struct Decoder {
        z_stream stream{};
        Decoder() {
            if(inflateInit2(&stream,-MAX_WBITS)!=Z_OK)throw std::runtime_error("Cannot initialize DEFLATE decoder");
        }
        ~Decoder(){inflateEnd(&stream);}
    };
    zip_t* archive=nullptr;
    const uint64_t checkpointId=sourceIds.fetch_add(1);
    std::map<uint32_t,Entry> entries;
    static constexpr uint64_t CheckpointInterval=4*MiB;
    static constexpr size_t MaximumPoints=4096;
    void savePoint(uint32_t item,Entry& entry,z_stream& z,uint64_t logical,uint64_t compressed,uint32_t crc) {
        if(entry.points.contains(logical)) {
            if(memoryPool.lookup(CacheKey{checkpointId,item,logical}))return;
            entry.points.erase(logical);
        }
        Checkpoint point{};
        point.logical=logical;point.compressed=compressed;point.crc=crc;
        point.bits=static_cast<unsigned>(z.data_type)&7;
        uInt length=0;
        if(inflateGetDictionary(&z,point.dictionary.data(),&length)!=Z_OK)return;
        point.dictionarySize=length;
        try {
            auto data=memoryPool.allocate(sizeof(point),false);
            memcpy(data->data.data(),&point,sizeof(point));
            memoryPool.publish(CacheKey{checkpointId,item,logical},std::move(data));
            entry.points.insert(logical);
            if(entry.points.size()>MaximumPoints) {
                auto oldest=entry.points.begin();
                memoryPool.erase(CacheKey{checkpointId,item,*oldest});
                entry.points.erase(oldest);
            }
        }catch(const MemoryFull&) {
            // Index entries are optional; the stream remains readable.
        }catch(const std::bad_alloc&) {}
    }
    Checkpoint findPoint(uint32_t item,Entry& entry,uint64_t offset) {
        auto it=entry.points.upper_bound(offset);
        while(it!=entry.points.begin()) {
            --it;
            auto data=memoryPool.lookup(CacheKey{checkpointId,item,*it});
            if(data) {
                Checkpoint point{};
                memcpy(&point,data->data.data(),sizeof(point));
                return point;
            }
            it=entry.points.erase(it);
        }
        Checkpoint start{};start.crc=static_cast<uint32_t>(crc32(0,Z_NULL,0));return start;
    }
public:
    explicit ZipAccess(std::shared_ptr<DiskInput> file) {
        zip_error_t error;zip_error_init(&error);
        auto context=std::make_unique<Source>(std::move(file));
        auto src=zip_source_function_create(callback,context.get(),&error);
        if(src) {
            context.release();
            archive=zip_open_from_source(src,ZIP_RDONLY,&error);
            if(!archive)zip_source_free(src);
        }
        zip_error_fini(&error);
    }
    ~ZipAccess(){memoryPool.drop(checkpointId);if(archive)zip_discard(archive);}
    ZipAccess(ZipAccess const&)=delete;
    ZipAccess& operator=(ZipAccess const&)=delete;
    void add(uint32_t item,std::wstring name,uint64_t size,uint32_t crc) {
        if(!archive)return;
        std::replace(name.begin(),name.end(),L'\\',L'/');
        auto index=zip_name_locate(archive,utf8(name).c_str(),0);
        if(index<0)return;
        zip_stat_t stat;zip_stat_init(&stat);
        if(zip_stat_index(archive,static_cast<zip_uint64_t>(index),0,&stat))return;
        constexpr zip_uint64_t required=ZIP_STAT_SIZE|ZIP_STAT_COMP_SIZE|ZIP_STAT_COMP_METHOD|ZIP_STAT_CRC|ZIP_STAT_ENCRYPTION_METHOD;
        if((stat.valid&required)!=required||stat.size!=size||stat.crc!=crc||
           stat.encryption_method!=ZIP_EM_NONE||
           (stat.comp_method!=ZIP_CM_STORE&&stat.comp_method!=ZIP_CM_DEFLATE))return;
        if(stat.comp_method==ZIP_CM_STORE&&stat.comp_size!=stat.size)return;
        // Probe the raw data stream; use the general decoder if it is not seekable.
        File raw(archive,static_cast<zip_uint64_t>(index),ZIP_FL_COMPRESSED);
        if(!raw.value||zip_file_is_seekable(raw.value)!=1)return;
        entries.emplace(item,Entry{static_cast<zip_uint64_t>(index),stat.size,stat.comp_size,stat.crc,stat.comp_method,{}});
    }
    bool stored(uint32_t index) const {
        auto it=entries.find(index);return it!=entries.end()&&it->second.method==ZIP_CM_STORE;
    }
    bool deflated(uint32_t index) const {
        auto it=entries.find(index);return it!=entries.end()&&it->second.method==ZIP_CM_DEFLATE;
    }
    void readStored(uint32_t index,uint64_t offset,void* out,size_t count) {
        auto& e=entries.at(index);
        File raw(archive,e.zipIndex,ZIP_FL_COMPRESSED);
        if(!raw.value)throw std::runtime_error("Cannot open stored ZIP item");
        raw.seek(offset);
        size_t done=0;
        while(done<count) {
            auto n=raw.read(static_cast<unsigned char*>(out)+done,count-done);
            if(!n)throw std::runtime_error("Short stored ZIP item");
            done+=n;
        }
    }
    // Restart a raw DEFLATE stream at a cached block boundary using the 32 KiB
    // history dictionary, bit alignment, and rolling CRC. Only the requested
    // output window is retained; earlier output is never written to disk.
    void readDeflated(uint32_t index,uint64_t base,Page& page) {
        auto& e=entries.at(index);
        auto point=findPoint(index,e,base);
        File raw(archive,e.zipIndex,ZIP_FL_COMPRESSED);
        if(!raw.value)throw std::runtime_error("Cannot open raw DEFLATE stream");
        Decoder decoder;auto& z=decoder.stream;
        if(point.bits) {
            if(!point.compressed)throw std::runtime_error("Invalid DEFLATE checkpoint");
            unsigned char byte=0;
            raw.seek(point.compressed-1);
            if(raw.read(&byte,1)!=1||inflatePrime(&z,static_cast<int>(point.bits),byte>>(8-point.bits))!=Z_OK)
                throw std::runtime_error("Cannot restore DEFLATE bit alignment");
        }else raw.seek(point.compressed);
        if(point.dictionarySize&&inflateSetDictionary(&z,point.dictionary.data(),point.dictionarySize)!=Z_OK)
            throw std::runtime_error("Cannot restore DEFLATE history");
        std::array<unsigned char,65536> input{},output{};
        uint64_t in=point.compressed,out=point.logical,lastPoint=out;
        uint32_t crc=point.crc;
        const uint64_t end=base+page.data.size();
        uint64_t copied=0;
        bool ended=false;
        while(out<end||(end==e.size&&!ended)) {
            if(!z.avail_in&&in<e.compressedSize) {
                auto n=raw.read(input.data(),static_cast<size_t>(std::min<uint64_t>(input.size(),e.compressedSize-in)));
                if(!n)throw std::runtime_error("Truncated DEFLATE stream");
                in+=n;z.next_in=input.data();z.avail_in=static_cast<uInt>(n);
            }
            z.next_out=output.data();z.avail_out=static_cast<uInt>(output.size());
            auto before=z.avail_in;
            int result=inflate(&z,Z_BLOCK);
            auto produced=static_cast<size_t>(output.size()-z.avail_out);
            if(produced>e.size-out)throw std::runtime_error("Invalid DEFLATE output length");
            crc=static_cast<uint32_t>(crc32(crc,output.data(),static_cast<uInt>(produced)));
            decodedBytes.fetch_add(produced,std::memory_order_relaxed);
            auto next=out+produced;
            auto begin=std::max(base,out),finish=std::min(end,next);
            if(begin<finish) {
                auto n=static_cast<size_t>(finish-begin);
                memcpy(page.data.data()+static_cast<size_t>(begin-base),output.data()+static_cast<size_t>(begin-out),n);
                copied+=n;
            }
            out=next;
            if(result==Z_STREAM_END) {
                ended=true;
                if(out!=e.size||crc!=e.crc||in-z.avail_in!=e.compressedSize)
                    throw std::runtime_error("DEFLATE size or CRC verification failed");
                break;
            }
            if(result!=Z_OK)throw std::runtime_error("Invalid DEFLATE stream");
            bool boundary=(z.data_type&128)&&!(z.data_type&64);
            if(boundary&&out>0&&out-lastPoint>=CheckpointInterval) {
                savePoint(index,e,z,out,in-z.avail_in,crc);lastPoint=out;
            }
            if(!produced&&before==z.avail_in&&!(z.data_type&128))
                throw std::runtime_error("DEFLATE decoder made no progress");
        }
        if(copied!=page.data.size())throw std::runtime_error("Short DEFLATE output");
    }
};
}
