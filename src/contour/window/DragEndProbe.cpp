// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DragEndProbe.hpp>

#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

namespace contour::window
{

PointerOverContour TopLevelProbe::pointerOverContourAtEnd() const
{
    return QGuiApplication::topLevelAt(QCursor::pos()) != nullptr ? PointerOverContour::Yes
                                                                  : PointerOverContour::No;
}

DropSpot TopLevelProbe::dropSpot() const
{
    auto const at = QCursor::pos();
    return DropSpot { .screen = QGuiApplication::screenAt(at), .at = at };
}

void TrackedDragProbe::noteDragOver(QWindow const& window, QPointF local)
{
    auto const at = DropPoint { .x = local.x(), .y = local.y() };
    // The first sighting in a window has no motion before it to extrapolate.
    _last.before = &window == _window && _presence == DragPresence::Over ? _last.at : at;
    _last.at = at;
    _last.window = DropExtent { .width = static_cast<double>(window.width()),
                                .height = static_cast<double>(window.height()) };
    _window = &window;
    _presence = DragPresence::Over;
    _screen = window.screen();
    _global = window.mapToGlobal(local.toPoint());
}

void TrackedDragProbe::noteDragLeft(QWindow const& window)
{
    // A leave of a window the drag has since entered another one past says nothing of where it is now.
    if (&window != _window || _presence != DragPresence::Over)
        return;
    _presence = DragPresence::Left;
}

TrackedDrag TrackedDragProbe::tracked() const
{
    return TrackedDrag { .presence = _presence, .last = _last };
}

PointerOverContour TrackedDragProbe::pointerOverContourAtEnd() const
{
    return classifyTrackedDrag(tracked());
}

DropSpot TrackedDragProbe::dropSpot() const
{
    return DropSpot { .screen = _screen.data(), .at = _global };
}

void TrackedDragProbe::forgetDrag()
{
    _window = nullptr;
    _presence = DragPresence::Unseen;
    _last = {};
    _screen.clear();
    _global = {};
}

std::unique_ptr<DragEndProbe> makeDragEndProbe(QString const& platformName)
{
    if (platformName.startsWith(QStringLiteral("wayland")))
        return std::make_unique<TrackedDragProbe>();
    return std::make_unique<TopLevelProbe>();
}

} // namespace contour::window
