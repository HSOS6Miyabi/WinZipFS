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
    auto b=identify(Button{},id);
    auto caption=text(title);caption.TextAlignment(TextAlignment::Center);
    b.Content(caption);b.HorizontalAlignment(HorizontalAlignment::Stretch);
    AutomationProperties::SetName(b,hs(title));return b;
}
inline TextBox number(std::wstring_view label,std::wstring_view id) {
    auto b=identify(TextBox{},id);b.Header(text(label));b.MinWidth(145);
    AutomationProperties::SetName(b,hs(label));return b;
}
inline ComboBox combo(std::wstring_view label,std::wstring_view id,std::initializer_list<const wchar_t*> items) {
    auto b=identify(ComboBox{},id);b.Header(text(label));b.MinWidth(180);
    b.HorizontalAlignment(HorizontalAlignment::Stretch);AutomationProperties::SetName(b,hs(label));
    for(auto item:items) b.Items().Append(box_value(hstring(item)));
    return b;
}
inline ToggleSwitch toggle(std::wstring_view title,std::wstring_view id) {
    auto t=identify(ToggleSwitch{},id);t.Header(text(title));
    AutomationProperties::SetName(t,hs(title));return t;
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
    TextBlock summary{nullptr},details{nullptr},message{nullptr},heading{nullptr},description{nullptr};
    Grid toolbar{nullptr};
    std::vector<StackPanel> adaptiveGroups;
    int layoutColumns=0;
    ProgressRing progress{nullptr};
    Button mountBtn{nullptr},readOnlyBtn{nullptr},exportBtn{nullptr},discardBtn{nullptr},openBtn{nullptr},flushBtn{nullptr},ejectBtn{nullptr},applyBtn{nullptr},
           defaultsBtn{nullptr},saveDefaultsBtn{nullptr},registerBtn{nullptr},unregisterBtn{nullptr},
           systemDefaultsBtn{nullptr},recoveryBtn{nullptr},hideBtn{nullptr},exitBtn{nullptr};
    Button uncBtn{nullptr},uncReadOnlyBtn{nullptr},copyUncBtn{nullptr},copyAddressBtn{nullptr},uncApplyBtn{nullptr},recompressBtn{nullptr};
    TextBox uncRoot{nullptr},uncSeconds{nullptr},uncSessionSeconds{nullptr},memoryMiB{nullptr};
    TextBlock memoryStats{nullptr};
    Button memoryApply{nullptr};
    ToggleSwitch preferUnc{nullptr};
    shell::UncSettings uncSettings=shell::loadUncSettings();
    bool uncAvailable=false;
    ToggleSwitch batch{nullptr},solid{nullptr};
    TextBox delay{nullptr},idle{nullptr},threads{nullptr};
    ComboBox level{nullptr},method{nullptr};
    WzOptions defaults=shell::loadDefaults();
    std::vector<WzVolume> rows;
    std::wstring selected;
    bool rebuilding=false,busy=false,quitting=false,noExplorer=false,trayReady=false,dialogOpen=false;
    struct SecretText {
        std::wstring value;
        explicit SecretText(std::wstring text):value(std::move(text)) {}
        ~SecretText(){if(!value.empty()) SecureZeroMemory(value.data(),value.size()*sizeof(wchar_t));}
    };
    struct Job {
        std::function<std::wstring()> action;
        bool quiet=false,exit=false;
        std::function<void()> passwordRetry;
        std::function<void()> completed;
    };
    std::deque<Job> jobs;
    static constexpr UINT trayMessage=WM_APP+1;
    static constexpr int levels[]={0,1,3,5,7,9};

    static StackPanel vertical(double spacing=10) {StackPanel p;p.Spacing(spacing);return p;}
    StackPanel horizontal(double spacing=8) {
        StackPanel p;p.Orientation(Orientation::Horizontal);p.Spacing(spacing);
        adaptiveGroups.push_back(p);return p;
    }
    void arrange(double width,double height) {
        if(width<=0)return;
        bool narrow=width<780;
        description.Visibility(narrow||height<600?Visibility::Collapsed:Visibility::Visible);
        int columns=narrow?2:4;
        if(layoutColumns==columns)return;
        layoutColumns=columns;
        for(auto const& group:adaptiveGroups)
            group.Orientation(narrow?Orientation::Vertical:Orientation::Horizontal);
        auto& grid=toolbar;
        grid.ColumnDefinitions().Clear();grid.RowDefinitions().Clear();
        for(int i=0;i<columns;++i) {
            ColumnDefinition col;col.Width(GridLength{1,GridUnitType::Star});grid.ColumnDefinitions().Append(col);
        }
        for(int i=0;i<(narrow?2:1);++i) {
            RowDefinition row;row.Height(GridLength{1,GridUnitType::Auto});grid.RowDefinitions().Append(row);
        }
        for(uint32_t i=0;i<grid.Children().Size();++i) {
            auto child=grid.Children().GetAt(i).as<FrameworkElement>();
            Grid::SetRow(child,int(i)/columns);Grid::SetColumn(child,int(i)%columns);
        }
        heading.FontSize(narrow?24:32);
    }
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
    void say(std::wstring_view value) {
        message.Text(hs(value));ToolTipService::SetToolTip(message,box_value(hs(value)));
    }
    WzVolume const* current() const {
        auto i=std::find_if(rows.begin(),rows.end(),[this](const auto& r){return selected==r.mount;});
        return i==rows.end()?nullptr:&*i;
    }
    static std::wstring rowText(WzVolume const& row) {
        auto name=std::filesystem::path(row.archive).filename().wstring();
        return (row.uncMode?std::wstring(L"UNC"):std::wstring(row.mount))+L"   "+name+L"   ·   "+(row.readOnly?L"Read-only":L"Read/write")+
            (row.dirty?L"   ·   Unsaved changes":L"");
    }
    void load(WzOptions const& o) {
        batch.IsOn(o.batch!=0);solid.IsOn(o.solid!=0);
        delay.Text(hs(std::to_wstring(o.delaySeconds)));idle.Text(hs(std::to_wstring(o.idleMinutes)));
        threads.Text(hs(std::to_wstring(o.threads)));
        auto row=current();
        uncSessionSeconds.Text(hs(std::to_wstring(row&&row->uncMode?row->uncCacheSeconds:uncSettings.seconds)));
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
    std::wstring sessionId(std::wstring_view drive) const {
        for(auto const& row:rows) if(drive==row.mount) return row.sessionId;
        return {};
    }
    static bool stillMounted(std::wstring const& drive,std::wstring const& id) {
        if(id.empty()) return false;
        for(int i=0;i<wz_count();++i) {
            WzVolume row{};
            if(wz_get(i,&row)&&drive==row.mount&&id==row.sessionId) return true;
        }
        return false;
    }
    void updateDetails() {
        auto r=current();
        std::wstring s=r?std::wstring(r->archive)+L"\n"+r->mount+L"   ·   Open handles: "+std::to_wstring(r->handles)+
                L"   ·   "+r->status+(r->dirty?L"   ·   Pending writeback":L"")
            :L"Select a mounted archive to manage its settings.";
        if(r&&r->uncMode)s+=L"   ·   Idle cache: "+std::to_wstring(r->uncCacheSeconds)+L" s";
        details.Text(hs(s));
        bool isUnc=r&&r->uncMode;
        idle.Visibility(isUnc?Visibility::Collapsed:Visibility::Visible);
        uncSessionSeconds.Visibility(isUnc?Visibility::Visible:Visibility::Collapsed);
        copyAddressBtn.IsEnabled(r!=nullptr);
        openBtn.IsEnabled(r!=nullptr);ejectBtn.IsEnabled(r!=nullptr);
        applyBtn.IsEnabled(r!=nullptr);flushBtn.IsEnabled(r&&!r->readOnly);
        exportBtn.IsEnabled(r&&!r->protectedArchive);
        recompressBtn.IsEnabled(r&&!r->readOnly);discardBtn.IsEnabled(r&&!r->readOnly&&r->dirty);
    }
    void refresh() {
        WzMemoryStats memory{};wz_memory_stats(&memory);
        constexpr uint64_t mib=1024ull*1024;
        memoryStats.Text(hs(L"Read cache: "+std::to_wstring(memory.cleanBytes/mib)+L" MiB · Unsaved pages: "+
            std::to_wstring(memory.dirtyBytes/mib)+L" MiB · Limit: "+std::to_wstring(memory.limitBytes/mib)+L" MiB"));
        int n=wz_snapshot(nullptr,0);
        if(n<0)return;
        std::vector<WzVolume> updated(static_cast<size_t>(n));
        int actual=wz_snapshot(updated.data(),static_cast<unsigned>(updated.size()));
        if(actual<0||actual>n)return;
        updated.resize(static_cast<size_t>(actual));
        bool changed=rows.size()!=updated.size();
        if(!changed) for(size_t i=0;i<rows.size();++i) if(rowText(rows[i])!=rowText(updated[i])||std::wstring_view(rows[i].sessionId)!=updated[i].sessionId) {changed=true;break;}
        auto previous=selected;
        auto prior=current()?current()->options:defaults;
        auto priorCache=current()?current()->uncCacheSeconds:uncSettings.seconds;
        auto priorId=current()?std::wstring(current()->sessionId):L"";
        if(updated.size()>rows.size()) {
            auto fresh=std::find_if(updated.rbegin(),updated.rend(),[&](auto const& v){
                return std::none_of(rows.begin(),rows.end(),[&](auto const& old){return std::wstring_view(v.sessionId)==old.sessionId;});
            });
            if(fresh!=updated.rend()) previous=fresh->mount;
        }
        rows=std::move(updated);
        if(changed) {
            rebuilding=true;
            list.Items().Clear();
            int index=-1;
            for(size_t i=0;i<rows.size();++i) {
                auto item=ListViewItem{};
                auto title=rowText(rows[i]);
                auto caption=text(title);
                item.Content(caption);item.HorizontalContentAlignment(HorizontalAlignment::Stretch);
                AutomationProperties::SetName(item,hs(title+L" · "+rows[i].archive));
                ToolTipService::SetToolTip(item,box_value(hs(rows[i].archive)));
                list.Items().Append(item);
                if(previous==rows[i].mount) index=int(i);
            }
            if(index<0&&!rows.empty()) index=0;
            list.SelectedIndex(index);selected=index<0?L"":rows[index].mount;
            rebuilding=false;
        }
        auto next=current()?current()->options:defaults;
        if(priorId!=(current()?std::wstring(current()->sessionId):L"")||previous!=selected||prior.batch!=next.batch||prior.delaySeconds!=next.delaySeconds||
           prior.idleMinutes!=next.idleMinutes||prior.level!=next.level||prior.method!=next.method||
           prior.threads!=next.threads||prior.solid!=next.solid||
           priorCache!=(current()?current()->uncCacheSeconds:uncSettings.seconds)) load(next);
        summary.Text(hs(std::to_wstring(rows.size())+L" mounted  ·  "+
            std::to_wstring(std::count_if(rows.begin(),rows.end(),[](const auto& r){return r.dirty!=0;}))+L" pending writeback"));
        updateDetails();
    }
    fire_and_forget startNext() {
        if(busy||jobs.empty()||quitting||dialogOpen) co_return;
        busy=true;
        auto job=std::move(jobs.front());jobs.pop_front();
        if(!job.quiet) {progress.IsActive(true);say(L"Working…");}
        apartment_context ui;
        co_await resume_background();
        std::wstring result;
        bool failed=false,requestPassword=false;
        try {
            result=job.action();if(result.starts_with(L"Error: ")) failed=true;
            requestPassword=failed&&job.passwordRetry&&wz_password_required();
        }
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
        try {
            if(!failed&&job.completed)job.completed();
            refresh();
            if(requestPassword) job.passwordRetry();
        }catch(hresult_error const& e){say(L"Error: "+ws(e.message()));}
        catch(...){say(L"Error: Cannot refresh the archive list.");}
        startNext();
    }
    void queue(std::function<std::wstring()> action,bool quiet=false,bool exit=false,
               std::function<void()> retry={},std::function<void()> completed={}) {
        if(quitting)return;
        jobs.push_back({std::move(action),quiet,exit,std::move(retry),std::move(completed)});startNext();
    }
    fire_and_forget promptPassword(std::wstring path,bool uncMode=false) {
        if(dialogOpen) co_return;
        dialogOpen=true;
        try {
            show();
            auto dialog=identify(ContentDialog{},L"PasswordDialog");
            dialog.XamlRoot(window.Content().as<FrameworkElement>().XamlRoot());
            dialog.Title(box_value(L"Protected archive"));
            auto body=vertical(12);
            body.Children().Append(text(std::filesystem::path(path).filename().wstring(),16,true));
            body.Children().Append(text(L"Enter the password to mount this archive read-only. File contents are decoded into memory only and released when the archive is closed."));
            auto password=identify(PasswordBox{},L"ArchivePassword");
            password.Header(box_value(L"Archive password"));
            body.Children().Append(password);
            dialog.Content(body);
            dialog.PrimaryButtonText(L"Mount read-only");
            dialog.CloseButtonText(L"Cancel");
            dialog.DefaultButton(ContentDialogButton::Primary);
            dialog.IsPrimaryButtonEnabled(false);
            auto token=password.PasswordChanged([dialog,password](auto&&,auto&&){
                dialog.IsPrimaryButtonEnabled(!password.Password().empty());
            });
            auto result=co_await dialog.ShowAsync();
            password.PasswordChanged(token);
            std::shared_ptr<SecretText> secret;
            if(result==ContentDialogResult::Primary) secret=std::make_shared<SecretText>(ws(password.Password()));
            password.Password(L"");
            dialogOpen=false;
            if(secret) mountPath(std::move(path),true,std::move(secret),uncMode);
            else say(L"Archive mount cancelled.");
        } catch(hresult_error const& e) {
            dialogOpen=false;say(L"Error: "+ws(e.message()));
        } catch(...) {
            dialogOpen=false;say(L"Error: Cannot open password dialog.");
        }
        startNext();
    }
    void mountPath(std::wstring path,bool readOnly=false,std::shared_ptr<SecretText> secret={},bool uncMode=false) {
        if(path.empty()) return;
        auto opt=defaults;bool explorer=!noExplorer;
        auto retryPath=path;
        queue([path=std::move(path),opt,explorer,readOnly,uncMode,secret=std::move(secret)] {
            wchar_t drive[WZ_MOUNT_CAPACITY]{};
            int ok=uncMode?wz_mount_unc(path.c_str(),&opt,readOnly,secret?secret->value.c_str():nullptr,drive,WZ_MOUNT_CAPACITY):
                   secret?wz_mount_password(path.c_str(),&opt,secret->value.c_str(),drive,WZ_MOUNT_CAPACITY):
                   readOnly?wz_mount_readonly(path.c_str(),&opt,drive,WZ_MOUNT_CAPACITY):wz_mount(path.c_str(),&opt,drive,WZ_MOUNT_CAPACITY);
            if(!ok) return L"Error: "+coreError();
            std::wstring result=L"Mounted "+std::wstring(drive)+L" · "+std::filesystem::path(path).filename().wstring();
            if(explorer&&!shell::open(std::wstring(drive)+L"\\")) result+=L" (Explorer could not be opened)";
            return result;
        },false,false,[this,retryPath,uncMode]{promptPassword(retryPath,uncMode);});
    }
    void browse(bool readOnly=false,bool uncMode=false) {
        try {auto p=shell::pick(hwnd);if(!p.empty())mountPath(std::move(p),readOnly,{},uncMode);}
        catch(hresult_error const& e){say(L"Error: "+ws(e.message()));}
    }
    void refreshUncControls() {
        wchar_t root[32768]{};
        uncAvailable=wz_unc_root(root,32768)!=0;
        uncRoot.Text(uncAvailable?hs(root):hstring(L"UNC namespace unavailable"));
        uncBtn.IsEnabled(uncAvailable);uncReadOnlyBtn.IsEnabled(uncAvailable);copyUncBtn.IsEnabled(uncAvailable);
    }
    void copyUncPath() {
        try {
            auto path=shell::pick(hwnd);
            if(path.empty())return;
            auto owner=hwnd;
            queue([path,owner]{
                wchar_t result[WZ_MOUNT_CAPACITY]{};
                if(!wz_unc_path(path.c_str(),result,WZ_MOUNT_CAPACITY))return L"Error: "+coreError();
                shell::clipboard(owner,result);
                return std::wstring(L"UNC path copied. Open it from any application while WinZipFS is running.");
            });
        }catch(...){say(L"Error: Cannot copy the UNC path.");}
    }
    void applyUncSettings() {
        try {
            shell::UncSettings next{static_cast<unsigned>(checkedNumber(uncSeconds,0,86400)),preferUnc.IsOn()};
            auto opt=defaults;
            queue([opt,next]{
                if(!wz_unc_start(&opt,next.seconds))return L"Error: "+coreError();
                shell::saveUncSettings(next);return std::wstring(L"UNC settings saved.");
            },false,false,{},[this,next]{uncSettings=next;refreshUncControls();});
        }catch(...){say(L"Error: Enter a cache time from 0 to 86400 seconds.");}
    }
    void exportCopy(std::wstring drive) {
        if(drive.empty()) return;
        auto id=sessionId(drive);
        try {
            std::wstring suggested=L"Archive copy.zip";
            for(auto const& row:rows) if(drive==row.mount)
                suggested=std::filesystem::path(row.archive).stem().wstring()+L" - copy.zip";
            show();
            auto destination=shell::pickSave(hwnd,suggested);
            if(destination.empty()) return;
            queue([drive,id,destination] {
                if(!stillMounted(drive,id)) return std::wstring(L"Error: The selected session is no longer mounted.");
                return wz_save_copy(drive.c_str(),destination.c_str())?L"Saved copy: "+destination:L"Error: "+coreError();
            });
        }catch(hresult_error const& e){say(L"Error: "+ws(e.message()));}
    }
    fire_and_forget recoveryDialog() {
        if(dialogOpen||busy) {say(L"An operation is already in progress.");co_return;}
        dialogOpen=true;
        show();
        apartment_context ui;
        progress.IsActive(true);say(L"Working…");
        co_await resume_background();
        std::vector<WzRecovery> entries;
        std::wstring failure;
        try {
            int count=wz_recovery_count();
            if(count<0)throw std::runtime_error("Cannot read recovery sessions");
            for(int i=0;i<count;++i) {
                WzRecovery entry{};
                if(wz_recovery_get(i,&entry))entries.push_back(entry);
            }
        }catch(...){failure=L"Error: Cannot access recovery sessions.";}
        co_await ui;
        progress.IsActive(false);
        try {
            if(!failure.empty()){say(failure);dialogOpen=false;startNext();co_return;}
            if(entries.empty()) {
                say(L"No abandoned archive sessions to recover.");
                dialogOpen=false;startNext();co_return;
            }
            auto dialog=identify(ContentDialog{},L"RecoveryDialog");
            dialog.XamlRoot(window.Content().as<FrameworkElement>().XamlRoot());
            dialog.Title(box_value(L"Recover archive files"));
            auto body=vertical(12);
            body.Children().Append(text(L"Save a new archive copy from an existing recovery directory without replacing the original."));
            auto items=identify(ListView{},L"RecoveryList");
            items.SelectionMode(ListViewSelectionMode::Single);items.MaxHeight(300);items.MinHeight(100);
            for(const auto& entry:entries) items.Items().Append(box_value(hs(entry.archive)));
            items.SelectedIndex(0);body.Children().Append(items);dialog.Content(body);
            dialog.PrimaryButtonText(L"Save recovered copy…");
            dialog.SecondaryButtonText(L"Delete recovery cache…");
            dialog.CloseButtonText(L"Close");
            auto result=co_await dialog.ShowAsync();
            int index=items.SelectedIndex();
            dialogOpen=false;
            if(index>=0&&index<int(entries.size())) {
                auto entry=entries[index];
                std::wstring id=entry.id;
                if(result==ContentDialogResult::Primary) {
                    auto suggested=std::filesystem::path(entry.archive).stem().wstring()+L" - recovered.zip";
                    auto destination=shell::pickSave(hwnd,suggested);
                    if(!destination.empty()) queue([id,destination]{
                        return wz_recovery_export(id.c_str(),destination.c_str())?
                            L"Recovered copy: "+destination:L"Error: "+coreError();
                    });
                } else if(result==ContentDialogResult::Secondary) {
                    if(MessageBoxW(hwnd,L"Permanently remove this recovered cache? Save a recovered copy first if you need the files.",
                                   L"WinZipFS - Delete recovery cache",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)==IDYES)
                        queue([id]{return wz_recovery_discard(id.c_str())?std::wstring(L"Recovery cache removed."):L"Error: "+coreError();});
                }
            }
        } catch(hresult_error const& e) {dialogOpen=false;say(L"Error: "+ws(e.message()));}
        catch(...) {dialogOpen=false;say(L"Error: Cannot access recovery sessions.");}
        startNext();
    }
    void discard(std::wstring drive) {
        if(drive.empty()) return;
        auto id=sessionId(drive);
        show();
        if(MessageBoxW(hwnd,L"Discard all unsaved changes and eject this archive? The original archive will remain unchanged.",
                       L"WinZipFS - Discard changes",MB_YESNO|MB_ICONWARNING|MB_DEFBUTTON2)!=IDYES) return;
        queue([drive,id] {
            if(!stillMounted(drive,id)) return std::wstring(L"Error: The selected session is no longer mounted.");
            return wz_discard(drive.c_str())?L"Discarded changes and ejected "+drive:L"Error: "+coreError();
        });
    }
    void save(std::wstring drive,bool eject) {
        if(drive.empty()) return;
        auto id=sessionId(drive);
        queue([drive=std::move(drive),eject,id]{
            if(!stillMounted(drive,id)) return std::wstring(L"Error: The selected session is no longer mounted.");
            int ok=eject?wz_eject(drive.c_str()):wz_flush(drive.c_str());
            return ok?(eject?L"Ejected "+drive:L"Saved "+drive):L"Error: "+coreError();
        });
    }
    void recompress(std::wstring drive) {
        if(drive.empty())return;
        auto id=sessionId(drive);
        show();
        if(MessageBoxW(hwnd,L"Recompress the entire archive using its current compression settings? This may take time.",
                       L"WinZipFS - Recompress archive",MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2)!=IDYES)return;
        queue([drive,id]{
            if(!stillMounted(drive,id))return std::wstring(L"Error: The selected session is no longer mounted.");
            return wz_recompress(drive.c_str())?L"Recompressed "+drive:L"Error: "+coreError();
        });
    }
    void apply() {
        if(selected.empty()) return;
        try {
            auto o=readOptions();auto drive=selected;auto id=sessionId(drive);
            bool useUnc=current()&&current()->uncMode;
            unsigned seconds=useUnc?static_cast<unsigned>(checkedNumber(uncSessionSeconds,0,86400)):0;
            queue([drive,o,id,useUnc,seconds] {
                if(!stillMounted(drive,id)) return std::wstring(L"Error: The selected session is no longer mounted.");
                int ok=useUnc?wz_unc_session_options(drive.c_str(),&o,seconds):wz_options(drive.c_str(),&o);
                return ok?L"Settings updated for "+drive:L"Error: "+coreError();
            });
        }catch(...){say(L"Error: Check numeric ranges and compression settings.");}
    }
    void saveDefaults() {
        try {
            auto o=readOptions();
            if(o.method!=0) {say(L"Error: Default compression method must be Automatic.");return;}
            auto seconds=uncSettings.seconds;
            queue([o,seconds]{
                if(!wz_unc_settings(&o,seconds))return L"Error: "+coreError();
                shell::saveDefaults(o);return std::wstring(L"Defaults saved for future mounts.");
            },false,false,{},[this,o]{defaults=o;});
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
        add(root,L"Mount read-only…",[this]{show();browse(true);});
        add(root,L"Open UNC archive…",[this]{show();browse(false,true);},MF_STRING|(uncAvailable?0:MF_GRAYED));
        add(root,L"Open UNC read-only…",[this]{show();browse(true,true);},MF_STRING|(uncAvailable?0:MF_GRAYED));
        add(root,L"Copy UNC path…",[this]{show();copyUncPath();},MF_STRING|(uncAvailable?0:MF_GRAYED));
        AppendMenuW(root,MF_SEPARATOR,0,nullptr);
        if(rows.empty()) AppendMenuW(root,MF_STRING|MF_GRAYED,0,L"No mounted archives");
        for(auto const& r:rows) {
            auto drive=std::wstring(r.mount);
            auto opt=r.options;bool readOnly=r.readOnly!=0;auto id=std::wstring(r.sessionId);
            HMENU sub=CreatePopupMenu();
            auto active=[this,drive,id]{return sessionId(drive)==id;};
            add(sub,L"Open drive",[this,drive,active]{if(active())openDrive(drive);});
            add(sub,L"Copy address",[this,drive,active]{if(!active())return;try{shell::clipboard(hwnd,drive);say(L"Archive address copied.");}catch(...){say(L"Error: Cannot copy the archive address.");}});
            add(sub,L"Compression and cache settings",[this,drive,active]{if(active())selectDrive(drive);});
            add(sub,L"Batch write mode",[this,drive,opt,id]() mutable {
                opt.batch=!opt.batch;
                queue([drive,opt,id]{
                    if(!stillMounted(drive,id)) return std::wstring(L"Error: The selected session is no longer mounted.");
                    return wz_options(drive.c_str(),&opt)?L"Batch mode updated":L"Error: "+coreError();
                });
            },MF_STRING|(opt.batch?MF_CHECKED:MF_UNCHECKED)|(readOnly?MF_GRAYED:0));
            add(sub,L"Save changes",[this,drive,active]{if(active())save(drive,false);},MF_STRING|(readOnly?MF_GRAYED:0));
            add(sub,L"Eject",[this,drive,active]{if(active())save(drive,true);});
            add(sub,L"Save archive copy…",[this,drive,active]{if(active())exportCopy(drive);},MF_STRING|(r.protectedArchive?MF_GRAYED:0));
            add(sub,L"Discard changes and eject…",[this,drive,active]{if(active())discard(drive);},MF_STRING|(!readOnly&&r.dirty?0:MF_GRAYED));
            auto title=(r.uncMode?std::wstring(L"UNC"):drive)+L"  "+std::filesystem::path(r.archive).filename().wstring();
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
        hideBtn.IsEnabled(trayReady);
        if(custom) DestroyIcon(icon.hIcon);
        if(!trayReady) {ShowWindow(hwnd,SW_SHOW);say(L"Notification area is unavailable; the main window remains open.");}
    }
    static LRESULT CALLBACK windowProc(HWND h,UINT msg,WPARAM w,LPARAM l,UINT_PTR,DWORD_PTR p) {
        auto self=reinterpret_cast<Dashboard*>(p);
        if(msg==WM_CLOSE&&!self->quitting) {self->hide();return 0;}
        if(msg==WM_GETMINMAXINFO) {
            auto result=DefSubclassProc(h,msg,w,l);
            auto minmax=reinterpret_cast<MINMAXINFO*>(l);
            UINT dpi=GetDpiForWindow(h);
            int width=MulDiv(440,dpi,96),height=MulDiv(480,dpi,96);
            MONITORINFO monitor{sizeof(monitor)};
            if(GetMonitorInfoW(MonitorFromWindow(h,MONITOR_DEFAULTTONEAREST),&monitor)) {
                width=std::min(width,static_cast<int>(monitor.rcWork.right-monitor.rcWork.left));
                height=std::min(height,static_cast<int>(monitor.rcWork.bottom-monitor.rcWork.top));
            }
            minmax->ptMinTrackSize.x=width;minmax->ptMinTrackSize.y=height;
            return result;
        }
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
                if(!data||data->dwData<1||data->dwData>3||!data->lpData||data->cbData<sizeof(wchar_t)||
                   data->cbData>65536||data->cbData%sizeof(wchar_t)) return FALSE;
                auto p=static_cast<const wchar_t*>(data->lpData);auto n=data->cbData/sizeof(wchar_t);
                if(p[n-1]!=0||wcsnlen_s(p,n)!=n-1)return FALSE;
                self->mountPath(p,false,{},data->dwData==2||(data->dwData==1&&self->uncSettings.prefer));return TRUE;
            }
        } catch(...) {self->say(L"Error: Shell operation failed.");return 0;}
        return DefWindowProcW(h,msg,w,l);
    }
    void build() {
        window.Title(L"WinZipFS");
        try {window.SystemBackdrop(MicaBackdrop());}catch(...){}
        Grid layout;
        RowDefinition fixedRow;fixedRow.Height(GridLength{1,GridUnitType::Auto});
        RowDefinition scrollRow;scrollRow.Height(GridLength{1,GridUnitType::Star});
        layout.RowDefinitions().Append(fixedRow);layout.RowDefinitions().Append(scrollRow);
        auto header=vertical(10);
        header.Padding(Thickness{28,22,28,12});header.MaxWidth(960);
        auto page=vertical(16);
        page.Padding(Thickness{28,4,28,28});page.MaxWidth(960);
        heading=text(L"WinZipFS",32,true);header.Children().Append(heading);
        description=text(L"Open archives as Windows drives or UNC paths. Read and edit files directly, with an in-memory cache.");
        header.Children().Append(description);
        toolbar=Grid{};toolbar.ColumnSpacing(8);toolbar.RowSpacing(8);
        mountBtn=button(L"Mount archive…",L"Mount");
        readOnlyBtn=button(L"Mount read-only…",L"MountReadOnly");
        hideBtn=button(L"Minimize to tray",L"Hide");hideBtn.IsEnabled(false);
        exitBtn=button(L"Exit",L"Exit");
        toolbar.Children().Append(mountBtn);toolbar.Children().Append(readOnlyBtn);toolbar.Children().Append(hideBtn);toolbar.Children().Append(exitBtn);
        header.Children().Append(toolbar);
        Grid activity;activity.ColumnSpacing(8);
        ColumnDefinition iconColumn;iconColumn.Width(GridLength{24,GridUnitType::Pixel});
        ColumnDefinition textColumn;textColumn.Width(GridLength{1,GridUnitType::Star});
        activity.ColumnDefinitions().Append(iconColumn);activity.ColumnDefinitions().Append(textColumn);
        progress=identify(ProgressRing{},L"Progress");progress.Width(20);progress.Height(20);progress.IsActive(false);
        AutomationProperties::SetName(progress,L"Archive operation progress");
        message=identify(text(L"Ready"),L"Status");message.MaxLines(3);message.TextTrimming(TextTrimming::CharacterEllipsis);
        Grid::SetColumn(message,1);activity.Children().Append(progress);activity.Children().Append(message);header.Children().Append(activity);
        auto volumes=vertical();
        volumes.Children().Append(text(L"Mounted archives",20,true));
        summary=identify(text(L"0 mounted"),L"Summary");volumes.Children().Append(summary);
        list=identify(ListView{},L"Volumes");list.SelectionMode(ListViewSelectionMode::Single);list.MinHeight(100);list.MaxHeight(230);
        volumes.Children().Append(list);
        details=identify(text(L"Select a mounted archive."),L"Details");volumes.Children().Append(details);
        auto actions=horizontal();
        openBtn=button(L"Open drive",L"Open");flushBtn=button(L"Save changes",L"Flush");ejectBtn=button(L"Eject",L"Eject");
        copyAddressBtn=button(L"Copy address",L"CopyAddress");
        actions.Children().Append(openBtn);actions.Children().Append(flushBtn);actions.Children().Append(ejectBtn);actions.Children().Append(copyAddressBtn);
        volumes.Children().Append(actions);
        auto protection=horizontal();
        exportBtn=button(L"Save archive copy…",L"SaveCopy");
        discardBtn=button(L"Discard changes and eject…",L"Discard");
        recompressBtn=button(L"Recompress all…",L"Recompress");
        protection.Children().Append(exportBtn);protection.Children().Append(discardBtn);protection.Children().Append(recompressBtn);
        volumes.Children().Append(protection);page.Children().Append(card(volumes));
        auto settings=vertical(12);
        settings.Children().Append(text(L"Compression and cache",20,true));
        settings.Children().Append(text(L"Apply settings to the selected archive, or save them as defaults for future mounts."));
        batch=toggle(L"Batch write mode",L"Batch");settings.Children().Append(batch);
        settings.Children().Append(text(L"Batch mode keeps modifications in memory until Save changes or Eject. Unexpected termination loses unsaved changes."));
        auto time=horizontal(12);
        delay=number(L"Writeback delay (seconds)",L"Delay");idle=number(L"Auto-eject (minutes, 0 = off)",L"Idle");
        uncSessionSeconds=number(L"UNC idle cache (seconds)",L"UncSessionSeconds");
        time.Children().Append(delay);time.Children().Append(idle);time.Children().Append(uncSessionSeconds);settings.Children().Append(time);
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
        auto memoryPanel=vertical(10);
        memoryPanel.Children().Append(text(L"Memory cache",20,true));
        memoryPanel.Children().Append(text(L"Archives are indexed on open. File contents are read directly or decoded on demand. Pending edits stay in memory until saved; no extracted files are written to a cache directory."));
        memoryStats=identify(text(L""),L"MemoryStats");
        memoryPanel.Children().Append(memoryStats);
        auto memoryActions=horizontal();
        memoryMiB=number(L"Memory limit (MiB)",L"MemoryMiB");
        memoryMiB.Text(hs(std::to_wstring(shell::loadMemoryLimit())));
        memoryApply=button(L"Apply memory limit",L"MemoryApply");
        memoryActions.Children().Append(memoryMiB);memoryActions.Children().Append(memoryApply);
        memoryPanel.Children().Append(memoryActions);
        page.Children().Append(card(memoryPanel));
        auto uncPanel=vertical(10);
        uncPanel.Children().Append(text(L"UNC access without drive letters",20,true));
        uncPanel.Children().Append(text(L"Open an archive by its UNC path from any Windows application while WinZipFS is running. Each archive cache is released after the last handle closes and its idle time expires."));
        uncRoot=identify(TextBox{},L"UncRoot");
        uncRoot.Header(box_value(L"Your UNC namespace"));
        uncRoot.IsReadOnly(true);uncRoot.TextWrapping(TextWrapping::Wrap);
        uncPanel.Children().Append(uncRoot);
        auto uncActions=horizontal();
        uncBtn=button(L"Open UNC archive…",L"MountUnc");
        uncReadOnlyBtn=button(L"Open UNC read-only…",L"MountUncReadOnly");
        copyUncBtn=button(L"Copy UNC path…",L"CopyUnc");
        uncActions.Children().Append(uncBtn);uncActions.Children().Append(uncReadOnlyBtn);uncActions.Children().Append(copyUncBtn);
        uncPanel.Children().Append(uncActions);
        uncSeconds=number(L"Cache lifetime (seconds)",L"UncSeconds");
        uncSeconds.Text(hs(std::to_wstring(uncSettings.seconds)));
        uncPanel.Children().Append(uncSeconds);
        uncPanel.Children().Append(text(L"0 releases the archive on the next idle check. Pending modifications are saved before release."));
        preferUnc=toggle(L"Use UNC for double-click and command-line opening",L"PreferUnc");
        preferUnc.IsOn(uncSettings.prefer);uncPanel.Children().Append(preferUnc);
        uncApplyBtn=button(L"Apply UNC settings",L"UncApply");uncPanel.Children().Append(uncApplyBtn);
        page.Children().Append(card(uncPanel));
        auto integrations=vertical();
        integrations.Children().Append(text(L"Windows integration",20,true));
        integrations.Children().Append(text(L"Register archive file types and choose WinZipFS as their default app in Windows Settings."));
        auto associations=horizontal();
        registerBtn=button(L"Register file types",L"Register");unregisterBtn=button(L"Remove registration",L"Unregister");
        systemDefaultsBtn=button(L"Default apps",L"DefaultApps");
        associations.Children().Append(registerBtn);associations.Children().Append(unregisterBtn);
        associations.Children().Append(systemDefaultsBtn);integrations.Children().Append(associations);
        recoveryBtn=button(L"Recover archive files…",L"Recovery");integrations.Children().Append(recoveryBtn);
        page.Children().Append(card(integrations));
        auto scroll=identify(ScrollViewer{},L"DashboardScroll");scroll.Content(page);scroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        scroll.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
        scroll.HorizontalScrollMode(ScrollMode::Disabled);
        Grid::SetRow(header,0);layout.Children().Append(header);
        Grid::SetRow(scroll,1);layout.Children().Append(scroll);
        arrange(900,850);
        layout.SizeChanged([this](auto&&,SizeChangedEventArgs const& e){arrange(e.NewSize().Width,e.NewSize().Height);});
        window.Content(layout);
        mountBtn.Click([this](auto&&,auto&&){browse();});
        readOnlyBtn.Click([this](auto&&,auto&&){browse(true);});
        uncBtn.Click([this](auto&&,auto&&){browse(false,true);});
        uncReadOnlyBtn.Click([this](auto&&,auto&&){browse(true,true);});
        copyUncBtn.Click([this](auto&&,auto&&){copyUncPath();});
        uncApplyBtn.Click([this](auto&&,auto&&){applyUncSettings();});
        memoryApply.Click([this](auto&&,auto&&){
            try {
                auto value=static_cast<unsigned>(checkedNumber(memoryMiB,16,65536));
                queue([value]{
                    if(!wz_memory_limit(value))return L"Error: "+coreError();
                    shell::saveMemoryLimit(value);return std::wstring(L"Memory limit saved.");
                });
            }catch(...){say(L"Error: Enter a memory limit from 16 to 65536 MiB.");}
        });
        exportBtn.Click([this](auto&&,auto&&){exportCopy(selected);});
        discardBtn.Click([this](auto&&,auto&&){discard(selected);});
        recompressBtn.Click([this](auto&&,auto&&){recompress(selected);});
        hideBtn.Click([this](auto&&,auto&&){hide();});
        exitBtn.Click([this](auto&&,auto&&){quit();});
        list.SelectionChanged([this](auto&&,auto&&){
            if(rebuilding)return;int i=list.SelectedIndex();selected=i>=0&&i<int(rows.size())?rows[i].mount:L"";
            load(current()?current()->options:defaults);updateDetails();
        });
        openBtn.Click([this](auto&&,auto&&){openDrive(selected);});
        copyAddressBtn.Click([this](auto&&,auto&&){
            if(!selected.empty())try{shell::clipboard(hwnd,selected);say(L"Archive address copied.");}catch(...){say(L"Error: Cannot copy the archive address.");}
        });
        flushBtn.Click([this](auto&&,auto&&){save(selected,false);});
        ejectBtn.Click([this](auto&&,auto&&){save(selected,true);});
        applyBtn.Click([this](auto&&,auto&&){apply();});
        defaultsBtn.Click([this](auto&&,auto&&){load(defaults);say(L"Default settings loaded.");});
        saveDefaultsBtn.Click([this](auto&&,auto&&){saveDefaults();});
        registerBtn.Click([this](auto&&,auto&&){try{shell::registerAssociations();say(L"Archive file types registered.");}catch(...){say(L"Error: Cannot register file types.");}});
        unregisterBtn.Click([this](auto&&,auto&&){shell::unregisterAssociations();say(L"Archive file type registration removed.");});
        systemDefaultsBtn.Click([this](auto&&,auto&&){say(shell::open(L"ms-settings:defaultapps")?L"Opened Windows default apps.":L"Error: Windows default apps settings unavailable.");});
        recoveryBtn.Click([this](auto&&,auto&&){recoveryDialog();});
        load(defaults);updateDetails();
    }
public:
    explicit Dashboard(std::vector<std::wstring> args) {
        build();
        winrt::check_hresult(window.as<IWindowNative>()->get_WindowHandle(&hwnd));
        auto dpi=GetDpiForWindow(hwnd);
        int width=MulDiv(900,dpi,96),height=MulDiv(850,dpi,96);
        MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
        if(GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor)) {
            auto const& work=monitor.rcWork;
            int workWidth=work.right-work.left,workHeight=work.bottom-work.top;
            width=(std::min)(width,workWidth);
            height=(std::min)(height,workHeight);
            SetWindowPos(hwnd,nullptr,work.left+(workWidth-width)/2,work.top+(workHeight-height)/2,width,height,SWP_NOZORDER);
        } else SetWindowPos(hwnd,nullptr,0,0,width,height,SWP_NOMOVE|SWP_NOZORDER);
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
        wz_memory_limit(shell::loadMemoryLimit());
        wz_unc_start(&defaults,uncSettings.seconds);
        refreshUncControls();
        int recovered=wz_recovery_count();
        if(recovered>0) say(L"Recovered archive sessions are available. Select Recover archive files to save their contents.");
        else if(recovered<0) say(L"Error: "+coreError());
        timer.Interval(std::chrono::seconds(2));
        timer.Tick([this](auto&&,auto&&){if(!trayReady&&!quitting)addTray();if(!busy&&jobs.empty()&&!quitting)queue([]{return wz_tick()?std::wstring{}:L"Error: "+coreError();},true);});
        timer.Start();
        bool useUnc=uncSettings.prefer;
        for(auto const& arg:args) {
            if(arg==L"--unc")useUnc=true;
            else if(arg==L"--drive")useUnc=false;
            else if(arg==L"--register") {try{shell::registerAssociations();}catch(...){say(L"Error: Cannot register archive types.");}}
            else if(arg==L"--unregister")shell::unregisterAssociations();
            else if(!arg.starts_with(L"--"))mountPath(arg,false,{},useUnc);
        }
    }
};
}
using Dashboard=wzui::Dashboard;
