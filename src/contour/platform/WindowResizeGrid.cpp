// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowResizeGrid.hpp>
#include <contour/platform/XcbProperty.hpp>

#include <QtGui/QWindow>

#include <utility>

#ifdef CONTOUR_FRONTEND_XCB
    #include <array>
    #include <cstring>
#endif

#ifdef _WIN32
    #include <core/log/LogStore.hpp>

    #include <QtCore/QEvent>
    #include <QtCore/QPointer>
    #include <QtCore/qt_windows.h>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QPlatformSurfaceEvent>

    #include <commctrl.h>
#endif

namespace contour::platform
{

#ifdef _WIN32
namespace
{
    /// Owns a per-HWND subclass: Qt's application-wide filters skip synchronous WM_SIZING.
    class WindowResizeGrid final: public QObject
    {
      public:
        WindowResizeGrid(QWindow& window,
                         std::function<std::optional<geometry::RowResizeGrid>()> grid,
                         std::function<void(WindowMoveResizePhase)> interactionChanged):
            _window { &window },
            _grid { std::move(grid) },
            _interactionChanged { std::move(interactionChanged) }
        {
            window.installEventFilter(this);
            attach();
        }

        WindowResizeGrid(WindowResizeGrid const&) = delete;
        WindowResizeGrid& operator=(WindowResizeGrid const&) = delete;
        WindowResizeGrid(WindowResizeGrid&&) = delete;
        WindowResizeGrid& operator=(WindowResizeGrid&&) = delete;

        ~WindowResizeGrid() override
        {
            detach();
            if (_window)
                _window->removeEventFilter(this);
        }

        bool eventFilter(QObject* watched, QEvent* event) override // Qt's signature, public as in QObject
        {
            if (watched != _window)
                return false;
            if (event->type() == QEvent::PlatformSurface)
            {
                auto const type = static_cast<QPlatformSurfaceEvent*>(event)->surfaceEventType();
                if (type == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed)
                    detach();
                else
                    attach();
            }
            else if (event->type() == QEvent::Expose)
                attach();
            return false;
        }

      private:
        void attach()
        {
            if (_nativeWindow != nullptr || _window == nullptr || _window->handle() == nullptr)
                return;
            auto const hwnd = reinterpret_cast<HWND>(_window->winId());
            if (SetWindowSubclass(
                    hwnd, windowProc, reinterpret_cast<UINT_PTR>(this), reinterpret_cast<DWORD_PTR>(this)))
                _nativeWindow = hwnd;
            else
                errorLog()("Could not install window row snapping.");
        }

        void detach()
        {
            if (auto const hwnd = std::exchange(_nativeWindow, nullptr))
                RemoveWindowSubclass(hwnd, windowProc, reinterpret_cast<UINT_PTR>(this));
        }

        static LRESULT CALLBACK windowProc(HWND hwnd,
                                           UINT message,
                                           WPARAM wParam,
                                           LPARAM lParam,
                                           [[maybe_unused]] UINT_PTR id,
                                           DWORD_PTR reference)
        {
            auto& self = *reinterpret_cast<WindowResizeGrid*>(reference);
            if (self._interactionChanged && (message == WM_ENTERSIZEMOVE || message == WM_EXITSIZEMOVE))
                self._interactionChanged(message == WM_ENTERSIZEMOVE ? WindowMoveResizePhase::Active
                                                                     : WindowMoveResizePhase::Idle);
            if (message == WM_SIZING)
                if (auto const result = self.sizingResult(wParam, lParam))
                    return *result;
            if (message == WM_NCDESTROY)
                self.detach();
            return DefSubclassProc(hwnd, message, wParam, lParam);
        }

        [[nodiscard]] std::optional<LRESULT> sizingResult(WPARAM edge, LPARAM rectangle)
        {
            if (_window == nullptr || _window->windowState() != Qt::WindowNoState || rectangle == 0)
                return std::nullopt;

            auto const top = edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT;
            auto const bottom = edge == WMSZ_BOTTOM || edge == WMSZ_BOTTOMLEFT || edge == WMSZ_BOTTOMRIGHT;
            if (!top && !bottom)
                return std::nullopt;
            auto const grid = _grid();
            if (!grid || grid->cellHeight <= 0)
                return std::nullopt;

            RECT windowRect {};
            RECT clientRect {};
            if (!GetWindowRect(_nativeWindow, &windowRect) || !GetClientRect(_nativeWindow, &clientRect))
                return std::nullopt;
            // Measure the actual native frame: the client-decorated window removes it, whereas a
            // native title bar keeps it. The proposed RECT includes it; the row geometry does not.
            auto const frameHeight =
                (windowRect.bottom - windowRect.top) - (clientRect.bottom - clientRect.top);
            auto& proposed = *reinterpret_cast<RECT*>(rectangle);
            auto const height = geometry::snapHeightToRows(
                                    static_cast<int>(proposed.bottom - proposed.top - frameHeight), *grid)
                                + frameHeight;
            if (top)
                proposed.top = proposed.bottom - height;
            else
                proposed.bottom = proposed.top + height;
            return TRUE;
        }

        QPointer<QWindow> _window;
        std::function<std::optional<geometry::RowResizeGrid>()> _grid;
        std::function<void(WindowMoveResizePhase)> _interactionChanged;
        HWND _nativeWindow = nullptr;
    };
} // namespace
#endif

std::unique_ptr<QObject> makeWindowResizeGrid(
    [[maybe_unused]] QWindow& window,
    [[maybe_unused]] std::function<std::optional<geometry::RowResizeGrid>()> const& grid,
    [[maybe_unused]] std::function<void(WindowMoveResizePhase)> const& interactionChanged)
{
#ifdef _WIN32
    if (QGuiApplication::platformName() != QStringLiteral("windows"))
        return nullptr;
    return std::make_unique<WindowResizeGrid>(window, grid, interactionChanged);
#elifdef __APPLE__
    return makeCocoaWindowResizeGrid(window, grid);
#else
    return nullptr;
#endif
}

void applyRowResizeHints([[maybe_unused]] QWindow& window,
                         [[maybe_unused]] std::optional<geometry::RowResizeGrid> grid)
{
#ifdef CONTOUR_FRONTEND_XCB
    if ((grid && grid->cellHeight <= 0) || window.handle() == nullptr)
        return;
    auto const info = queryXcbPropertyInfo(&window, "WM_NORMAL_HINTS");
    if (!info)
        return;

    // ICCCM WM_SIZE_HINTS is eighteen 32-bit words. Keep Qt's width, min/max, aspect and gravity
    // fields; only the row increment and its origin need physical-pixel precision at fractional DPR.
    std::array<uint32_t, 18> hints {};
    auto const cookie = xcb_get_property(info->connection,
                                         0,
                                         info->window,
                                         info->propertyAtom,
                                         XCB_ATOM_WM_SIZE_HINTS,
                                         0,
                                         static_cast<uint32_t>(hints.size()));
    auto const reply =
        XcbReply<xcb_get_property_reply_t> { xcb_get_property_reply(info->connection, cookie, nullptr) };
    if (!reply || reply->format != 32 || reply->type != XCB_ATOM_WM_SIZE_HINTS
        || xcb_get_property_value_length(reply.get()) != static_cast<int>(sizeof(hints)))
        return;
    std::memcpy(hints.data(), xcb_get_property_value(reply.get()), sizeof(hints));
    constexpr uint32_t ResizeIncrement = 0b0100'0000;
    constexpr uint32_t BaseSize = 0b0001'0000'0000;
    hints[0] |= ResizeIncrement | BaseSize;
    // Qt does not republish unchanged cached setters after our native override. Restore the
    // current Qt values explicitly when row snapping is disabled (including after a DPI change).
    auto const scale = window.devicePixelRatio();
    hints[10] =
        static_cast<uint32_t>(grid ? grid->cellHeight : qRound(window.sizeIncrement().height() * scale));
    hints[16] = static_cast<uint32_t>(grid ? grid->baseHeight : qRound(window.baseSize().height() * scale));
    xcb_change_property(info->connection,
                        XCB_PROP_MODE_REPLACE,
                        info->window,
                        info->propertyAtom,
                        XCB_ATOM_WM_SIZE_HINTS,
                        32,
                        static_cast<uint32_t>(hints.size()),
                        hints.data());
    xcb_flush(info->connection);
#endif
}

} // namespace contour::platform
