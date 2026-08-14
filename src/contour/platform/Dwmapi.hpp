// SPDX-License-Identifier: Apache-2.0
#pragma once

#ifdef _WIN32

    #include <Windows.h>

namespace contour::platform
{

/// The handful of DWM and per-monitor-DPI entry points the window frame and the blur effect need.
///
/// Resolved at runtime rather than linked, which is the convention BlurBehind.cpp already
/// established here: every one of these is absent on some Windows version Contour still starts on,
/// and a missing import stops the process from launching at all rather than costing it a shadow.
struct DwmApi
{
    // dwmapi.dll
    HRESULT(WINAPI* setWindowAttribute)(HWND, DWORD, LPCVOID, DWORD) = nullptr;
    HRESULT(WINAPI* extendFrameIntoClientArea)(HWND, MARGINS const*) = nullptr;

    // user32.dll, Windows 10 1607 and later. Without these the frame thickness can only be asked
    // for the PRIMARY monitor, which is the wrong answer on a mixed-DPI desktop.
    UINT(WINAPI* getDpiForWindow)(HWND) = nullptr;
    BOOL(WINAPI* adjustWindowRectExForDpi)(LPRECT, DWORD, BOOL, DWORD, UINT) = nullptr;

    /// Whether the frame can be kept and zeroed at all. False on a Windows too old for it, where the
    /// caller should leave the window alone rather than half-decorate it.
    [[nodiscard]] bool isUsable() const noexcept
    {
        return setWindowAttribute != nullptr && extendFrameIntoClientArea != nullptr
               && getDpiForWindow != nullptr && adjustWindowRectExForDpi != nullptr;
    }
};

/// The resolved entry points, loaded once.
///
/// A function-local static because this is a cache of the process's own module handles, not
/// configuration: there is exactly one dwmapi.dll and it answers the same thing for every window.
/// The modules are deliberately never freed -- both are system libraries the process keeps loaded
/// regardless, and a FreeLibrary here would only risk invalidating a pointer still in use.
[[nodiscard]] inline DwmApi const& dwmApi()
{
    static DwmApi const api = [] {
        auto resolved = DwmApi {};

        if (auto* dwm = LoadLibraryW(L"dwmapi.dll"))
        {
            resolved.setWindowAttribute = reinterpret_cast<decltype(resolved.setWindowAttribute)>(
                GetProcAddress(dwm, "DwmSetWindowAttribute"));
            resolved.extendFrameIntoClientArea =
                reinterpret_cast<decltype(resolved.extendFrameIntoClientArea)>(
                    GetProcAddress(dwm, "DwmExtendFrameIntoClientArea"));
        }

        if (auto* user32 = LoadLibraryW(L"user32.dll"))
        {
            resolved.getDpiForWindow = reinterpret_cast<decltype(resolved.getDpiForWindow)>(
                GetProcAddress(user32, "GetDpiForWindow"));
            resolved.adjustWindowRectExForDpi = reinterpret_cast<decltype(resolved.adjustWindowRectExForDpi)>(
                GetProcAddress(user32, "AdjustWindowRectExForDpi"));
        }

        return resolved;
    }();
    return api;
}

// Attribute ids, spelled out because the SDK Contour builds against may predate them.
inline constexpr auto DwmwaUseImmersiveDarkMode = DWORD { 20 };
inline constexpr auto DwmwaWindowCornerPreference = DWORD { 33 };
inline constexpr auto DwmwcpRound = DWORD { 2 };

} // namespace contour::platform

#endif
