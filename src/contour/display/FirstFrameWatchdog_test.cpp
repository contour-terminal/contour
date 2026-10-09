// SPDX-License-Identifier: Apache-2.0
#include <contour/display/FirstFrameWatchdog.hpp>

#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <QtTest/QTest>

using namespace std::chrono_literals;

TEST_CASE("FirstFrameWatchdog: no frame within the budget reports failure exactly once", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 20ms, [&] { ++failures; });
    QTest::qWait(120);
    CHECK(failures == 1);
}

TEST_CASE("FirstFrameWatchdog: a swapped frame disarms it", "[gpu]")
{
    auto window = QQuickWindow {};
    auto failures = 0;
    auto const watchdog = contour::display::FirstFrameWatchdog(window, 50ms, [&] { ++failures; });
    QMetaObject::invokeMethod(&window, "frameSwapped");
    QTest::qWait(150);
    CHECK(failures == 0);
}
