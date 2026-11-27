#pragma once
#include "pch.h"
namespace shell {
inline constexpr wchar_t registry[]=L"Software\\WinZipFS";
inline constexpr wchar_t progId[]=L"WinZipFS.Archive";
inline constexpr wchar_t controlClass[]=L"WinZipFS.Control";
inline std::wstring exe() {
    wchar_t name[32768]{};GetModuleFileNameW(nullptr,name,32768);return name;
}
inline void regString(std::wstring key,const wchar_t* name,std::wstring value) {
    HKEY h{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,key.c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&h,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot create registry key");
    LSTATUS r=RegSetValueExW(h,name,0,REG_SZ,reinterpret_cast<const BYTE*>(value.c_str()),DWORD((value.size()+1)*sizeof(wchar_t)));
    RegCloseKey(h);if(r!=ERROR_SUCCESS) throw std::runtime_error("Cannot write registry value");
}
inline std::vector<std::wstring> extensions() {
    std::vector<std::wstring> values;
    std::wstring s=wz_extensions();
    for(size_t p=0;p<s.size();) {
        auto end=s.find(L';',p);if(end==std::wstring::npos) end=s.size();
        values.push_back(s.substr(p,end-p));p=end+1;
    }
    return values;
}
inline void registerAssociations() {
    const std::wstring classes=L"Software\\Classes\\";
    const std::wstring cap=std::wstring(registry)+L"\\Capabilities";
    regString(classes+progId,nullptr,L"WinZipFS archive");
    regString(classes+progId+L"\\DefaultIcon",nullptr,L"\""+exe()+L"\",0");
    regString(classes+progId+L"\\shell\\open\\command",nullptr,L"\""+exe()+L"\" \"%1\"");
    regString(cap,L"ApplicationName",L"WinZipFS");
    regString(cap,L"ApplicationDescription",L"Mount archives as Windows drives");
    regString(L"Software\\RegisteredApplications",L"WinZipFS",cap);
    for(const auto& e:extensions()) {
        regString(cap+L"\\FileAssociations",e.c_str(),progId);
        regString(classes+e+L"\\OpenWithProgids",progId,L"");
        wchar_t value[512]{};DWORD size=sizeof(value);
        if(RegGetValueW(HKEY_CLASSES_ROOT,e.c_str(),nullptr,RRF_RT_REG_SZ,nullptr,value,&size)!=ERROR_SUCCESS || !*value)
            regString(classes+e,nullptr,progId);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
}
inline void unregisterAssociations() {
    const std::wstring classes=L"Software\\Classes\\";
    for(const auto& e:extensions()) {
        HKEY h{};
        if(RegOpenKeyExW(HKEY_CURRENT_USER,(classes+e+L"\\OpenWithProgids").c_str(),0,KEY_SET_VALUE,&h)==ERROR_SUCCESS) {
            RegDeleteValueW(h,progId);RegCloseKey(h);
        }
        if(RegOpenKeyExW(HKEY_CURRENT_USER,(classes+e).c_str(),0,KEY_QUERY_VALUE|KEY_SET_VALUE,&h)==ERROR_SUCCESS) {
            wchar_t current[512]{};DWORD size=sizeof(current),type=0;
            if(RegQueryValueExW(h,nullptr,nullptr,&type,reinterpret_cast<BYTE*>(current),&size)==ERROR_SUCCESS &&
                type==REG_SZ && _wcsicmp(current,progId)==0) RegDeleteValueW(h,nullptr);
            RegCloseKey(h);
        }
    }
    RegDeleteTreeW(HKEY_CURRENT_USER,(classes+progId).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER,(std::wstring(registry)+L"\\Capabilities").c_str());
    HKEY h{};
    if(RegOpenKeyExW(HKEY_CURRENT_USER,L"Software\\RegisteredApplications",0,KEY_SET_VALUE,&h)==ERROR_SUCCESS) {
        RegDeleteValueW(h,L"WinZipFS");RegCloseKey(h);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED,SHCNF_IDLIST,nullptr,nullptr);
}
inline WzOptions loadDefaults() {
    WzOptions o{};wz_default_options(&o);WzOptions saved{};DWORD size=sizeof(saved);
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"Options",RRF_RT_REG_BINARY,nullptr,&saved,&size)==ERROR_SUCCESS&&size==sizeof(saved)&&
       saved.delaySeconds>=1&&saved.delaySeconds<=3600&&saved.idleMinutes>=0&&saved.idleMinutes<=1440&&
       saved.threads>=0&&saved.threads<=256&&saved.level>=0&&saved.level<=9) o=saved;
    return o;
}
inline void saveDefaults(WzOptions o) {
    HKEY h{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&h,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot save settings");
    auto r=RegSetValueExW(h,L"Options",0,REG_BINARY,reinterpret_cast<const BYTE*>(&o),sizeof(o));RegCloseKey(h);
    if(r!=ERROR_SUCCESS) throw std::runtime_error("Cannot save settings");
}
inline unsigned loadMemoryLimit() {
    DWORD value=0,size=sizeof(value);
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"MemoryMiB",RRF_RT_REG_DWORD,nullptr,&value,&size)==ERROR_SUCCESS&&value>=16&&value<=65536)
        return value;
    return 512;
}
inline void saveMemoryLimit(unsigned megabytes) {
    HKEY h{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&h,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot save memory settings");
    DWORD value=megabytes;
    auto result=RegSetValueExW(h,L"MemoryMiB",0,REG_DWORD,reinterpret_cast<BYTE*>(&value),sizeof(value));
    RegCloseKey(h);
    if(result!=ERROR_SUCCESS)throw std::runtime_error("Cannot save memory settings");
}
struct UncSettings {unsigned seconds=30;bool prefer=false;};
inline UncSettings loadUncSettings() {
    UncSettings value;
    DWORD data=0,size=sizeof(data);
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"UncCacheSeconds",RRF_RT_REG_DWORD,nullptr,&data,&size)==ERROR_SUCCESS&&data<=86400)
        value.seconds=data;
    size=sizeof(data);
    if(RegGetValueW(HKEY_CURRENT_USER,registry,L"PreferUnc",RRF_RT_REG_DWORD,nullptr,&data,&size)==ERROR_SUCCESS)
        value.prefer=data!=0;
    return value;
}
inline void saveUncSettings(UncSettings value) {
    HKEY h{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,registry,0,nullptr,0,KEY_SET_VALUE,nullptr,&h,nullptr)!=ERROR_SUCCESS)
        throw std::runtime_error("Cannot save UNC settings");
    DWORD seconds=value.seconds,prefer=value.prefer;
    auto r=RegSetValueExW(h,L"UncCacheSeconds",0,REG_DWORD,reinterpret_cast<BYTE*>(&seconds),sizeof(seconds));
    auto r2=RegSetValueExW(h,L"PreferUnc",0,REG_DWORD,reinterpret_cast<BYTE*>(&prefer),sizeof(prefer));
    RegCloseKey(h);
    if(r!=ERROR_SUCCESS||r2!=ERROR_SUCCESS)throw std::runtime_error("Cannot save UNC settings");
}
inline void clipboard(HWND owner,std::wstring_view text) {
    auto mem=GlobalAlloc(GMEM_MOVEABLE,(text.size()+1)*sizeof(wchar_t));
    if(!mem)throw std::runtime_error("Cannot allocate clipboard text");
    auto dest=static_cast<wchar_t*>(GlobalLock(mem));
    if(!dest){GlobalFree(mem);throw std::runtime_error("Cannot lock clipboard text");}
    std::copy(text.begin(),text.end(),dest);dest[text.size()]=0;GlobalUnlock(mem);
    if(!OpenClipboard(owner)){GlobalFree(mem);throw std::runtime_error("Cannot open clipboard");}
    bool ok=EmptyClipboard()&&SetClipboardData(CF_UNICODETEXT,mem);
    CloseClipboard();
    if(!ok){GlobalFree(mem);throw std::runtime_error("Cannot copy UNC path");}
}
inline std::vector<std::wstring> arguments() {
    int n=0;LPWSTR* args=CommandLineToArgvW(GetCommandLineW(),&n);std::vector<std::wstring> values;
    if(args) {for(int i=1;i<n;++i) values.emplace_back(args[i]);LocalFree(args);}
    return values;
}
inline bool forward(const std::vector<std::wstring>& args) {
    HWND target=nullptr;
    for(int i=0;i<100 && !target;++i) {
        target=FindWindowW(controlClass,nullptr);
        if(!target) Sleep(100);
    }
    if(!target) return false;
    if(args.empty()) {PostMessageW(target,WM_APP+3,0,0);return true;}
    ULONG_PTR mode=1;
    for(const auto& a:args) {
        if(a==L"--unc") {mode=2;continue;}
        if(a==L"--drive") {mode=3;continue;}
        if(a.starts_with(L"--")) continue;
        COPYDATASTRUCT data{mode,DWORD((a.size()+1)*sizeof(wchar_t)),const_cast<wchar_t*>(a.c_str())};
        DWORD_PTR result=0;
        if(!SendMessageTimeoutW(target,WM_COPYDATA,0,reinterpret_cast<LPARAM>(&data),SMTO_ABORTIFHUNG|SMTO_BLOCK,10000,&result)||!result) return false;
    }
    return true;
}
inline bool open(std::wstring const& path) {
    HRESULT apartment=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);
    auto result=ShellExecuteW(nullptr,L"open",path.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    if(SUCCEEDED(apartment)) CoUninitialize();
    return reinterpret_cast<INT_PTR>(result)>32;
}
inline std::wstring pickSave(HWND owner,std::wstring const& suggested) {
    winrt::com_ptr<IFileSaveDialog> dialog;
    winrt::check_hresult(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put())));
    FILEOPENDIALOGOPTIONS options{};
    winrt::check_hresult(dialog->GetOptions(&options));
    winrt::check_hresult(dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_PATHMUSTEXIST|FOS_OVERWRITEPROMPT));
    COMDLG_FILTERSPEC filters[]={{L"ZIP archive",L"*.zip"},{L"7-Zip archive",L"*.7z"}};
    winrt::check_hresult(dialog->SetFileTypes(2,filters));
    winrt::check_hresult(dialog->SetDefaultExtension(L"zip"));
    winrt::check_hresult(dialog->SetTitle(L"WinZipFS - Save archive copy"));
    winrt::check_hresult(dialog->SetFileName(suggested.c_str()));
    auto result=dialog->Show(owner);
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    winrt::check_hresult(result);
    winrt::com_ptr<IShellItem> item;
    winrt::check_hresult(dialog->GetResult(item.put()));
    PWSTR name=nullptr;winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH,&name));
    std::wstring value(name);CoTaskMemFree(name);return value;
}
inline std::wstring pick(HWND owner) {
    winrt::com_ptr<IFileOpenDialog> dialog;
    winrt::check_hresult(CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(dialog.put())));
    FILEOPENDIALOGOPTIONS options{};dialog->GetOptions(&options);
    dialog->SetOptions(options|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST);
    std::wstring pattern;
    for(auto const& e:extensions()) {if(!pattern.empty()) pattern+=L";";pattern+=L"*"+e;}
    COMDLG_FILTERSPEC filters[]={{L"Archive files",pattern.c_str()},{L"All files",L"*.*"}};
    winrt::check_hresult(dialog->SetTitle(L"WinZipFS - Mount archive"));
    winrt::check_hresult(dialog->SetFileTypes(2,filters));
    auto result=dialog->Show(owner);
    if(result==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    winrt::check_hresult(result);
    winrt::com_ptr<IShellItem> item;winrt::check_hresult(dialog->GetResult(item.put()));
    PWSTR name=nullptr;winrt::check_hresult(item->GetDisplayName(SIGDN_FILESYSPATH,&name));
    std::wstring value(name);CoTaskMemFree(name);return value;
}
}
