#pragma once
#include "pch.h"
#include "App.xaml.g.h"
namespace winrt::WinZipFS::implementation {
struct App : AppT<App> {
    App();
    void OnLaunched(Microsoft::UI::Xaml::LaunchActivatedEventArgs const&);
};
}
