// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/Win32CaptionButtons.hpp>

#ifdef _WIN32

    #include <contour/platform/CaptionHitTest.hpp>
    #include <contour/platform/QtInvoke.hpp>
    #include <contour/platform/QtPlatformPlugin.hpp>

    #include <QtCore/QAbstractNativeEventFilter>
    #include <QtCore/QByteArray>
    #include <QtCore/QCoreApplication>
    #include <QtCore/QPointer>
    #include <QtGui/QWindow>

    #include <algorithm>
    #include <array>
    #include <optional>
    #include <utility>
    #include <vector>

    #include <Windows.h>
    #include <windowsx.h>

namespace contour::platform
{

namespace
{
    /// A non-client mouse message, and the event the transition table knows it as.
    struct NcMessage
    {
        UINT message = 0;                   ///< The Win32 message.
        NcEvent event = NcEvent::MouseMove; ///< What onNcEvent calls it.
    };

    /// The messages the filter hands to onNcEvent; every other message passes by.
    constexpr auto NcMessages = std::array {
        NcMessage { .message = WM_NCMOUSEMOVE, .event = NcEvent::MouseMove },
        NcMessage { .message = WM_NCMOUSELEAVE, .event = NcEvent::MouseLeave },
        NcMessage { .message = WM_NCLBUTTONDOWN, .event = NcEvent::LeftDown },
        NcMessage { .message = WM_NCLBUTTONDBLCLK, .event = NcEvent::LeftDoubleClick },
        NcMessage { .message = WM_NCLBUTTONUP, .event = NcEvent::LeftUp },
    };
    static_assert(NcMessages.size() == NcEventCount, "every NcEvent needs exactly one Win32 message");

    /// @param message A Win32 message.
    /// @return The event @p message is, or nothing when the filter does not handle it.
    [[nodiscard]] std::optional<NcEvent> ncEventOf(UINT message) noexcept
    {
        auto const found = std::ranges::find(NcMessages, message, &NcMessage::message);
        if (found == NcMessages.end())
            return std::nullopt;
        return found->event;
    }

    /// @return Where @p window stands, as far as its caption cares.
    [[nodiscard]] WindowPlacement placementOf(QWindow const& window, HWND hwnd)
    {
        if (IsZoomed(hwnd))
            return WindowPlacement::Maximized;
        if (window.windowStates().testFlag(Qt::WindowFullScreen))
            return WindowPlacement::FullScreen;
        return WindowPlacement::Windowed;
    }

    /// @return The monitor @p hwnd stands on, in its logical client coordinates (@p devicePixelRatio), or an
    ///         empty rectangle when it cannot be read.
    [[nodiscard]] LogicalRect monitorOf(HWND hwnd, double devicePixelRatio)
    {
        auto info = MONITORINFO {};
        info.cbSize = static_cast<DWORD>(sizeof(info));
        if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &info))
            return {};
        auto topLeftPoint = POINT { .x = info.rcMonitor.left, .y = info.rcMonitor.top };
        auto bottomRightPoint = POINT { .x = info.rcMonitor.right, .y = info.rcMonitor.bottom };
        if (!ScreenToClient(hwnd, &topLeftPoint) || !ScreenToClient(hwnd, &bottomRightPoint))
            return {};
        auto const logical = [devicePixelRatio](POINT point) {
            return toLogical(PhysicalPoint { .x = static_cast<int>(point.x), .y = static_cast<int>(point.y) },
                             devicePixelRatio);
        };
        auto const topLeft = logical(topLeftPoint);
        auto const bottomRight = logical(bottomRightPoint);
        return LogicalRect { .x = topLeft.x,
                             .y = topLeft.y,
                             .width = bottomRight.x - topLeft.x,
                             .height = bottomRight.y - topLeft.y };
    }

    /// One attached window: what it lent, and how its maximise button looks now.
    struct CaptionEntry
    {
        QPointer<QWindow> window {};                            ///< Null once the window is destroyed.
        CaptionBinding binding {};                              ///< What the window lent.
        CaptionButtonVisual visual = CaptionButtonVisual::Rest; ///< The button's look now.
    };

    /// Answers the attached windows' caption messages.
    ///
    /// Only the maximise button is non-client (HTMAXBUTTON): that one hit is what Snap Layouts keys on, and
    /// minimise and close stay QML buttons with the scene's own hover, press, focus and accessibility. Drag
    /// and resize stay with TitleBar.qml's drag region and ResizeBorder.qml.
    ///
    /// Beside Win32FrameEventFilter, which owns WM_NCCALCSIZE and WM_DWMCOMPOSITIONCHANGED: this one owns
    /// WM_NCHITTEST and the WM_NC* mouse messages. The two sets are disjoint, so the order Qt runs the
    /// filters in (the reverse of their installation) decides nothing.
    class Win32CaptionEventFilter final: public QAbstractNativeEventFilter
    {
      public:
        Win32CaptionEventFilter()
        {
            if (auto* app = QCoreApplication::instance())
                app->installNativeEventFilter(this);
        }

        Win32CaptionEventFilter(Win32CaptionEventFilter const&) = delete;
        Win32CaptionEventFilter& operator=(Win32CaptionEventFilter const&) = delete;
        Win32CaptionEventFilter(Win32CaptionEventFilter&&) = delete;
        Win32CaptionEventFilter& operator=(Win32CaptionEventFilter&&) = delete;

        ~Win32CaptionEventFilter() override
        {
            if (auto* app = QCoreApplication::instance())
                app->removeNativeEventFilter(this);
        }

        /// Answers @p window from @p binding, replacing an earlier binding. @pre GUI thread.
        void attach(QWindow& window, CaptionBinding binding)
        {
            forgetDestroyed();
            if (auto* const entry = entryOf(&window))
            {
                entry->binding = std::move(binding);
                return;
            }
            _entries.push_back(CaptionEntry {
                .window = &window, .binding = std::move(binding), .visual = CaptionButtonVisual::Rest });
        }

        /// Stops answering @p window; a window never attached is ignored. @pre GUI thread.
        void detach(QWindow& window)
        {
            std::erase_if(_entries, [&window](CaptionEntry const& entry) {
                return entry.window.isNull() || entry.window.data() == &window;
            });
        }

        bool nativeEventFilter(QByteArray const& eventType, void* message, qintptr* result) override
        {
            if (eventType != "windows_generic_MSG")
                return false;

            auto const* msg = static_cast<MSG const*>(message);
            if (msg == nullptr)
                return false;
            if (msg->message == WM_NCHITTEST)
                return answerHitTest(*msg, result);
            if (auto const event = ncEventOf(msg->message))
                return answerNcMouse(*msg, *event, result);
            return false;
        }

      private:
        /// Drops the entries of windows destroyed while attached.
        void forgetDestroyed()
        {
            std::erase_if(_entries, [](CaptionEntry const& entry) { return entry.window.isNull(); });
        }

        /// @return @p window's entry, or nullptr.
        [[nodiscard]] CaptionEntry* entryOf(QWindow const* window)
        {
            if (window == nullptr)
                return nullptr;
            auto const found = std::ranges::find_if(
                _entries, [window](CaptionEntry const& entry) { return entry.window.data() == window; });
            return found == _entries.end() ? nullptr : &*found;
        }

        /// @return The entry of the window whose platform window is @p hwnd, or nullptr.
        [[nodiscard]] CaptionEntry* entryOfHandle(HWND hwnd)
        {
            auto const found = std::ranges::find_if(_entries, [hwnd](CaptionEntry const& entry) {
                auto const* window = entry.window.data();
                // handle() first: winId() would create a platform window that does not exist yet.
                return window != nullptr && window->handle() != nullptr
                       && reinterpret_cast<HWND>(window->winId()) == hwnd;
            });
            return found == _entries.end() ? nullptr : &*found;
        }

        /// WM_NCHITTEST: HTMAXBUTTON over the maximise button, the default answer anywhere else.
        bool answerHitTest(MSG const& msg, qintptr* result)
        {
            auto* const entry = entryOfHandle(msg.hwnd);
            if (entry == nullptr || !entry->binding.layout)
                return false;

            // The message carries a screen point in physical pixels; the layout is the scene's, logical.
            auto point = POINT { .x = GET_X_LPARAM(msg.lParam), .y = GET_Y_LPARAM(msg.lParam) };
            if (!ScreenToClient(msg.hwnd, &point))
                return false;
            auto const logical =
                toLogical(PhysicalPoint { .x = static_cast<int>(point.x), .y = static_cast<int>(point.y) },
                          entry->window->devicePixelRatio());
            auto const placement = placementOf(*entry->window, msg.hwnd);
            // Windowed, the monitor is not read: no need to ask Windows for it on every pointer move.
            auto const monitor = placement == WindowPlacement::Windowed
                                     ? LogicalRect {}
                                     : monitorOf(msg.hwnd, entry->window->devicePixelRatio());
            auto const hit = captionHitAt(logical, entry->binding.layout(), placement, monitor);
            if (hit != CaptionHit::MaximizeButton)
                return false;

            // A sent message, so Qt's window procedure passes a result to write; guarded like answerNcMouse.
            if (result != nullptr)
                *result = HTMAXBUTTON;
            return true;
        }

        /// The non-client mouse messages: the maximise button's state machine (onNcEvent).
        bool answerNcMouse(MSG const& msg, NcEvent event, qintptr* result)
        {
            auto* const entry = entryOfHandle(msg.hwnd);
            if (entry == nullptr)
                return false;

            // The message's wParam is the hit-test code of where it landed; a leave carries none.
            auto const hit = event != NcEvent::MouseLeave && msg.wParam == static_cast<WPARAM>(HTMAXBUTTON)
                                 ? CaptionHit::MaximizeButton
                                 : CaptionHit::PassThrough;
            auto const transition = onNcEvent(event, hit, entry->visual);
            auto const changed = transition.visual != entry->visual;
            entry->visual = transition.visual;

            // Copied before any call out: a callback may detach the window, destroying the entry it came
            // from.
            auto const window = entry->window;
            auto const visual = entry->binding.visual;

            switch (transition.effect)
            {
                case NcEffect::None: break;
                case NcEffect::TrackLeave: trackNonClientLeave(msg.hwnd); break;
                case NcEffect::ToggleMaximize: queueToggle(window); break;
            }
            if (changed && visual)
                visual(transition.visual);

            if (transition.disposition == NcDisposition::DefaultHandling)
                return false;
            // Queued input: Qt's event dispatcher is the only filter caller, and it passes no result.
            // Swallowing the message needs only the true.
            if (result != nullptr)
                *result = 0;
            return true;
        }

        /// Toggles @p window once its message is handled.
        ///
        /// Queued, so the window is not maximised or restored inside its own message. Looked up again when
        /// it runs, so a window detached in the meantime -- its controller gone, show_title_bar turned on --
        /// drops the toggle rather than calling into a binding that no longer stands.
        void queueToggle(QPointer<QWindow> const& window)
        {
            if (window.isNull())
                return;
            postToObject(window.data(), [this, window] {
                auto* const entry = entryOf(window.data());
                if (entry == nullptr || !entry->binding.toggleMaximize)
                    return;
                auto const toggle = entry->binding.toggleMaximize;
                toggle();
            });
        }

        /// Asks for WM_NCMOUSELEAVE once the pointer leaves @p hwnd's non-client area.
        static void trackNonClientLeave(HWND hwnd)
        {
            auto track = TRACKMOUSEEVENT { .cbSize = static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)),
                                           .dwFlags = TME_LEAVE | TME_NONCLIENT,
                                           .hwndTrack = hwnd,
                                           .dwHoverTime = 0 };
            TrackMouseEvent(&track);
        }

        std::vector<CaptionEntry> _entries;
    };

    /// The one filter, shared by every window: a native event filter is process-scoped (@see
    /// Win32WindowFrame.cpp's frameEventFilter). Built on the first attach, which finds the application.
    [[nodiscard]] Win32CaptionEventFilter& captionEventFilter()
    {
        static Win32CaptionEventFilter filter;
        return filter;
    }

    class Win32CaptionButtons final: public INativeCaptionButtons
    {
      public:
        void attach(QWindow& window, CaptionBinding binding) override
        {
            if (onNativeWindowsPlatform())
                captionEventFilter().attach(window, std::move(binding));
        }

        void detach(QWindow& window) override
        {
            if (onNativeWindowsPlatform())
                captionEventFilter().detach(window);
        }
    };
} // namespace

std::unique_ptr<INativeCaptionButtons> makeWin32CaptionButtons()
{
    return std::make_unique<Win32CaptionButtons>();
}

} // namespace contour::platform

#endif
