// SPDX-License-Identifier: Apache-2.0
#include <contour/display/FirstFrameWatchdog.hpp>

#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <QtTest/QTest>

using namespace std::chrono_literals;

// The offscreen platform really draws an exposed window, and that swapped frame would disarm the
// watchdog. Blocking the window's signals (the Expose event the watchdog filters is no signal) stands in
// for a GPU that never completes a frame.
TEST_CASE("FirstFrameWatchdog: no frame within the budget of exposure reports failure exactly once", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 20ms, [&] { ++failures; });
    auto const blocker = QSignalBlocker(&window);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(120);
    CHECK(failures == 1);
}

TEST_CASE("FirstFrameWatchdog: an already exposed window starts its budget at once", "[gpu]")
{
    auto window = QQuickWindow {};
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(100); // let the first frames go by
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 20ms, [&] { ++failures; });
    auto const blocker = QSignalBlocker(&window);
    QTest::qWait(120);
    CHECK(failures == 1);
}

TEST_CASE("FirstFrameWatchdog: a window that is never exposed never fails", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 20ms, [&] { ++failures; });
    QTest::qWait(150);
    CHECK(failures == 0);
}

TEST_CASE("FirstFrameWatchdog: a swapped frame disarms it", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 50ms, [&] { ++failures; });
    QMetaObject::invokeMethod(&window, "frameSwapped");
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(150);
    CHECK(failures == 0);
}

TEST_CASE("FirstFrameWatchdog: a scene-graph error reports failure exactly once", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 20ms, [&] { ++failures; });
    QMetaObject::invokeMethod(&window,
                              "sceneGraphError",
                              Q_ARG(QQuickWindow::SceneGraphError, QQuickWindow::ContextNotAvailable),
                              Q_ARG(QString, QStringLiteral("x")));
    CHECK(failures == 1);
    window.show();
    REQUIRE(QTest::qWaitForWindowExposed(&window));
    QTest::qWait(120);
    CHECK(failures == 1);
}
