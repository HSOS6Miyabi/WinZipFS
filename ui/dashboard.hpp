#pragma once
#include "pch.h"
#include "shell.hpp"
#include <deque>

namespace wzui {
using namespace winrt;
using namespace Microsoft::UI::Xaml;
using namespace Microsoft::UI::Xaml::Controls;
using namespace Microsoft::UI::Xaml::Automation;
using namespace Microsoft::UI::Xaml::Media;

inline hstring hs(std::wstring_view s) { return hstring(s); }
inline std::wstring ws(hstring const& s) { return std::wstring(s.c_str(),s.size()); }
inline TextBlock text(std::wstring_view value,double size=14,bool title=false) {
    TextBlock t;t.Text(hs(value));t.FontSize(size);t.TextWrapping(TextWrapping::Wrap);
    if(title) t.FontWeight(Windows::UI::Text::FontWeight{600});
    return t;
}
template<class T> T identify(T value,std::wstring_view id) {
    AutomationProperties::SetAutomationId(value,hs(id));return value;
}
inline Button button(std::wstring_view title,std::wstring_view id) {
    auto b=identify(Button{},id);b.Content(box_value(hs(title)));return b;
}
inline TextBox number(std::wstring_view label,std::wstring_view id) {
    auto b=identify(TextBox{},id);b.Header(box_value(hs(label)));b.MinWidth(145);return b;
}
inline ComboBox combo(std::wstring_view label,std::wstring_view id,std::initializer_list<const wchar_t*> items) {
    auto b=identify(ComboBox{},id);b.Header(box_value(hs(label)));b.MinWidth(180);
    for(auto item:items) b.Items().Append(box_value(hstring(item)));
    return b;
}
inline ToggleSwitch toggle(std::wstring_view title,std::wstring_view id) {
    auto t=identify(ToggleSwitch{},id);t.Header(box_value(hs(title)));return t;
}
inline std::wstring coreError() {
    wchar_t b[512]{};wz_error(b,512);return *b?b:L"Operation failed";
}
inline int checkedNumber(TextBox const& b,int min,int max) {
    auto value=ws(b.Text());size_t end=0;int n=std::stoi(value,&end);
    if(end!=value.size()||n<min||n>max) throw std::runtime_error("Number outside allowed range");
    return n;
}

class Dashboard {
    Window window;
    HWND hwnd{},control{};
    UINT taskbarCreated{};
    NOTIFYICONDATAW tray{};
    DispatcherTimer timer;
    ListView list{nullptr};
    TextBlock summary{nullptr},details{nullptr},message{nullptr};
    ProgressRing progress{nullptr};
    Button mountBtn{nullptr},openBtn{nullptr},flushBtn{nullptr},ejectBtn{nullptr},applyBtn{nullptr},
           defaultsBtn{nullptr},saveDefaultsBtn{nullptr},registerBtn{nullptr},unregisterBtn{nullptr},
           systemDefaultsBtn{nullptr},recoveryBtn{nullptr},hideBtn{nullptr},exitBtn{nullptr};
    ToggleSwitch batch{nullptr},solid{nullptr};
    TextBox delay{nullptr},idle{nullptr},threads{nullptr};
    ComboBox level{nullptr},method{nullptr};
    WzOptions defaults=shell::loadDefaults();
    std::vector<WzVolume> rows;
    std::wstring selected;
    bool rebuilding=false,busy=false,quitting=false,noExplorer=false,trayReady=false;
    struct Job {std::function<std::wstring()> action;bool quiet=false,exit=false;};
    std::deque<Job> jobs;
    static constexpr UINT trayMessage=WM_APP+1;
    static constexpr int levels[]={0,1,3,5,7,9};

    static StackPanel vertical(double spacing=10) {StackPanel p;p.Spacing(spacing);return p;}
    static StackPanel horizontal(double spacing=8) {StackPanel p;p.Orientation(Orientation::Horizontal);p.Spacing(spacing);return p;}
    static Border card(UIElement const& content) {
        Border b;b.CornerRadius(CornerRadius{10,10,10,10});b.Padding(Thickness{18,16,18,16});
        try {
            auto resources=Application::Current().Resources();
            b.Background(resources.Lookup(box_value(L"CardBackgroundFillColorDefaultBrush")).as<Brush>());
            b.BorderBrush(resources.Lookup(box_value(L"CardStrokeColorDefaultBrush")).as<Brush>());
            b.BorderThickness(Thickness{1,1,1,1});
        }catch(...){}
        b.Child(content);return b;
    }
    void say(std::wstring_view value) {message.Text(hs(value));}
    WzVolume const* current() const {
        auto i=std::find_if(rows.begin(),rows.end(),[this](const auto& r){return selected==r.mount;});
        return i==rows.end()?nullptr:&*i;
    }
    static std::wstring rowText(WzVolume const& row) {
        auto name=std::filesystem::path(row.archive).filename().wstring();
        return std::wstring(row.mount)+L"   "+name+L"   ·   "+(row.readOnly?L"Read-only":L"Read/write")+
            (row.dirty?L"   ·   Unsaved changes":L"");
    }
    void load(WzOptions const& o) {
        batch.IsOn(o.batch!=0);solid.IsOn(o.solid!=0);
        delay.Text(hs(std::to_wstring(o.delaySeconds)));idle.Text(hs(std::to_wstring(o.idleMinutes)));
        threads.Text(hs(std::to_wstring(o.threads)));
        auto it=std::find(std::begin(levels),std::end(levels),o.level);
        level.SelectedIndex(it==std::end(levels)?3:int(it-std::begin(levels)));
        method.SelectedIndex(o.method>=0&&o.method<=4?o.method:0);
    }
    WzOptions readOptions() const {
        int l=level.SelectedIndex(),m=method.SelectedIndex();
        if(l<0||l>5||m<0||m>4) throw std::runtime_error("Select compression settings");
        return {batch.IsOn()?1:0,checkedNumber(delay,1,3600),checkedNumber(idle,0,1440),levels[l],m,
                checkedNumber(threads,0,256),solid.IsOn()?1:0};
    }
    void updateDetails() {
        auto r=current();
        std::wstring s=r?std::wstring(r->archive)+L"\n"+r->mount+L"   ·   Open handles: "+std::to_wstring(r->handles)+
                L"   ·   "+r->status+(r->dirty?L"   ·   Pending writeback":L"")
            :L"Select a mounted archive to manage its settings.";
        details.Text(hs(s));
        openBtn.IsEnabled(r!=nullptr);ejectBtn.IsEnabled(r!=nullptr);
        applyBtn.IsEnabled(r!=nullptr);flushBtn.IsEnabled(r&&!r->readOnly);
    }
    void refresh() {
        std::vector<WzVolume> updated;
        int n=wz_count();
        for(int i=0;i<n;++i) {WzVolume v{};if(wz_get(i,&v)) updated.push_back(v);}
        bool changed=rows.size()!=updated.size();
        if(!changed) for(size_t i=0;i<rows.size();++i) if(rowText(rows[i])!=rowText(updated[i])) {changed=true;break;}
        auto previous=selected;
        auto prior=current()?current()->options:defaults;
        rows=std::move(updated);
        if(changed) {
            rebuilding=true;
            list.Items().Clear();
            int index=-1;
            for(size_t i=0;i<rows.size();++i) {
                list.Items().Append(box_value(hs(rowText(rows[i]))));
                if(previous==rows[i].mount) index=int(i);
            }
            if(index<0&&!rows.empty()) index=0;
            list.SelectedIndex(index);selected=index<0?L"":rows[index].mount;
            rebuilding=false;
        }
        auto next=current()?current()->options:defaults;
        if(previous!=selected||prior.batch!=next.batch||prior.delaySeconds!=next.delaySeconds||
           prior.idleMinutes!=next.idleMinutes||prior.level!=next.level||prior.method!=next.method||
           prior.threads!=next.threads||prior.solid!=next.solid) load(next);
        summary.Text(hs(std::to_wstring(rows.size())+L" mounted  ·  "+
            std::to_wstring(std::count_if(rows.begin(),rows.end(),[](const auto& r){return r.dirty!=0;}))+L" pending writeback"));
        updateDetails();
    }
    fire_and_forget startNext() {
        if(busy||jobs.empty()||quitting) co_return;
        busy=true;
        auto job=std::move(jobs.front());jobs.pop_front();
        if(!job.quiet) {progress.IsActive(true);say(L"Working…");}
        apartment_context ui;
        co_await resume_background();
        std::wstring result;
        bool failed=false;
        try {result=job.action();if(result.starts_with(L"Error: ")) failed=true;}
        catch(hresult_error const& e) {result=L"Error: "+ws(e.message());failed=true;}
        catch(std::exception const&) {result=L"Error: Invalid operation or settings";failed=true;}
        catch(...) {result=L"Error: Unexpected operation failure";failed=true;}
        co_await ui;
        busy=false;progress.IsActive(false);
        if(!job.quiet||failed) say(result);
        if(failed&&!IsWindowVisible(hwnd)) show();
        if(job.exit&&!failed) {
            quitting=true;timer.Stop();
            Shell_NotifyIconW(NIM_DELETE,&tray);
            if(control) DestroyWindow(control);
            window.Close();
            Application::Current().Exit();
            co_return;
        }
        refresh();
        startNext();
    }
    void queue(std::function<std::wstring()> action,bool quiet=false,bool exit=false) {
        jobs.push_back({std::move(action),quiet,exit});startNext();
    }
    void mountPath(std::wstring path) {
        if(path.empty()) return;
        auto opt=defaults;bool explorer=!noExplorer;
        queue([path=std::move(path),opt,explorer] {
            wchar_t drive[16]{};
            if(!wz_mount(path.c_str(),&opt,drive,16)) return L"Error: "+coreError();
            std::wstring result=L"Mounted "+std::wstring(drive)+L" · "+std::filesystem::path(path).filename().wstring();
            if(explorer&&!shell::open(std::wstring(drive)+L"\\")) result+=L" (Explorer could not be opened)";
            return result;
        });
    }
    void browse() {
        try {auto p=shell::pick(hwnd);if(!p.empty())mountPath(std::move(p));}
        catch(hresult_error const& e){say(L"Error: "+ws(e.message()));}
    }
    void save(std::wstring drive,bool eject) {
        if(drive.empty()) return;
        queue([drive=std::move(drive),eject]{
            int ok=eject?wz_eject(drive.c_str()):wz_flush(drive.c_str());
            return ok?(eject?L"Ejected "+drive:L"Saved "+drive):L"Error: "+coreError();
        });
    }
    void apply() {
        if(selected.empty()) return;
        try {
            auto o=readOptions();auto drive=selected;
            queue([drive,o] {return wz_options(drive.c_str(),&o)?L"Settings updated for "+drive:L"Error: "+coreError();});
        }catch(...){say(L"Error: Check numeric ranges and compression settings.");}
    }
    void saveDefaults() {
        try {
            auto o=readOptions();
            if(o.method!=0) {say(L"Error: Default compression method must be Automatic.");return;}
            shell::saveDefaults(o);defaults=o;say(L"Defaults saved for future mounts.");
        }catch(...){say(L"Error: Check numeric ranges and compression settings.");}
    }
    void selectDrive(std::wstring drive) {
        show();
        for(size_t i=0;i<rows.size();++i) if(drive==rows[i].mount) {list.SelectedIndex(int(i));break;}
    }
    void quit() {
        queue([]{return wz_shutdown()?std::wstring(L"All archives saved"):L"Error: "+coreError();},false,true);
    }
    void show() {
        ShowWindow(hwnd,SW_RESTORE);window.Activate();SetForegroundWindow(hwnd);
    }
    void hide() {
        if(!trayReady) {say(L"Error: Notification area is unavailable. Keep this window open.");return;}
        ShowWindow(hwnd,SW_HIDE);
    }
    void openDrive(std::wstring drive) {
        if(drive.empty()) return;
        say(shell::open(drive+L"\\")?L"Opened "+drive:L"Error: Cannot open drive "+drive);
    }
    void trayMenu() {
        HMENU root=CreatePopupMenu();
        std::vector<std::function<void()>> commands;
        auto add=[&](HMENU menu,std::wstring label,std::function<void()> fn,UINT flags=MF_STRING) {
            commands.push_back(std::move(fn));AppendMenuW(menu,flags,commands.size(),label.c_str());
        };
        add(root,L"Open WinZipFS",[this]{show();});
        add(root,L"Mount archive…",[this]{show();browse();});
        AppendMenuW(root,MF_SEPARATOR,0,nullptr);
        if(rows.empty()) AppendMenuW(root,MF_STRING|MF_GRAYED,0,L"No mounted archives");
        for(auto const& r:rows) {
            auto drive=std::wstring(r.mount);
            auto opt=r.options;bool readOnly=r.readOnly!=0;
            HMENU sub=CreatePopupMenu();
            add(sub,L"Open drive",[this,drive]{openDrive(drive);});
            add(sub,L"Compression and cache settings",[this,drive]{selectDrive(drive);});
            add(sub,L"Batch write mode",[this,drive,opt]() mutable {
                opt.batch=!opt.batch;
                queue([drive,opt]{return wz_options(drive.c_str(),&opt)?L"Batch mode updated":L"Error: "+coreError();});
            },MF_STRING|(opt.batch?MF_CHECKED:MF_UNCHECKED)|(readOnly?MF_GRAYED:0));
            add(sub,L"Save changes",[this,drive]{save(drive,false);},MF_STRING|(readOnly?MF_GRAYED:0));
            add(sub,L"Eject",[this,drive]{save(drive,true);});
            auto title=drive+L"  "+std::filesystem::path(r.archive).filename().wstring();
            AppendMenuW(root,MF_POPUP,reinterpret_cast<UINT_PTR>(sub),title.c_str());
        }
        AppendMenuW(root,MF_SEPARATOR,0,nullptr);
        add(root,L"Exit",[this]{quit();});
        POINT p{};GetCursorPos(&p);SetForegroundWindow(control);
        auto command=TrackPopupMenu(root,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,control,nullptr);
        DestroyMenu(root);PostMessageW(control,WM_NULL,0,0);
        if(command>0&&command<=commands.size()) commands[command-1]();
    }
    void addTray() {
        tray={};tray.cbSize=sizeof(tray);tray.hWnd=control;tray.uID=1;
        tray.uFlags=NIF_ICON|NIF_MESSAGE|NIF_TIP;tray.uCallbackMessage=trayMessage;
        SHSTOCKICONINFO icon{sizeof(icon)};
        bool custom=SUCCEEDED(SHGetStockIconInfo(SIID_ZIPFILE,SHGSI_ICON|SHGSI_SMALLICON,&icon));
        tray.hIcon=custom?icon.hIcon:LoadIconW(nullptr,IDI_APPLICATION);
        wcscpy_s(tray.szTip,L"WinZipFS · Archive drives");
        trayReady=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE;
        if(custom) DestroyIcon(icon.hIcon);
        if(!trayReady) {ShowWindow(hwnd,SW_SHOW);say(L"Notification area is unavailable; the main window remains open.");}
    }
    static LRESULT CALLBACK windowProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR p) {
        auto self=reinterpret_cast<Dashboard*>(p);
        if(msg==WM_CLOSE&&!self->quitting) {self->hide();return 0;}
        return DefSubclassProc(h,msg,w,l);
    }
    static LRESULT CALLBACK controlProc(HWND h,UINT msg,WPARAM w,LPARAM l) {
        if(msg==WM_NCCREATE) {
            auto c=reinterpret_cast<CREATESTRUCTW*>(l);
            SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(c->lpCreateParams));
        }
        auto self=reinterpret_cast<Dashboard*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(self) try {
            if(msg==self->taskbarCreated){self->addTray();return 0;}
            if(msg==trayMessage) {
                if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)self->trayMenu();
                else if(l==WM_LBUTTONDBLCLK)self->show();
                return 0;
            }
            if(msg==WM_APP+3){self->show();return 0;}
            if(msg==WM_COPYDATA) {
                auto data=reinterpret_cast<COPYDATASTRUCT*>(l);
                if(!data||data->dwData!=1||!data->lpData||data->cbData<sizeof(wchar_t)||
                   data->cbData>65536||data->cbData%sizeof(wchar_t)) return FALSE;
                auto p=static_cast<const wchar_t*>(data->lpData);auto n=data->cbData/sizeof(wchar_t);
                if(p[n-1]!=0||wcsnlen_s(p,n)!=n-1)return FALSE;
                self->mountPath(p);return TRUE;
            }
        } catch(...) {self->say(L"Error: Shell operation failed.");return 0;}
        return DefWindowProcW(h,msg,w,l);
    }
    void build() {
        window.Title(L"WinZipFS");
        try {window.SystemBackdrop(MicaBackdrop());}catch(...){}
        auto page=vertical(16);
        page.Padding(Thickness{28,22,28,28});page.MaxWidth(960);
        page.Children().Append(text(L"Archive drives",32,true));
        page.Children().Append(text(L"Mount compressed files as Windows drives, edit files in place, and save changes when you are ready."));
        auto toolbar=horizontal();
        mountBtn=button(L"Mount archive…",L"Mount");
        hideBtn=button(L"Minimize to tray",L"Hide");
        exitBtn=button(L"Exit",L"Exit");
        toolbar.Children().Append(mountBtn);toolbar.Children().Append(hideBtn);toolbar.Children().Append(exitBtn);
        page.Children().Append(toolbar);
        auto activity=horizontal();
        progress=identify(ProgressRing{},L"Progress");progress.Width(20);progress.Height(20);progress.IsActive(false);
        AutomationProperties::SetName(progress,L"Archive operation progress");
        message=identify(text(L"Ready"),L"Status");
        activity.Children().Append(progress);activity.Children().Append(message);page.Children().Append(activity);
        auto volumes=vertical();
        volumes.Children().Append(text(L"Mounted archives",20,true));
        summary=identify(text(L"0 mounted"),L"Summary");volumes.Children().Append(summary);
        list=identify(ListView{},L"Volumes");list.SelectionMode(ListViewSelectionMode::Single);list.MinHeight(100);list.MaxHeight(230);
        volumes.Children().Append(list);
        details=identify(text(L"Select a mounted archive."),L"Details");volumes.Children().Append(details);
        auto actions=horizontal();
        openBtn=button(L"Open drive",L"Open");flushBtn=button(L"Save changes",L"Flush");ejectBtn=button(L"Eject",L"Eject");
        actions.Children().Append(openBtn);actions.Children().Append(flushBtn);actions.Children().Append(ejectBtn);
        volumes.Children().Append(actions);page.Children().Append(card(volumes));
        auto settings=vertical(12);
        settings.Children().Append(text(L"Compression and cache",20,true));
        settings.Children().Append(text(L"Apply settings to the selected archive, or save them as defaults for future mounts."));
        batch=toggle(L"Batch write mode",L"Batch");settings.Children().Append(batch);
        settings.Children().Append(text(L"Batch mode keeps modifications in the local cache until Save changes or Eject."));
        auto time=horizontal(12);
        delay=number(L"Writeback delay (seconds)",L"Delay");idle=number(L"Auto-eject (minutes, 0 = off)",L"Idle");
        time.Children().Append(delay);time.Children().Append(idle);settings.Children().Append(time);
        auto compression=horizontal(12);
        level=combo(L"Compression level",L"Level",{L"Store (0)",L"Fastest (1)",L"Fast (3)",L"Normal (5)",L"Maximum (7)",L"Ultra (9)"});
        method=combo(L"Compression method",L"Method",{L"Automatic",L"Store",L"Deflate (ZIP)",L"LZMA2 (7z)",L"BZip2 (ZIP / 7z)"});
        threads=number(L"Threads (0 = auto)",L"Threads");
        compression.Children().Append(level);compression.Children().Append(method);compression.Children().Append(threads);
        settings.Children().Append(compression);
        solid=toggle(L"Solid compression (7z)",L"Solid");settings.Children().Append(solid);
        auto buttons=horizontal();
        applyBtn=button(L"Apply to selected",L"Apply");defaultsBtn=button(L"Load defaults",L"LoadDefaults");
        saveDefaultsBtn=button(L"Save as defaults",L"SaveDefaults");
        buttons.Children().Append(applyBtn);buttons.Children().Append(defaultsBtn);buttons.Children().Append(saveDefaultsBtn);
        settings.Children().Append(buttons);page.Children().Append(card(settings));
        auto integrations=vertical();
        integrations.Children().Append(text(L"Windows integration",20,true));
        integrations.Children().Append(text(L"Register archive file types and choose WinZipFS as their default app in Windows Settings."));
        auto associations=horizontal();
        registerBtn=button(L"Register file types",L"Register");unregisterBtn=button(L"Remove registration",L"Unregister");
        systemDefaultsBtn=button(L"Default apps",L"DefaultApps");
        associations.Children().Append(registerBtn);associations.Children().Append(unregisterBtn);
        associations.Children().Append(systemDefaultsBtn);integrations.Children().Append(associations);
        recoveryBtn=button(L"Open recovery cache",L"Recovery");integrations.Children().Append(recoveryBtn);
        page.Children().Append(card(integrations));
        auto scroll=identify(ScrollViewer{},L"DashboardScroll");scroll.Content(page);scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Auto);window.Content(scroll);
        mountBtn.Click([this](auto&&,auto&&){browse();});
        hideBtn.Click([this](auto&&,auto&&){hide();});
        exitBtn.Click([this](auto&&,auto&&){quit();});
        list.SelectionChanged([this](auto&&,auto&&){
            if(rebuilding)return;int i=list.SelectedIndex();selected=i>=0&&i<int(rows.size())?rows[i].mount:L"";
            load(current()?current()->options:defaults);updateDetails();
        });
        openBtn.Click([this](auto&&,auto&&){openDrive(selected);});
        flushBtn.Click([this](auto&&,auto&&){save(selected,false);});
        ejectBtn.Click([this](auto&&,auto&&){save(selected,true);});
        applyBtn.Click([this](auto&&,auto&&){apply();});
        defaultsBtn.Click([this](auto&&,auto&&){load(defaults);say(L"Default settings loaded.");});
        saveDefaultsBtn.Click([this](auto&&,auto&&){saveDefaults();});
        registerBtn.Click([this](auto&&,auto&&){try{shell::registerAssociations();say(L"Archive file types registered.");}catch(...){say(L"Error: Cannot register file types.");}});
        unregisterBtn.Click([this](auto&&,auto&&){shell::unregisterAssociations();say(L"Archive file type registration removed.");});
        systemDefaultsBtn.Click([this](auto&&,auto&&){say(shell::open(L"ms-settings:defaultapps")?L"Opened Windows default apps.":L"Error: Windows default apps settings unavailable.");});
        recoveryBtn.Click([this](auto&&,auto&&){
            PWSTR p=nullptr;if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&p))) {
                std::filesystem::path path(p);CoTaskMemFree(p);path/=L"WinZipFS";path/=L"cache";
                std::error_code ec;std::filesystem::create_directories(path,ec);
                if(!ec&&shell::open(path.wstring())) say(L"Opened recovery cache.");else say(L"Error: Cannot open recovery cache.");
            } else say(L"Error: Cannot locate recovery cache.");
        });
        load(defaults);updateDetails();
    }
public:
    explicit Dashboard(std::vector<std::wstring> args) {
        build();
        winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
        auto dpi=GetDpiForWindow(hwnd);
        SetWindowPos(hwnd,nullptr,0,0,MulDiv(900,dpi,96),MulDiv(850,dpi,96),SWP_NOMOVE|SWP_NOZORDER);
        SetWindowSubclass(hwnd,windowProc,1,reinterpret_cast<DWORD_PTR>(this));
        WNDCLASSW wc{};wc.lpfnWndProc=controlProc;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=shell::controlClass;
        RegisterClassW(&wc);
        control=CreateWindowExW(WS_EX_TOOLWINDOW,shell::controlClass,L"WinZipFS control",WS_OVERLAPPED,
                 0,0,0,0,nullptr,nullptr,wc.hInstance,this);
        if(!control)throw std::runtime_error("Cannot create notification window");
        taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
        addTray();
        bool background=false;
        for(auto const& arg:args) {
            if(arg==L"--no-explorer")noExplorer=true;
            if(arg==L"--background")background=true;
        }
        window.Activate();if(background)hide();
        timer.Interval(std::chrono::seconds(2));
        timer.Tick([this](auto&&,auto&&){if(!trayReady&&!quitting)addTray();if(!busy&&jobs.empty()&&!quitting)queue([]{wz_tick();return std::wstring{};},true);});
        timer.Start();
        for(auto const& arg:args) {
            if(arg==L"--register") {try{shell::registerAssociations();}catch(...){say(L"Error: Cannot register archive types.");}}
            else if(arg==L"--unregister")shell::unregisterAssociations();
            else if(!arg.starts_with(L"--"))mountPath(arg);
        }
    }
};
}
using Dashboard=wzui::Dashboard;
