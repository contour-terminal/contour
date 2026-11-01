// SPDX-License-Identifier: Apache-2.0
#include <contour/window/DragEndProbe.hpp>

#include <QtCore/QString>
#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using contour::window::PointerOverContour;
using contour::window::TrackedDragProbe;

namespace
{
/// An 800 x 600 window to track drags through. Offscreen it need not be shown: only its size, screen and
/// mapping are read.
[[nodiscard]] std::unique_ptr<QWindow> trackedWindow()
{
    auto window = std::make_unique<QWindow>();
    window->resize(800, 600);
    return window;
}
} // namespace

TEST_CASE("a tracked drag reads its end from where its window last saw it", "[contour][gui][dragend]")
{
    auto probe = TrackedDragProbe {};
    auto const window = trackedWindow();

    SECTION("never seen: nothing opens")
    {
        CHECK(probe.pointerOverContourAtEnd() == PointerOverContour::Yes);
    }

    SECTION("still over the window: a cancel")
    {
        probe.noteDragOver(*window, QPointF { 400, 300 });
        CHECK(probe.pointerOverContourAtEnd() == PointerOverContour::Yes);
    }

    SECTION("left across an edge: outside, where it was last seen on the window's screen")
    {
        probe.noteDragOver(*window, QPointF { 700, 300 });
        probe.noteDragOver(*window, QPointF { 790, 300 });
        probe.noteDragLeft(*window);
        CHECK(probe.pointerOverContourAtEnd() == PointerOverContour::No);
        CHECK(probe.dropSpot().screen == window->screen());
        CHECK(probe.dropSpot().at == window->mapToGlobal(QPoint { 790, 300 }));
    }

    SECTION("left from deep inside, as a compositor ends Escape: nothing opens")
    {
        probe.noteDragOver(*window, QPointF { 400, 300 });
        probe.noteDragLeft(*window);
        CHECK(probe.pointerOverContourAtEnd() == PointerOverContour::Yes);
    }

    SECTION("a leave of a window the drag left behind says nothing")
    {
        auto const other = trackedWindow();
        probe.noteDragOver(*window, QPointF { 790, 300 });
        probe.noteDragLeft(*window);
        probe.noteDragOver(*other, QPointF { 400, 300 });
        probe.noteDragLeft(*window);
        CHECK(probe.tracked().presence == contour::window::DragPresence::Over);
    }

    SECTION("forgotten once decided: the next drag starts unseen")
    {
        probe.noteDragOver(*window, QPointF { 790, 300 });
        probe.noteDragLeft(*window);
        probe.forgetDrag();
        CHECK(probe.tracked().presence == contour::window::DragPresence::Unseen);
        CHECK(probe.dropSpot().screen == nullptr);
    }
}

TEST_CASE("the drag-end probe is chosen by the platform Qt runs on", "[contour][gui][dragend]")
{
    CHECK(dynamic_cast<TrackedDragProbe*>(contour::window::makeDragEndProbe(QStringLiteral("wayland")).get())
          != nullptr);
    CHECK(dynamic_cast<TrackedDragProbe*>(
              contour::window::makeDragEndProbe(QStringLiteral("wayland-egl")).get())
          != nullptr);
    for (auto const* const platform: { "xcb", "windows", "cocoa", "offscreen" })
        CHECK(dynamic_cast<contour::window::TopLevelProbe*>(
                  contour::window::makeDragEndProbe(QString::fromUtf8(platform)).get())
              != nullptr);
}
