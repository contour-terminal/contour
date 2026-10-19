// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowResizeGrid.hpp>

#include <QtCore/QPointer>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <cmath>
#include <utility>

#import <AppKit/AppKit.h>

namespace contour::platform
{
namespace
{
    /// Owns a notification subscription without replacing Qt's NSWindow delegate.
    class CocoaWindowResizeGrid final: public QObject
    {
      public:
        CocoaWindowResizeGrid(QWindow& window, std::function<std::optional<geometry::RowResizeGrid>()> grid):
            _window { &window }, _grid { std::move(grid) }
        {
            _observer =
                [[NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEndLiveResizeNotification
                                                                 object:nil
                                                                  queue:nil
                                                             usingBlock:^(NSNotification* notification) {
                                                               this->snap(notification.object);
                                                             }] retain];
        }

        CocoaWindowResizeGrid(CocoaWindowResizeGrid const&) = delete;
        CocoaWindowResizeGrid& operator=(CocoaWindowResizeGrid const&) = delete;
        CocoaWindowResizeGrid(CocoaWindowResizeGrid&&) = delete;
        CocoaWindowResizeGrid& operator=(CocoaWindowResizeGrid&&) = delete;

        ~CocoaWindowResizeGrid() override
        {
            [NSNotificationCenter.defaultCenter removeObserver:_observer];
            [_observer release];
        }

      private:
        void snap(NSWindow* nativeWindow)
        {
            if (_window == nullptr || _window->handle() == nullptr
                || _window->windowState() != Qt::WindowNoState || nativeWindow.zoomed
                || (nativeWindow.styleMask & NSWindowStyleMaskFullScreen) != 0)
                return;
            auto* view = reinterpret_cast<NSView*>(_window->winId());
            if (view.window != nativeWindow)
                return;
            auto const grid = _grid();
            if (!grid || grid->cellHeight <= 0)
                return;

            // AppKit notifications can arrive before Qt publishes its final geometry. Read the
            // actual content view rather than snapping a possibly stale QWindow::height().
            auto const currentHeight = view.bounds.size.height;
            auto const height = geometry::snapLogicalHeightToRows(
                static_cast<int>(std::round(currentHeight)), _window->devicePixelRatio(), *grid);
            if (height == currentHeight)
                return;
            auto frame = nativeWindow.frame;
            auto const delta = height - currentHeight;
            // AppKit's origin is at the bottom. Keep the top edge and the width fixed.
            frame.origin.y -= delta;
            frame.size.height += delta;
            [nativeWindow setFrame:frame display:YES];
        }

        QPointer<QWindow> _window;
        std::function<std::optional<geometry::RowResizeGrid>()> _grid;
        id _observer = nil;
    };
} // namespace

std::unique_ptr<QObject> makeCocoaWindowResizeGrid(
    QWindow& window, std::function<std::optional<geometry::RowResizeGrid>()> grid)
{
    if (QGuiApplication::platformName() != QStringLiteral("cocoa"))
        return nullptr;
    return std::make_unique<CocoaWindowResizeGrid>(window, std::move(grid));
}
} // namespace contour::platform
