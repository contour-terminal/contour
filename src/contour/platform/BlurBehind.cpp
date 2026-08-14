// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/platform/BlurBehind.hpp>
#include <contour/platform/XcbProperty.hpp>

#include <crispy/Utils.hpp>

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#ifdef _WIN32
    #include <contour/platform/Dwmapi.hpp>

    #include <Windows.h>
#endif

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    #include <QtWaylandClient/QWaylandClientExtension>
    #include <QtWaylandClient/private/qwaylandwindow_p.h>

    #include <map>
    #include <memory>

    #include "qwayland-blur.h"
    #include <qpa/qplatformwindow.h>
#endif

namespace contour::platform
{

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
namespace
{
    class KWinBlurManager:
        public QWaylandClientExtensionTemplate<KWinBlurManager>,
        public QtWayland::org_kde_kwin_blur_manager
    {
      public:
        KWinBlurManager(): QWaylandClientExtensionTemplate<KWinBlurManager>(1) { initialize(); }
    };
    Q_GLOBAL_STATIC(KWinBlurManager, kwinBlurManager)
} // namespace
#endif

void setBlurBehind(QWindow* window, bool enable, QRegion const& region)
{
    crispy::ignoreUnused(region);

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::platformName() == "wayland")
    {
        static auto activeBlurs = std::map<QWindow*, std::unique_ptr<QtWayland::org_kde_kwin_blur>> {};

        if (auto* blurManager = kwinBlurManager(); blurManager && blurManager->isActive())
        {
            if (auto* platformWindow = window->handle())
            {
                if (auto* waylandWindow = static_cast<QtWaylandClient::QWaylandWindow*>(platformWindow))
                {
                    if (auto* surface = waylandWindow->surface())
                    {
                        if (enable)
                        {
                            if (!activeBlurs.contains(window))
                            {
                                auto* rawBlur = blurManager->create(surface);
                                activeBlurs[window] = std::make_unique<QtWayland::org_kde_kwin_blur>(rawBlur);
                                QObject::connect(window, &QWindow::destroyed, [window]() {
                                    auto it = activeBlurs.find(window);
                                    if (it != activeBlurs.end())
                                    {
                                        it->second->release();
                                        activeBlurs.erase(it);
                                    }
                                });
                            }
                            auto& blur = *activeBlurs[window];
                            blur.set_region(nullptr);
                            blur.commit();
                        }
                        else
                        {
                            auto it = activeBlurs.find(window);
                            if (it != activeBlurs.end())
                            {
                                it->second->release();
                                activeBlurs.erase(it);
                            }
                            blurManager->unset(surface);
                        }
                    }
                }
            }
        }
        return;
    }
#endif

#if !defined(Q_OS_WINDOWS) && !defined(Q_OS_DARWIN)
    // This #if should catch UNIX in general but not Mac, so we have not just Linux but also the BSDs and
    // maybe others if one wants to.
    //
    // I was looking into the kwin source code and it's all in fact just a one-liner, so easy to get rid of
    // the dependency and still support nice looking semi transparent blurred backgrounds.
    if (QGuiApplication::platformName() == "xcb")
    {
        if (enable)
        {
            setPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION", uint32_t { 0 });
            setPropertyX11(window, "_MUTTER_HINTS", "blur-provider=sigma:60,brightness:0.9");
        }
        else
        {
            // Unset exactly what the branch above sets. This used to delete an atom named
            // "kwin_blur", which nothing ever creates -- so _KDE_NET_WM_BLUR_BEHIND_REGION
            // survived, and blur could never be switched off again for the window's lifetime.
            unsetPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION");
            unsetPropertyX11(window, "_MUTTER_HINTS");
        }
        return;
    }
#endif

#if defined(_WIN32) // {{{
    // Awesome hack with the noteworthy links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    if (HWND hwnd = (HWND) window->winId(); hwnd != nullptr)
    {
        // 1. Attempt DWM-based blur (Windows 11+)
        //
        // This used to force DWMWA_USE_IMMERSIVE_DARK_MODE on here, unconditionally and regardless
        // of the configured `theme`, which put a dark window border on a light-themed window. It is
        // a FRAME attribute rather than a blur one -- and now that the frame is kept rather than
        // discarded it also tints the border around the rounded corners -- so it moved to
        // Win32WindowFrame, which is told what the effective color scheme actually is.
        if (auto const& api = dwmApi(); api.setWindowAttribute != nullptr)
        {
            const DWORD DWMWA_SYSTEMBACKDROP_TYPE = 38;
            const DWORD DWMSBT_NONE = 1;
            const DWORD DWMSBT_TRANSIENTWINDOW = 3; // Acrylic

            DWORD backdropType = enable ? DWMSBT_TRANSIENTWINDOW : DWMSBT_NONE;
            HRESULT hr =
                api.setWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdropType, sizeof(backdropType));
            if (SUCCEEDED(hr))
                return;
        }

        // 2. Fallback to undocumented SetWindowCompositionAttribute (Windows 10)
        const HINSTANCE hModule = LoadLibrary(TEXT("user32.dll"));
        if (hModule)
        {
            enum WindowCompositionAttribute : int
            {
                // ...
                WCA_ACCENT_POLICY = 19,
                // ...
            };
            enum AcceptState : int
            {
                ACCENT_DISABLED = 0,
                ACCENT_ENABLE_GRADIENT = 1,
                ACCENT_ENABLE_TRANSPARENTGRADIENT = 2,
                ACCENT_ENABLE_BLURBEHIND = 3,
                ACCENT_ENABLE_ACRYLICBLURBEHIND = 4,
                ACCENT_ENABLE_HOSTBACKDROP = 5,
            };
            struct ACCENTPOLICY
            {
                AcceptState nAccentState;
                UINT nFlags;
                COLORREF nColor;
                LONG nAnimationId;
            };
            struct WINCOMPATTRDATA
            {
                WindowCompositionAttribute nAttribute;
                void const* pData;
                ULONG ulDataSize;
            };
            typedef BOOL(WINAPI * pSetWindowCompositionAttribute)(HWND, WINCOMPATTRDATA const*);
            pSetWindowCompositionAttribute const SetWindowCompositionAttribute =
                (pSetWindowCompositionAttribute) GetProcAddress(hModule, "SetWindowCompositionAttribute");
            if (SetWindowCompositionAttribute)
            {
                // Use a dark semi-transparent tint (0x99000000) for Acrylic to be visible.
                auto const policy = enable
                                        ? ACCENTPOLICY { ACCENT_ENABLE_ACRYLICBLURBEHIND, 0, 0x99000000, 0 }
                                        : ACCENTPOLICY { ACCENT_DISABLED, 0, 0, 0 };
                auto const data = WINCOMPATTRDATA { WCA_ACCENT_POLICY, &policy, sizeof(ACCENTPOLICY) };
                BOOL rs = SetWindowCompositionAttribute(hwnd, &data);
                if (!rs)
                    qDebug() << "SetWindowCompositionAttribute" << rs;
            }
            FreeLibrary(hModule);
        }
    }
#endif // }}}

    // Get me working on other platforms/compositors (such as OSX, Gnome, ...), please.
    crispy::ignoreUnused(window, enable);
}

} // namespace contour::platform
