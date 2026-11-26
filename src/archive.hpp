#pragma once
#include <windows.h>
#include <bit7z/bitarchivereader.hpp>
#include <bit7z/biterror.hpp>
#include <bit7z/bitarchivewriter.hpp>
#include <bit7z/bitarchiveeditor.hpp>
#include <bcrypt.h>
#include <array>
#include <optional>
#include <bit7z/bitformat.hpp>
#include <bit7z/bitcompressionlevel.hpp>
#include <bit7z/bitcompressionmethod.hpp>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>
#include <cwctype>
#include <algorithm>
#include "api.h"

namespace wz {
namespace fs = std::filesystem;
using namespace bit7z;
inline std::string utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (!n) throw std::runtime_error("Invalid Unicode path");
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
inline std::wstring wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), nullptr, 0);
    if (!n) return L"Unknown error";
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), (int)s.size(), r.data(), n);
    return r;
}
inline std::string path8(const fs::path& p) { return utf8(p.wstring()); }
inline std::wstring lower(std::wstring s) { for(auto& c:s) c=(wchar_t)towlower(c); return s; }
inline bool ends(std::wstring_view s, std::wstring_view suffix) { return s.ends_with(suffix); }
struct CaseLess {
    bool operator()(const std::wstring& a,const std::wstring& b) const {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    }
};
inline bool same(const std::wstring& a,const std::wstring& b) {
    CaseLess cmp; return !cmp(a,b) && !cmp(b,a);
}
inline std::wstring safeName(std::wstring s,bool dir) {
    std::replace(s.begin(),s.end(),L'/',L'\\');
    while(s.starts_with(L".\\")) s.erase(0,2);
    if(!s.empty() && s.front()==L'\\') throw std::runtime_error("Invalid or absolute archive path");
    if(dir) while(!s.empty() && s.back()==L'\\') s.pop_back();
    else if(!s.empty() && s.back()==L'\\') throw std::runtime_error("Invalid file path");
    if(dir && (s==L"."||s.empty())) return {};
    if(s.empty() || s.front()==L'\\') throw std::runtime_error("Invalid or absolute archive path");
    size_t start=0;
    while(start<s.size()) {
        size_t end=s.find(L'\\',start); if(end==std::wstring::npos) end=s.size();
        auto part=s.substr(start,end-start);
        if(part.empty() || part==L"." || part==L".." || part.back()==L'.' || part.back()==L' ' ||
           part.find_first_of(L":<>\"|?*")!=std::wstring::npos || part.size()>255 ||
           std::any_of(part.begin(),part.end(),[](wchar_t c){return c<32;}))
            throw std::runtime_error("Archive contains an unsafe Windows file name");
        auto stem=lower(part.substr(0,part.find(L'.')));
        while(!stem.empty() && stem.back()==L' ') stem.pop_back();
        if(stem==L"con"||stem==L"prn"||stem==L"aux"||stem==L"nul"||stem==L"conin$"||stem==L"conout$"||
           (stem.size()==4 && (stem.starts_with(L"com")||stem.starts_with(L"lpt")) &&
            ((stem.back()>=L'0'&&stem.back()<=L'9')||stem.back()==L'\u00b9'||stem.back()==L'\u00b2'||stem.back()==L'\u00b3')))
            throw std::runtime_error("Archive contains a reserved Windows device name");
        start=end+1;
    }
    return s;
}
inline bool present(const BitPropVariant& v) {
    if(v.isEmpty()) return false;
    if(v.isBool()) return v.getBool();
    if(v.isString()) return !v.getString().empty();
    return true;
}
inline bool nonzero(const BitPropVariant& v) {
    if(v.isEmpty()) return false;
    if(v.isUInt64()) return v.getUInt64()!=0;
    if(v.isUInt32()) return v.getUInt32()!=0;
    if(v.isInt64()) return v.getInt64()!=0;
    if(v.isInt32()) return v.getInt32()!=0;
    return present(v);
}
inline bool archiveReadOnly(const BitArchiveReader& reader) {
    if(nonzero(reader.archiveProperty(BitProperty::ErrorFlags)))
        throw std::runtime_error("The archive contains structural errors");
    return reader.isMultiVolume() || present(reader.archiveProperty(BitProperty::Comment)) ||
           nonzero(reader.archiveProperty(BitProperty::Offset)) ||
           nonzero(reader.archiveProperty(BitProperty::TailSize)) ||
           nonzero(reader.archiveProperty(BitProperty::EmbeddedStubSize)) ||
           nonzero(reader.archiveProperty(BitProperty::WarningFlags));
}
struct PasswordRequired : std::runtime_error {
    PasswordRequired():std::runtime_error("A password is required or the password is incorrect") {}
};
inline bool validate(const BitArchiveReader& reader,const fs::path& root,bool allowEncrypted=false) {
    std::map<std::wstring,bool,CaseLess> names, prefixes;
    uint64_t total=0;
    bool metadataSensitive=false;
    if(reader.hasEncryptedItems()&&!allowEncrypted) throw PasswordRequired();
    for(const auto& item:reader.items()) {
        if(item.isSymLink() || (item.attributes() & FILE_ATTRIBUTE_REPARSE_POINT) ||
           present(reader.itemProperty(item.index(),BitProperty::HardLink)) ||
           present(reader.itemProperty(item.index(),BitProperty::NtReparse)) ||
           present(reader.itemProperty(item.index(),BitProperty::IsAltStream)) ||
           present(reader.itemProperty(item.index(),BitProperty::IsAnti)))
            throw std::runtime_error("Archive contains links, alternate streams or special entries");
        if(present(reader.itemProperty(item.index(),BitProperty::Comment)) ||
           present(reader.itemProperty(item.index(),BitProperty::NtSecure))) metadataSensitive=true;
        auto name=safeName(item.rawPath(),item.isDir());
        if(name.empty()) continue;
        auto [it,ok]=names.emplace(name,item.isDir());
        if(!ok) throw std::runtime_error("Archive contains duplicate or case-conflicting names");
        for(size_t i=name.find(L'\\');i!=std::wstring::npos;i=name.find(L'\\',i+1)) {
            auto prefix=name.substr(0,i);
            auto [p,inserted]=prefixes.emplace(prefix,true);
            if(!inserted && p->first!=prefix) throw std::runtime_error("Archive contains case-conflicting directories");
        }
        if(item.size()>UINT64_MAX-total) throw std::runtime_error("Archive is too large");
        total+=item.size();
    }
    for(const auto& [n,isDir]:names) {
        if(auto i=prefixes.find(n);i!=prefixes.end() && (!isDir || i->first!=n))
            throw std::runtime_error("Archive contains conflicting file and directory names");
    }
    if(total>fs::space(root).available) throw std::runtime_error("Insufficient cache disk space");
    return metadataSensitive;
}
inline void verifyTree(const fs::path& root) {
    for(const auto& e:fs::recursive_directory_iterator(root)) {
        DWORD attr=GetFileAttributesW(e.path().c_str());
        if(attr==INVALID_FILE_ATTRIBUTES || (attr&FILE_ATTRIBUTE_REPARSE_POINT))
            throw std::runtime_error("Reparse points cannot be mounted");
        if(!e.is_directory()&&!e.is_regular_file()) throw std::runtime_error("Unsupported cache entry");
        safeName(e.path().lexically_relative(root).wstring(),e.is_directory());
    }
}
struct Format {
    const BitInOutFormat* inner=nullptr;
    const BitInOutFormat* outer=nullptr;
};
inline Format formatFor(const fs::path& path,const BitInFormat& detected) {
    auto n=lower(path.filename().wstring());
    if(ends(n,L".7z")&&detected==BitFormat::SevenZip) return {&BitFormat::SevenZip};
    if((ends(n,L".zip")||ends(n,L".zipx"))&&detected==BitFormat::Zip) return {&BitFormat::Zip};
    if(ends(n,L".tar")&&detected==BitFormat::Tar) return {&BitFormat::Tar};
    if((ends(n,L".tar.gz")||ends(n,L".tgz"))&&detected==BitFormat::GZip) return {&BitFormat::Tar,&BitFormat::GZip};
    if((ends(n,L".tar.bz2")||ends(n,L".tbz")||ends(n,L".tbz2"))&&detected==BitFormat::BZip2) return {&BitFormat::Tar,&BitFormat::BZip2};
    if((ends(n,L".tar.xz")||ends(n,L".txz"))&&detected==BitFormat::Xz) return {&BitFormat::Tar,&BitFormat::Xz};
    return {};
}
inline bool isWritable(const fs::path& path) {
    HANDLE h=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                         nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(h==INVALID_HANDLE_VALUE) return false;
    CloseHandle(h); return true;
}
class TemporaryFile {
    fs::path path_;
public:
    explicit TemporaryFile(fs::path path):path_(std::move(path)) {}
    TemporaryFile(const TemporaryFile&)=delete;
    TemporaryFile& operator=(const TemporaryFile&)=delete;
    ~TemporaryFile() {std::error_code ec;fs::remove(path_,ec);}
    const fs::path& path() const {return path_;}
};
inline void extractStreamItem(const BitArchiveReader& outer,const fs::path& file) {
    if(outer.itemsCount()!=1||outer.isItemFolder(0)||outer.hasEncryptedItems())
        throw std::runtime_error("Invalid single-stream archive");
    if(outer.itemProperty(0,BitProperty::Size).isUInt64() &&
        outer.itemProperty(0,BitProperty::Size).getUInt64()>fs::space(file.parent_path()).available)
        throw std::runtime_error("Insufficient disk space for decompressed stream");
    std::ofstream output(file,std::ios::binary|std::ios::trunc);
    if(!output) throw std::runtime_error("Cannot create decompressed file");
    output.exceptions(std::ios::badbit|std::ios::failbit);
    outer.extractTo(output,0);
    output.close();
}
inline bool singleStream(const BitInFormat& format) {
    return format==BitFormat::GZip||format==BitFormat::BZip2||format==BitFormat::Xz||
           format==BitFormat::Zstd||format==BitFormat::Z||format==BitFormat::Lzma||
           format==BitFormat::Lzma86||format==BitFormat::Ppmd;
}
inline void extractStandalone(const BitArchiveReader& reader,const fs::path& source,const fs::path& root) {
    if(reader.itemsCount()!=1||reader.isItemFolder(0))
        throw std::runtime_error("Invalid single-stream archive");
    auto item=reader.items().front();
    if(item.isSymLink()||item.isEncrypted()) throw std::runtime_error("Unsupported single-stream entry");
    auto name=item.rawPath();
    if(name.empty()) name=source.stem().wstring();
    name=safeName(name,false);
    auto output=root/fs::path(name);
    fs::create_directories(output.parent_path());
    extractStreamItem(reader,output);
    auto modified=reader.itemProperty(0,BitProperty::MTime);
    if(modified.isFileTime()) {
        HANDLE h=CreateFileW(output.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                             nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(h!=INVALID_HANDLE_VALUE) {
            auto value=modified.getFileTime();
            SetFileTime(h,nullptr,nullptr,&value);CloseHandle(h);
        }
    }
}
inline Format extract(const Bit7zLibrary& lib,const fs::path& source,const fs::path& root,const std::string& password={}) {
    Format fmt;
    try {
        BitArchiveReader outer(lib,path8(source),BitFormat::Auto,password);
        if(outer.hasEncryptedItems()&&password.empty()) throw PasswordRequired();
        fmt=formatFor(source,outer.detectedFormat());
        auto name=lower(source.filename().wstring());
        bool zstdTar=(ends(name,L".tar.zst")||ends(name,L".tzst"))&&outer.detectedFormat()==BitFormat::Zstd;
        if(fmt.outer||zstdTar) {
            bool outerMetadata=archiveReadOnly(outer);
            TemporaryFile tar(root.parent_path()/L"extracted.tar");
            extractStreamItem(outer,tar.path());
            BitArchiveReader inner(lib,path8(tar.path()),BitFormat::Tar);
            if(validate(inner,root)||archiveReadOnly(inner)||outerMetadata||zstdTar) fmt={};
            if(inner.itemsCount()) inner.extractTo(path8(root));
        } else if(singleStream(outer.detectedFormat())&&outer.itemsCount()==1) {
            fmt={};
            extractStandalone(outer,source,root);
        } else {
            if(validate(outer,root,!password.empty()) || archiveReadOnly(outer))
                fmt={};
            if(outer.itemsCount()) outer.extractTo(path8(root));
        }
    } catch(const BitException& e) {
        if(e.code()==BitFailureSource::WrongPassword ||
           (password.empty()&&BitArchiveReader::isHeaderEncrypted(lib,path8(source),BitFormat::Auto)))
            throw PasswordRequired();
        throw;
    }
    verifyTree(root);
    if(!password.empty()||!isWritable(source)) fmt={};
    return fmt;
}
inline void configure(BitArchiveWriter& writer,const BitInOutFormat& fmt,const WzOptions& o) {
    if(fmt.hasFeature(FormatFeatures::CompressionLevel))
        writer.setCompressionLevel(static_cast<BitCompressionLevel>(o.level));
    if(o.threads>0 && fmt!=BitFormat::Tar) writer.setThreadsCount(o.threads);
    if(fmt==BitFormat::SevenZip) writer.setSolidMode(o.solid!=0);
    if(o.method && fmt.hasFeature(FormatFeatures::MultipleMethods)) {
        BitCompressionMethod m=BitCompressionMethod::Copy;
        switch(o.method) {
            case 2: m=BitCompressionMethod::Deflate;break;
            case 3: m=BitCompressionMethod::Lzma2;break;
            case 4: m=BitCompressionMethod::BZip2;break;
        }
        writer.setCompressionMethod(m);
    }
}
struct CacheEntry {
    bool directory=false;
    uint64_t size=0,created=0,modified=0;
    DWORD attributes=0;
    std::array<BYTE,32> digest{};
    bool operator==(const CacheEntry&) const =default;
};
using CacheManifest=std::map<std::wstring,CacheEntry,CaseLess>;
inline uint64_t timeValue(FILETIME t) {
    return (uint64_t(t.dwHighDateTime)<<32)|t.dwLowDateTime;
}
class CacheHasher {
    BCRYPT_ALG_HANDLE algorithm=nullptr;
    DWORD objectSize=0;
    std::vector<BYTE> object,buffer;
    static void check(NTSTATUS status) {
        if(status<0)throw std::runtime_error("Cannot hash cached data");
    }
public:
    CacheHasher() {
        check(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0));
        try {
            DWORD returned=0;
            check(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&returned,0));
            object.resize(objectSize);buffer.resize(1024*1024);
        }catch(...){BCryptCloseAlgorithmProvider(algorithm,0);throw;}
    }
    ~CacheHasher(){if(algorithm)BCryptCloseAlgorithmProvider(algorithm,0);}
    CacheHasher(const CacheHasher&)=delete;
    CacheHasher& operator=(const CacheHasher&)=delete;
    std::array<BYTE,32> hash(HANDLE file) {
        BCRYPT_HASH_HANDLE h=nullptr;
        check(BCryptCreateHash(algorithm,&h,object.data(),objectSize,nullptr,0,0));
        try {
            DWORD count=0;
            do {
                if(!ReadFile(file,buffer.data(),static_cast<DWORD>(buffer.size()),&count,nullptr))
                    throw std::runtime_error("Cannot read cached file for verification");
                if(count) check(BCryptHashData(h,buffer.data(),count,0));
            }while(count);
            std::array<BYTE,32> result{};
            check(BCryptFinishHash(h,result.data(),static_cast<ULONG>(result.size()),0));
            BCryptDestroyHash(h);return result;
        }catch(...){BCryptDestroyHash(h);throw;}
    }
};
inline CacheManifest cacheManifest(const fs::path& root) {
    verifyTree(root);
    CacheManifest result;
    CacheHasher hasher;
    for(auto const& entry:fs::recursive_directory_iterator(root)) {
        auto name=safeName(entry.path().lexically_relative(root).wstring(),entry.is_directory());
        HANDLE h=CreateFileW(entry.path().c_str(),entry.is_directory()?FILE_READ_ATTRIBUTES:GENERIC_READ,
            FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
        if(h==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot inspect cached file");
        CacheEntry item;
        try {
            BY_HANDLE_FILE_INFORMATION info{};
            if(!GetFileInformationByHandle(h,&info)||(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT))
                throw std::runtime_error("Cannot inspect cached file attributes");
            item.directory=(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)!=0;
            item.size=item.directory?0:(uint64_t(info.nFileSizeHigh)<<32)|info.nFileSizeLow;
            item.attributes=info.dwFileAttributes&(FILE_ATTRIBUTE_READONLY|FILE_ATTRIBUTE_HIDDEN|FILE_ATTRIBUTE_SYSTEM|FILE_ATTRIBUTE_ARCHIVE);
            item.created=timeValue(info.ftCreationTime);item.modified=timeValue(info.ftLastWriteTime);
            if(!item.directory)item.digest=hasher.hash(h);
            CloseHandle(h);
        }catch(...){CloseHandle(h);throw;}
        if(!result.emplace(std::move(name),item).second)throw std::runtime_error("Conflicting cached file names");
    }
    return result;
}
inline bool incrementalFormat(Format format) {
    return !format.outer&&(format.inner==&BitFormat::Zip||format.inner==&BitFormat::SevenZip);
}
inline bool underPath(std::wstring_view name,std::wstring_view parent) {
    return name.size()>=parent.size() &&
           CompareStringOrdinal(name.data(),static_cast<int>(parent.size()),parent.data(),static_cast<int>(parent.size()),TRUE)==CSTR_EQUAL &&
           (name.size()==parent.size()||name[parent.size()]==L'\\');
}
inline void verifyManifest(const BitArchiveReader& reader,const fs::path& root) {
    struct Entry { bool directory; uint64_t size; bool seen=false; };
    std::map<std::wstring,Entry,CaseLess> expected;
    for(const auto& file:fs::recursive_directory_iterator(root)) {
        auto name=safeName(file.path().lexically_relative(root).wstring(),file.is_directory());
        auto [it,inserted]=expected.emplace(name,Entry{file.is_directory(),file.is_directory()?0:file.file_size()});
        if(!inserted) throw std::runtime_error("Conflicting cache names");
    }
    std::set<std::wstring,CaseLess> explicitNames;
    for(const auto& item:reader.items()) {
        auto name=safeName(item.rawPath(),item.isDir());
        if(name.empty()) continue;
        if(!explicitNames.insert(name).second) throw std::runtime_error("Duplicate entry in generated archive");
        auto i=expected.find(name);
        if(i==expected.end()||i->first!=name||i->second.directory!=item.isDir()||
           (!item.isDir()&&i->second.size!=item.size()))
            throw std::runtime_error("Generated archive does not match the cache: "+utf8(name));
        i->second.seen=true;
        for(size_t pos=name.find(L'\\');pos!=std::wstring::npos;pos=name.find(L'\\',pos+1)) {
            auto parent=expected.find(name.substr(0,pos));
            if(parent==expected.end()||!parent->second.directory)
                throw std::runtime_error("Invalid generated directory path");
            parent->second.seen=true;
        }
    }
    for(const auto& [name,entry]:expected) if(!entry.seen)
        throw std::runtime_error("Generated archive is missing "+utf8(name));
}
inline void packIncremental(const Bit7zLibrary& lib,const fs::path& original,const fs::path& root,
                            const fs::path& output,Format fmt,const WzOptions& opt,
                            const CacheManifest& previous,const CacheManifest& current) {
    struct OldEntry {uint32_t index;bool directory;};
    std::map<std::wstring,OldEntry,CaseLess> old;
    {
        BitArchiveReader reader(lib,path8(original),*fmt.inner);
        for(const auto& item:reader.items()) {
            auto name=safeName(item.rawPath(),item.isDir());
            if(!name.empty())old.emplace(std::move(name),OldEntry{item.index(),item.isDir()});
        }
    }
    // Add a complete subtree only when its directory did not have an explicit
    // entry and must now be materialized (new/renamed directories or empty dirs).
    std::vector<std::wstring> subtrees;
    auto covered=[&](std::wstring_view name) {
        return std::any_of(subtrees.begin(),subtrees.end(),[&](auto const& parent){return underPath(name,parent);});
    };
    for(auto const& [name,item]:current) {
        if(!item.directory||covered(name))continue;
        auto oldItem=old.find(name);
        if(oldItem!=old.end()&&oldItem->first==name&&oldItem->second.directory)continue;
        auto prior=previous.find(name);
        auto next=current.upper_bound(name);
        bool hasChildren=next!=current.end()&&underPath(next->first,name);
        if(prior==previous.end()||prior->first!=name||prior->second!=item||!hasChildren)
            subtrees.push_back(name);
    }
    {
        BitArchiveEditor editor(lib,path8(original),*fmt.inner);
        configure(editor,*fmt.inner,opt);
        for(auto const& [name,entry]:old) {
            auto now=current.find(name);
            if(covered(name)||now==current.end()||now->first!=name||now->second.directory!=entry.directory) {
                editor.deleteItem(entry.index,DeletePolicy::ItemOnly);
                continue;
            }
            auto prior=previous.find(name);
            if(prior==previous.end()||prior->first!=name||prior->second!=now->second)
                editor.updateItem(entry.index,path8(root/fs::path(name)));
        }
        for(auto const& name:subtrees)
            editor.addItems(std::vector<std::pair<tstring,tstring>>{{path8(root/fs::path(name)),utf8(name)}});
        for(auto const& [name,item]:current) {
            if(item.directory||covered(name))continue;
            auto prior=old.find(name);
            if(prior==old.end()||prior->first!=name||prior->second.directory)
                editor.addFile(path8(root/fs::path(name)),utf8(name));
        }
        editor.compressTo(path8(output));
    }
    BitArchiveReader verified(lib,path8(output),*fmt.inner);
    if(verified.itemsCount())verified.test();
    verifyManifest(verified,root);
}
inline void pack(const Bit7zLibrary& lib,const fs::path& root,const fs::path& output,const fs::path& work,Format fmt,const WzOptions& opt) {
    if(!fmt.inner) throw std::runtime_error("Read-only archive format");
    verifyTree(root);
    auto target=fmt.outer?work:output;
    {
        BitArchiveWriter writer(lib,*fmt.inner);
        configure(writer,*fmt.inner,opt);
        if(!fs::is_empty(root)) writer.addDirectoryContents(path8(root));
        writer.compressTo(path8(target));
    }
    if(fmt.outer) {
        BitArchiveWriter writer(lib,*fmt.outer);
        configure(writer,*fmt.outer,opt);
        writer.addFile(path8(target),"archive.tar");
        writer.compressTo(path8(output));
    }
    BitArchiveReader reader(lib,path8(output),fmt.outer?static_cast<const BitInFormat&>(*fmt.outer):*fmt.inner);
    if(reader.itemsCount()) reader.test();
    if(fmt.outer) {
        TemporaryFile tar(work.parent_path()/L"verified.tar");
        extractStreamItem(reader,tar.path());
        BitArchiveReader inner(lib,path8(tar.path()),BitFormat::Tar);
        if(inner.itemsCount()) inner.test();
        verifyManifest(inner,root);
    } else {
        verifyManifest(reader,root);
    }
}
}
