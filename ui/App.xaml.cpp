#include "pch.h"
#include "App.xaml.h"
#if __has_include("App.g.cpp")
#include "App.g.cpp"
#endif
#include "dashboard.hpp"

namespace winrt::WinZipFS::implementation {
App::App() { InitializeComponent(); }
void App::OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&) {
    static HANDLE instance=CreateMutexW(nullptr,FALSE,L"Local\\WinZipFS.Instance");
    const bool existing=GetLastError()==ERROR_ALREADY_EXISTS;
    auto args=shell::arguments();
    if(existing) {
        if(!shell::forward(args)) MessageBoxW(nullptr,L"Cannot contact the running WinZipFS instance.",L"WinZipFS",MB_ICONERROR);
        Exit();return;
    }
    static std::unique_ptr<Dashboard> dashboard;
    dashboard=std::make_unique<Dashboard>(std::move(args));
}
}
