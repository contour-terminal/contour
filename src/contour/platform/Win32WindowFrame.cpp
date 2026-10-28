// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32WindowFrame.hpp>

#ifdef _WIN32

    #include <contour/platform/Dwmapi.hpp>
    #include <contour/platform/Win32SystemBackdrop.hpp>

    #include <core/log/LogStore.hpp>

    #include <QtCore/QAbstractNativeEventFilter>
    #include <QtCore/QCoreApplication>
    #include <QtCore/QPointer>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <map>
    #include <utility>

namespace contour::platform
{

namespace
{
    auto const backdropLog = core::log::Category { "window.backdrop", "Windows DWM system backdrop" };

    /// Re-applies the backdrop and the margins @p window's records decide (Win32SystemBackdrop.hpp), logging
    /// a failure that is news: the frame keeps working without the backdrop.
    void reapplySystemBackdrop(QWindow& window, HWND hwnd, DwmApi const& api)
    {
        auto const applied = applySystemBackdrop(window, hwnd, api);
        if (applied)
            return;
        auto const expected =
            applied.error() == TitleBandMaterialError::Unsupported
            && unsupportedBackdropReportFor(recordedBlurBehind(window)) == UnsupportedBackdropReport::Quiet;
        if (!expected)
            backdropLog()("Re-applying the system backdrop: {}.",
                          titleBandMaterialErrorName(applied.error()));
    }

    /// The frame thickness Windows would put around @p hwnd at its CURRENT monitor's DPI.
    ///
    /// AdjustWindowRectExForDpi grows an empty rectangle by the frame, so the members come back
    /// negative on the leading edges; the thickness is their magnitude.
    [[nodiscard]] FrameInset frameThicknessOf(HWND hwnd, DwmApi const& api)
    {
        if (!api.isUsable())
            return {};

        // WS_CAPTION combines WS_BORDER and WS_DLGFRAME. Keep WS_BORDER's one-pixel allowance,
        // but break the caption combination so its title height is not reserved above our chrome.
        auto const style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & ~WS_DLGFRAME;
        auto const exStyle = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        auto rect = RECT { 0, 0, 0, 0 };
        if (!api.adjustWindowRectExForDpi(&rect, style, FALSE, exStyle, api.getDpiForWindow(hwnd)))
            return {};

        // Explicit casts: RECT counts in LONG, and narrowing inside a braced initializer is an
        // error rather than a warning.
        return FrameInset { .left = static_cast<int>(-rect.left),
                            .top = static_cast<int>(-rect.top),
                            .right = static_cast<int>(rect.right),
                            .bottom = static_cast<int>(rect.bottom) };
    }

    /// Watches the windows we decorate, and answers the two messages that keep the frame invisible.
    ///
    /// There is one, shared -- see frameEventFilter() below for why that is the right scope.
    class Win32FrameEventFilter final: public QAbstractNativeEventFilter
    {
      public:
        Win32FrameEventFilter()
        {
            if (auto* app = QCoreApplication::instance())
                app->installNativeEventFilter(this);
        }

        Win32FrameEventFilter(Win32FrameEventFilter const&) = delete;
        Win32FrameEventFilter& operator=(Win32FrameEventFilter const&) = delete;
        Win32FrameEventFilter(Win32FrameEventFilter&&) = delete;
        Win32FrameEventFilter& operator=(Win32FrameEventFilter&&) = delete;

        ~Win32FrameEventFilter() override
        {
            if (auto* app = QCoreApplication::instance())
                app->removeNativeEventFilter(this);
        }

        /// Answers @p hwnd, @p window's handle, through the DWM entry points of the frame that decorated it.
        void watch(HWND hwnd, QWindow& window, DwmApi const& api)
        {
            _watched.insert_or_assign(hwnd, WatchedWindow { .window = &window, .api = api });
        }
        void forget(HWND hwnd) { _watched.erase(hwnd); }

        bool nativeEventFilter(QByteArray const& eventType, void* message, qintptr* result) override
        {
            if (eventType != "windows_generic_MSG")
                return false;

            auto* msg = static_cast<MSG*>(message);
            if (msg == nullptr || !_watched.contains(msg->hwnd))
                return false;

            switch (msg->message)
            {
                case WM_NCCALCSIZE: {
                    // wParam FALSE asks only for the client rectangle and wants the default answer.
                    if (msg->wParam == FALSE)
                        return false;

                    // Report a zero-thickness frame, which is what leaves the client area covering
                    // the whole window while DWM still treats it as framed -- and hence still
                    // shadows, rounds and snaps it.
                    //
                    // Except when maximized: Windows sizes a maximized window to the work area PLUS
                    // the frame, so a zero inset there would hang the terminal's content over the
                    // neighbouring monitor.
                    //
                    // The thickness is looked up only when it will be used: ncCalcSizeInset
                    // discards it for a normal window, and this message arrives on every step of a
                    // drag-resize.
                    // The thickness is looked up only when the answer uses it -- this message
                    // arrives on every step of a drag-resize, and AdjustWindowRectExForDpi is four
                    // calls deep. ncCalcSizeInset still owns the DECISION; this owns the laziness.
                    auto const maximized = IsZoomed(msg->hwnd) ? WindowMaximized::Yes : WindowMaximized::No;
                    auto const thickness = maximized == WindowMaximized::Yes
                                               ? frameThicknessOf(msg->hwnd, _watched.at(msg->hwnd).api)
                                               : FrameInset {};
                    auto const inset = ncCalcSizeInset(maximized, thickness);

                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                    params->rgrc[0].left += inset.left;
                    params->rgrc[0].top += inset.top;
                    params->rgrc[0].right -= inset.right;
                    params->rgrc[0].bottom -= inset.bottom;

                    // A sent message has a result to write; a dispatcher-delivered one would pass none.
                    if (result != nullptr)
                        *result = 0;
                    return true;
                }

                case WM_DWMCOMPOSITIONCHANGED:
                    // Composition restarting drops what DwmExtendFrameIntoClientArea established; the
                    // window's records say what it was (the backdrop's margins, not a fixed one pixel).
                    if (auto const& watched = _watched.at(msg->hwnd); auto* window = watched.window.data())
                        reapplySystemBackdrop(*window, msg->hwnd, watched.api);
                    return false;

                default: return false;
            }
        }

      private:
        /// A decorated window and the DWM entry points its frame speaks through.
        struct WatchedWindow
        {
            QPointer<QWindow> window; ///< Null once the window is destroyed, until it is forgotten.
            DwmApi api;               ///< The decorating frame's entry points.
        };

        /// The windows decorated, by handle.
        std::map<HWND, WatchedWindow> _watched;
    };

    /// The one filter, shared by every window.
    ///
    /// Process-scoped because a native event filter IS process-scoped: it sees every message the
    /// application receives and dispatches on msg->hwnd. One per window would have every Contour
    /// window add another qstrcmp and set probe to every mouse move in the process.
    [[nodiscard]] Win32FrameEventFilter& frameEventFilter()
    {
        static Win32FrameEventFilter filter;
        return filter;
    }

    class Win32WindowFrame final: public NativeWindowFrame
    {
      public:
        Win32WindowFrame(std::function<Qt::ColorScheme()> colorScheme, DwmApi const& api):
            _colorScheme { std::move(colorScheme) }, _api { api }
        {
        }

        void apply(QWindow& window, WindowDecoration decoration) override
        {
            auto const& api = _api;
            if (!api.isUsable())
                return;

            // Deliberately not winId(), which CREATES the platform window if there is none: the
            // window is sized while still unmapped (WindowController::showInitial), and realizing it
            // early is the trap WindowGeometry.hpp documents. A window with no handle yet is simply
            // skipped and decorated on its first expose instead.
            if (window.handle() == nullptr)
                return;

            auto* hwnd = reinterpret_cast<HWND>(window.winId());
            if (hwnd == nullptr)
                return;

            auto const policy = framePolicyFor(FramePlatform::Windows, decoration);
            if (!needsFrameApplied(window, decoration))
            {
                // The decoration stands, but the scheme its border was tinted for need not: a reload or
                // an OSC 11 flips the chrome without touching the decoration, and WindowController
                // re-applies on the palette change that follows.
                if (policy.shadow == FrameShadowStrategy::NativeFrameKept
                    && window.property(BorderSchemeKey).toInt() != schemeRecord(_colorScheme()))
                    tintBorder(window, hwnd);
                return;
            }

            if (policy.shadow != FrameShadowStrategy::NativeFrameKept)
            {
                // The native title bar: Qt's own frame is exactly right, so stop intercepting, and take
                // back the margins the kept frame extended -- the OS's frame never had them (F32).
                frameEventFilter().forget(hwnd);
                reapplySystemBackdrop(window, hwnd, api);
                return;
            }

            // Keep a real, resizable, maximizable frame. WS_CAPTION is what DWM keys its shadow and
            // its rounded corners off; WS_THICKFRAME is what keeps the window snappable and sizable.
            // Both are then made invisible by the WM_NCCALCSIZE handler above.
            // Removing the native frame grows the client area unless its size is preserved.
            RECT clientBefore {};
            bool const preserveClientSize = window.windowState() == Qt::WindowNoState && !IsZoomed(hwnd)
                                            && !IsIconic(hwnd) && GetClientRect(hwnd, &clientBefore);

            auto const style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX);

            frameEventFilter().watch(hwnd, window, api);

            // Force the WM_NCCALCSIZE round that applies the style change.
            SetWindowPos(hwnd,
                         nullptr,
                         0,
                         0,
                         0,
                         0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                             | SWP_NOOWNERZORDER);

            if (preserveClientSize)
            {
                RECT clientAfter {};
                RECT windowAfter {};
                if (GetClientRect(hwnd, &clientAfter) && GetWindowRect(hwnd, &windowAfter))
                {
                    auto const widthChange =
                        (clientBefore.right - clientBefore.left) - (clientAfter.right - clientAfter.left);
                    auto const heightChange =
                        (clientBefore.bottom - clientBefore.top) - (clientAfter.bottom - clientAfter.top);
                    if (widthChange != 0 || heightChange != 0)
                        SetWindowPos(hwnd,
                                     nullptr,
                                     0,
                                     0,
                                     static_cast<int>(windowAfter.right - windowAfter.left + widthChange),
                                     static_cast<int>(windowAfter.bottom - windowAfter.top + heightChange),
                                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
                }
            }

            // The margins that keep DWM's shadow on -- one pixel, or the whole client area under a
            // backdrop -- are the system backdrop's to decide (R-h).
            reapplySystemBackdrop(window, hwnd, api);

            tintBorder(window, hwnd);

            auto corner = DwmwcpRound;
            api.setWindowAttribute(
                hwnd, DwmwaWindowCornerPreference, &corner, static_cast<DWORD>(sizeof(corner)));
        }

        void invalidateShadow(QWindow& /*window*/) override
        {
            // DWM keeps its own shadow current; nothing to re-assert.
        }

        /// Zero: Windows draws its controls in a title bar of its own, never over our chrome.
        [[nodiscard]] qreal nativeControlsInset(QWindow& /*window*/) const override { return 0.0; }

      private:
        /// The window property recording the scheme the border was last tinted for, on the window for
        /// the reason @ref needsFrameApplied keeps its record there.
        static constexpr auto BorderSchemeKey = "contour_applied_frame_scheme";

        /// @p scheme as recorded under @ref BorderSchemeKey: +1, so "never tinted" (an absent
        /// property, which converts to 0) is distinct from every scheme.
        [[nodiscard]] static int schemeRecord(Qt::ColorScheme scheme) noexcept
        {
            return static_cast<int>(scheme) + 1;
        }

        /// Tints the border DWM draws around the rounded corners for the effective scheme.
        ///
        /// Driven from the CONFIGURED theme: it used to be forced on in BlurBehind.cpp regardless,
        /// which put a dark border on a light-themed window.
        void tintBorder(QWindow& window, HWND hwnd) const
        {
            auto const scheme = _colorScheme();
            window.setProperty(BorderSchemeKey, schemeRecord(scheme));
            // sizeof yields size_t and the attribute size is a DWORD; spelled out rather than left
            // to an implicit narrowing, which -Werror rejects on the Windows toolchains.
            auto dark = static_cast<BOOL>(scheme == Qt::ColorScheme::Dark);
            _api.setWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, &dark, static_cast<DWORD>(sizeof(dark)));
        }

        std::function<Qt::ColorScheme()> _colorScheme;
        DwmApi _api;
    };
} // namespace

std::unique_ptr<NativeWindowFrame> makeWin32WindowFrame(std::function<Qt::ColorScheme()> const& colorScheme)
{
    return makeWin32WindowFrame(colorScheme, dwmApi());
}

std::unique_ptr<NativeWindowFrame> makeWin32WindowFrame(std::function<Qt::ColorScheme()> const& colorScheme,
                                                        DwmApi const& api)
{
    // Only under the Windows platform plugin: QWindow::winId() is an HWND there and something else
    // anywhere else, and handing a non-HWND to SetWindowLongPtr is undefined. The same guard the
    // Cocoa adapter and both shadow backends carry.
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        return nullptr;

    if (!api.isUsable())
        return nullptr;

    return std::make_unique<Win32WindowFrame>(colorScheme, api);
}

} // namespace contour::platform

#endif
