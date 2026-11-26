#include "pch.h"
#include "App.xaml.h"
#include "dashboard.hpp"

namespace winrt::WinZipFS::implementation {
App::App() { InitializeComponent(); }
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
    auto args=shell::arguments();
    if(args.size()==1 && (args[0]==L"--register"||args[0]==L"--unregister")) {
        try {
            if(args[0]==L"--register")shell::registerAssociations();
            else shell::unregisterAssociations();
            ExitProcess(0);
        }catch(...) {ExitProcess(1);}
    }
    static HANDLE instance=nullptr;
    instance=CreateMutexW(nullptr,TRUE,L"Local\\WinZipFS.Instance");
    if(!instance) {
        MessageBoxW(nullptr,L"Cannot initialize the application instance.",L"WinZipFS",MB_ICONERROR);
        Exit();return;
    }
    const bool existing=GetLastError()==ERROR_ALREADY_EXISTS;
    if(existing) {
        auto acquired=[] {
            DWORD result=WaitForSingleObject(instance,0);
            return result==WAIT_OBJECT_0||result==WAIT_ABANDONED;
        };
        // A referenced mutex can outlive its former owner. Only ownership
        // indicates that another primary instance is still running.
        if(!acquired()) {
            if(shell::forward(args)) {
                CloseHandle(instance);instance=nullptr;
                Exit();return;
            }
            if(!acquired()) {
                CloseHandle(instance);instance=nullptr;
                MessageBoxW(nullptr,L"Cannot contact the running WinZipFS instance.",L"WinZipFS",MB_ICONERROR);
                Exit();return;
            }
        }
    }
    static std::unique_ptr<Dashboard> dashboard;
    dashboard=std::make_unique<Dashboard>(std::move(args));
}
}
