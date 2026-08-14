// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32WindowFrame.hpp>

#ifdef _WIN32

    #include <contour/platform/Dwmapi.hpp>

    #include <QtCore/QAbstractNativeEventFilter>
    #include <QtCore/QCoreApplication>
    #include <QtGui/QWindow>

    #include <map>
    #include <set>
    #include <utility>

namespace contour::platform
{

namespace
{
    /// The frame thickness Windows would put around @p hwnd at its CURRENT monitor's DPI.
    ///
    /// AdjustWindowRectExForDpi grows an empty rectangle by the frame, so the members come back
    /// negative on the leading edges; the thickness is their magnitude.
    [[nodiscard]] FrameInset frameThicknessOf(HWND hwnd)
    {
        auto const& api = dwmApi();
        if (!api.isUsable())
            return {};

        auto const style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
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

        void watch(HWND hwnd) { _watched.insert(hwnd); }
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
                    auto const maximized = IsZoomed(msg->hwnd) ? WindowMaximized::Yes : WindowMaximized::No;
                    auto const inset = ncCalcSizeInset(
                        maximized,
                        maximized == WindowMaximized::Yes ? frameThicknessOf(msg->hwnd) : FrameInset {});

                    auto* params = reinterpret_cast<NCCALCSIZE_PARAMS*>(msg->lParam);
                    params->rgrc[0].left += inset.left;
                    params->rgrc[0].top += inset.top;
                    params->rgrc[0].right -= inset.right;
                    params->rgrc[0].bottom -= inset.bottom;

                    *result = 0;
                    return true;
                }

                case WM_DWMCOMPOSITIONCHANGED:
                    // Composition restarting drops what DwmExtendFrameIntoClientArea established.
                    extendFrame(msg->hwnd);
                    return false;

                default: return false;
            }
        }

        /// Asks DWM to render its frame into the client area.
        ///
        /// One pixel at the top, not zero: a zero margin disables the extension outright, and a
        /// window DWM is not extending into is one it does not shadow. One pixel is the smallest
        /// that keeps it on, and it sits under our own title bar where nothing shows.
        static void extendFrame(HWND hwnd)
        {
            auto const& api = dwmApi();
            if (api.extendFrameIntoClientArea == nullptr)
                return;
            auto const margins =
                MARGINS { .cxLeftWidth = 0, .cxRightWidth = 0, .cyTopHeight = 1, .cyBottomHeight = 0 };
            api.extendFrameIntoClientArea(hwnd, &margins);
        }

      private:
        std::set<HWND> _watched;
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
        explicit Win32WindowFrame(std::function<Qt::ColorScheme()> colorScheme):
            _colorScheme { std::move(colorScheme) }
        {
        }

        void apply(QWindow& window, WindowDecoration decoration) override
        {
            auto const& api = dwmApi();
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

            // Idempotent, and it has to be: this is re-applied on every expose, and the work below
            // is not free -- SWP_FRAMECHANGED forces a fresh WM_NCCALCSIZE round and can queue the
            // repaint that produces the next expose, which would feed itself during a drag-resize.
            if (auto const seen = _applied.find(hwnd); seen != _applied.end() && seen->second == decoration)
                return;
            _applied[hwnd] = decoration;

            auto const policy = framePolicyFor(FramePlatform::Windows, decoration);
            if (policy.shadow != FrameShadowStrategy::NativeFrameKept)
            {
                // The native title bar: Qt's own frame is exactly right, so stop intercepting.
                frameEventFilter().forget(hwnd);
                return;
            }

            // Keep a real, resizable, maximizable frame. WS_CAPTION is what DWM keys its shadow and
            // its rounded corners off; WS_THICKFRAME is what keeps the window snappable and sizable.
            // Both are then made invisible by the WM_NCCALCSIZE handler above.
            auto const style = GetWindowLongPtrW(hwnd, GWL_STYLE);
            SetWindowLongPtrW(hwnd, GWL_STYLE, style | WS_THICKFRAME | WS_CAPTION | WS_MAXIMIZEBOX);

            frameEventFilter().watch(hwnd);

            // Force the WM_NCCALCSIZE round that applies the style change.
            SetWindowPos(hwnd,
                         nullptr,
                         0,
                         0,
                         0,
                         0,
                         SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE
                             | SWP_NOOWNERZORDER);

            Win32FrameEventFilter::extendFrame(hwnd);

            // The border DWM draws around the rounded corners is tinted by this. Driven from the
            // CONFIGURED theme: it used to be forced on in BlurBehind.cpp regardless, which put a
            // dark border on a light-themed window.
            auto dark = static_cast<BOOL>(_colorScheme() == Qt::ColorScheme::Dark);
            api.setWindowAttribute(hwnd, DwmwaUseImmersiveDarkMode, &dark, sizeof(dark));

            auto corner = DwmwcpRound;
            api.setWindowAttribute(hwnd, DwmwaWindowCornerPreference, &corner, sizeof(corner));
        }

        void invalidateShadow(QWindow& /*window*/) override
        {
            // DWM keeps its own shadow current; nothing to re-assert.
        }

        /// Zero: Windows draws its controls in a title bar of its own, never over our chrome.
        [[nodiscard]] qreal nativeControlsInset(QWindow& /*window*/) const override { return 0.0; }

      private:
        std::function<Qt::ColorScheme()> _colorScheme;
        std::map<HWND, WindowDecoration> _applied;
    };
} // namespace

std::unique_ptr<NativeWindowFrame> makeWin32WindowFrame(std::function<Qt::ColorScheme()> const& colorScheme)
{
    if (!dwmApi().isUsable())
        return std::make_unique<NullWindowFrame>();
    return std::make_unique<Win32WindowFrame>(colorScheme);
}

} // namespace contour::platform

#endif
