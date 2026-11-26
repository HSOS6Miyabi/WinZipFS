#pragma once
#include <windows.h>
#include <bit7z/bitarchivereader.hpp>
#include <bit7z/bitarchivewriter.hpp>
#include <bit7z/bitformat.hpp>
#include <bit7z/bitcompressionlevel.hpp>
#include <bit7z/bitcompressionmethod.hpp>
#include <filesystem>
#include <fstream>
#include <map>
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
    if(dir) while(!s.empty() && s.back()==L'\\') s.pop_back();
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
inline bool validate(const BitArchiveReader& reader,const fs::path& root) {
    std::map<std::wstring,bool,CaseLess> names, prefixes;
    uint64_t total=0;
    bool metadataSensitive=false;
    if(reader.hasEncryptedItems()) throw std::runtime_error("Encrypted archives require a password");
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
inline Format extract(const Bit7zLibrary& lib,const fs::path& source,const fs::path& root) {
    BitArchiveReader outer(lib,path8(source),BitFormat::Auto);
    auto fmt=formatFor(source,outer.detectedFormat());
    if(fmt.outer) {
        if(outer.itemsCount()!=1 || outer.isItemFolder(0)) throw std::runtime_error("Invalid compressed TAR");
        validate(outer,root);
        BitArchiveReader inner(lib,outer,0,BitFormat::Tar);
        if(validate(inner,root)) fmt={};
        if(inner.itemsCount()) inner.extractTo(path8(root));
    } else {
        if(validate(outer,root) || outer.isMultiVolume() || present(outer.archiveProperty(BitProperty::Comment)))
            fmt={};
        if(outer.itemsCount()) outer.extractTo(path8(root));
    }
    verifyTree(root);
    if(!isWritable(source)) fmt={};
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
    reader.test();
    if(fmt.outer) { BitArchiveReader inner(lib,reader,0,BitFormat::Tar); inner.test(); }
}
}
