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
    static HANDLE instance=CreateMutexW(nullptr,FALSE,L"Local\\WinZipFS.Instance");
    if(!instance) {
        MessageBoxW(nullptr,L"Cannot initialize the application instance.",L"WinZipFS",MB_ICONERROR);
        Exit();return;
    }
    const bool existing=GetLastError()==ERROR_ALREADY_EXISTS;
    if(existing) {
        if(!shell::forward(args)) MessageBoxW(nullptr,L"Cannot contact the running WinZipFS instance.",L"WinZipFS",MB_ICONERROR);
        Exit();return;
    }
    static std::unique_ptr<Dashboard> dashboard;
    dashboard=std::make_unique<Dashboard>(std::move(args));
}
}
