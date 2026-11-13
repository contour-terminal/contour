// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/BlurBehind.hpp>
#include <contour/platform/XcbProperty.hpp>

#include <core/Utils.hpp>
#include <core/log/LogStore.hpp>

#include <QtCore/QDebug>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <algorithm>
#include <array>
#include <cmath>
#include <expected>
#include <map>
#include <memory>
#include <span>

#ifdef _WIN32
    #include <contour/platform/Dwmapi.hpp>
    #include <contour/platform/Win32SystemBackdrop.hpp>

    #include <Windows.h>
#endif

#if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    #include <QtWaylandClient/QWaylandClientExtension>
    #include <QtWaylandClient/private/qwaylanddisplay_p.h>
    #include <QtWaylandClient/private/qwaylandwindow_p.h>

    #include "qwayland-blur.h"
    #include <qpa/qplatformwindow.h>
#endif

namespace contour::platform
{

#if !defined(_WIN32) && !defined(__APPLE__)
namespace
{
    /// What one window asks of the compositor's blur: `background.blur`'s wish, behind the whole window, and
    /// the title band's (setTitleBandBlur), behind its top alone. One blur object per surface carries both,
    /// so each request recomputes the region from the two rather than overwriting the other's.
    struct CompositorBlurWishes
    {
        BlurBehindWish window = BlurBehindWish::Off; ///< The profile's wish, for the whole window.
        qreal bandHeight = 0.0;                      ///< The title band's height; 0 asks for none.
    };

    /// The region a window's wishes add up to.
    enum class CompositorBlurRegion : uint8_t
    {
        None = 0,    ///< Nothing is blurred.
        WholeWindow, ///< The whole surface: the profile asked for it, which covers the band too.
        Band,        ///< The top @c bandHeight logical pixels.
    };

    /// @param wishes A window's wishes.
    /// @return The region they add up to.
    [[nodiscard]] constexpr CompositorBlurRegion regionOf(CompositorBlurWishes const& wishes) noexcept
    {
        if (wishes.window == BlurBehindWish::On)
            return CompositorBlurRegion::WholeWindow;
        return wishes.bandHeight > 0.0 ? CompositorBlurRegion::Band : CompositorBlurRegion::None;
    }

    /// How wide a band region is: wider than any window, so it follows a resize without being sent again --
    /// the compositor clips a blur region to its surface.
    constexpr int BandBlurWidth = 32767;

    /// The wishes of every window that has made one, forgotten when the window goes.
    [[nodiscard]] std::map<QWindow*, CompositorBlurWishes>& wishesByWindow()
    {
        static auto wishes = std::map<QWindow*, CompositorBlurWishes> {};
        return wishes;
    }

    #if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    class KWinBlurManager:
        public QWaylandClientExtensionTemplate<KWinBlurManager>,
        public QtWayland::org_kde_kwin_blur_manager
    {
      public:
        KWinBlurManager(): QWaylandClientExtensionTemplate<KWinBlurManager>(1) { initialize(); }
    };
    Q_GLOBAL_STATIC(KWinBlurManager, kwinBlurManager)

    /// The blur object of every window KWin blurs behind.
    [[nodiscard]] std::map<QWindow*, std::unique_ptr<QtWayland::org_kde_kwin_blur>>& activeBlurs()
    {
        static auto blurs = std::map<QWindow*, std::unique_ptr<QtWayland::org_kde_kwin_blur>> {};
        return blurs;
    }

    /// Releases @p window's blur object, if it has one.
    void releaseBlur(QWindow* window)
    {
        auto& blurs = activeBlurs();
        if (auto const blur = blurs.find(window); blur != blurs.end())
        {
            blur->second->release();
            blurs.erase(blur);
        }
    }

    /// Asks KWin, through org_kde_kwin_blur, for the region @p wishes add up to behind @p window.
    /// @return Unsupported where the compositor offers no such protocol (anything but KWin).
    [[nodiscard]] std::expected<void, TitleBandMaterialError> applyWaylandBlur(
        QWindow* window, CompositorBlurWishes const& wishes)
    {
        auto* const blurManager = kwinBlurManager();
        if (blurManager == nullptr || !blurManager->isActive())
            return std::unexpected(TitleBandMaterialError::Unsupported);
        auto* const waylandWindow = static_cast<QtWaylandClient::QWaylandWindow*>(window->handle());
        if (waylandWindow == nullptr || waylandWindow->surface() == nullptr)
            return {}; // Not shown yet: the window asks again once it is.
        auto* const surface = waylandWindow->surface();

        auto const region = regionOf(wishes);
        if (region == CompositorBlurRegion::None)
        {
            releaseBlur(window);
            blurManager->unset(surface);
            window->requestUpdate();
            return {};
        }

        auto& blurs = activeBlurs();
        if (!blurs.contains(window))
            blurs[window] = std::make_unique<QtWayland::org_kde_kwin_blur>(blurManager->create(surface));
        auto& blur = *blurs[window];
        if (region == CompositorBlurRegion::WholeWindow)
            blur.set_region(nullptr);
        else
        {
            // Surface-local, logical coordinates: Wayland scales a region with its surface.
            auto const band = QRegion(0, 0, BandBlurWidth, static_cast<int>(std::ceil(wishes.bandHeight)));
            auto* const wlRegion = waylandWindow->display()->createRegion(band);
            blur.set_region(wlRegion);
            wl_region_destroy(wlRegion);
        }
        blur.commit();
        // The blur takes effect with the surface's next commit.
        window->requestUpdate();
        return {};
    }
    #endif

    /// Asks the X11 compositor for the region @p wishes add up to behind @p window: KWin's
    /// _KDE_NET_WM_BLUR_BEHIND_REGION, and for the whole window also the Mutter fork's hint. A window manager
    /// that knows neither ignores both.
    void applyXcbBlur(QWindow* window, CompositorBlurWishes const& wishes)
    {
        switch (regionOf(wishes))
        {
            case CompositorBlurRegion::None:
                // Unset exactly what the branches below set.
                unsetPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION");
                unsetPropertyX11(window, "_MUTTER_HINTS");
                return;
            case CompositorBlurRegion::WholeWindow:
                setPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION", uint32_t { 0 });
                setPropertyX11(window, "_MUTTER_HINTS", "blur-provider=sigma:60,brightness:0.9");
                return;
            case CompositorBlurRegion::Band: {
                // x, y, width, height, in device pixels.
                auto const height =
                    static_cast<uint32_t>(std::ceil(wishes.bandHeight * window->devicePixelRatio()));
                auto const rect = std::array<uint32_t, 4> { 0, 0, uint32_t { BandBlurWidth }, height };
                setPropertyX11(window, "_KDE_NET_WM_BLUR_BEHIND_REGION", std::span<uint32_t const>(rect));
                unsetPropertyX11(window, "_MUTTER_HINTS");
                return;
            }
        }
    }

    /// Lets @p update change @p window's wishes and asks the compositor for what they now add up to.
    /// @return Unsupported where no compositor blur is reachable from this platform plugin.
    template <typename Update>
    [[nodiscard]] std::expected<void, TitleBandMaterialError> updateCompositorBlur(QWindow* window,
                                                                                   Update const& update)
    {
        auto& wishes = wishesByWindow();
        if (!wishes.contains(window))
        {
            QObject::connect(window, &QWindow::destroyed, [window]() {
                wishesByWindow().erase(window);
    #if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
                releaseBlur(window);
    #endif
            });
        }
        update(wishes[window]);
        auto const& current = wishes[window];

    #if defined(CONTOUR_WAYLAND) && QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        if (QGuiApplication::platformName() == "wayland")
            return applyWaylandBlur(window, current);
    #endif
        if (QGuiApplication::platformName() == "xcb")
        {
            applyXcbBlur(window, current);
            return {};
        }
        return std::unexpected(TitleBandMaterialError::Unsupported);
    }
} // namespace
#endif

void setBlurBehind(QWindow* window, BlurBehindWish blur, QRegion const& region)
{
    core::ignoreUnused(region);

#if !defined(_WIN32) && !defined(__APPLE__)
    // Not just Linux but every UNIX that is not macOS: the BSDs run KWin too. A desktop with no compositor
    // blur (GNOME, most X11 window managers) is the ordinary case and says nothing; a compositor that has
    // one and refuses it is worth a line.
    if (auto const blurred =
            updateCompositorBlur(window, [blur](CompositorBlurWishes& wishes) { wishes.window = blur; });
        !blurred && blurred.error() == TitleBandMaterialError::Rejected)
        errorLog()("The compositor rejected the window's background blur ({}).",
                   titleBandMaterialErrorName(blurred.error()));
#else
    auto const enable = blur == BlurBehindWish::On;
#endif

#if defined(_WIN32) // {{{
    // Awesome hack with the noteworthy links:
    // * https://gist.github.com/ethanhs/0e157e4003812e99bf5bc7cb6f73459f (used as code template)
    // * https://github.com/riverar/sample-win32-acrylicblur/blob/master/MainWindow.xaml.cs
    // * https://stackoverflow.com/questions/44000217/mimicking-acrylic-in-a-win32-app
    // p.s.: if you find a more official way to do it, please PR me. :)

    if (HWND hwnd = (HWND) window->winId(); hwnd != nullptr)
    {
        // 1. Attempt DWM-based blur (Windows 11 22H2+)
        //
        // Not a write of DWMWA_SYSTEMBACKDROP_TYPE of its own: the title band's Mica writes the same
        // attribute, and blur off must hand the window back to Mica rather than to no backdrop at all. The
        // wish is recorded on the window, and applySystemBackdrop decides from it and the band's record --
        // Acrylic over the whole window while blur is on, the user's explicit choice (R-i).
        //
        // DWMWA_USE_IMMERSIVE_DARK_MODE used to be forced on here too, regardless of the configured `theme`;
        // it is a FRAME attribute and lives in Win32WindowFrame, which knows the effective color scheme.
        recordBlurBehind(*window, blur);
        if (applySystemBackdrop(*window, hwnd, dwmApi()))
            return;

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

#ifdef __APPLE__
    core::ignoreUnused(window, enable);
#endif
}

std::expected<void, TitleBandMaterialError> setTitleBandBlur(QWindow& window, qreal bandHeight)
{
#if !defined(_WIN32) && !defined(__APPLE__)
    return updateCompositorBlur(&window, [bandHeight](CompositorBlurWishes& wishes) {
        wishes.bandHeight = std::max(0.0, bandHeight);
    });
#else
    core::ignoreUnused(window, bandHeight);
    return std::unexpected(TitleBandMaterialError::Unsupported);
#endif
}

} // namespace contour::platform
