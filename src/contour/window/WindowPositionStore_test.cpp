// SPDX-License-Identifier: Apache-2.0
#include <contour/window/WindowPositionStore.hpp>

#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <limits>

using namespace contour::window;

TEST_CASE("remembered window positions stay reachable after monitor changes", "[contour][window-position]")
{
    auto const available = QRect(-1920, 40, 1920, 1040);
    CHECK(reachableWindowPosition({ 120, 60 }, { 800, 600 }, available) == QPoint(-1800, 100));
    CHECK(reachableWindowPosition({ 4000, -1000 }, { 800, 600 }, available) == QPoint(-800, 40));
    CHECK(reachableWindowPosition({ -4000, 2000 }, { 800, 600 }, available) == QPoint(-1920, 480));
    CHECK(reachableWindowPosition({ 4000, 2000 }, { 3000, 2000 }, available) == available.topLeft());
}

TEST_CASE("restored native frames fit the work area for arbitrary saved positions",
          "[contour][window-position]")
{
    auto const margins = QMargins(8, 31, 8, 8);
    auto const size = QSize(800, 600);
    auto const offsets = std::array {
        QPoint(std::numeric_limits<int>::min(), std::numeric_limits<int>::min()),
        QPoint(std::numeric_limits<int>::max(), std::numeric_limits<int>::max()),
        QPoint(std::numeric_limits<int>::min(), std::numeric_limits<int>::max()),
        QPoint(std::numeric_limits<int>::max(), std::numeric_limits<int>::min()),
        QPoint(120, 60),
        QPoint(0, 0),
    };
    for (auto const available:
         { QRect(0, 40, 1920, 1040), QRect(-1920, 40, 1920, 1040), QRect(1920, -1080, 1920, 1040) })
        for (auto const requested: offsets)
        {
            auto const position = reachableWindowPosition(requested, size, available, margins);
            auto const frame = QRect(position, size).marginsAdded(margins);
            CHECK(available.contains(frame));
        }

    auto const available = QRect(-1920, 40, 1920, 1040);
    CHECK(reachableWindowPosition({ -4000, -4000 }, size, available, margins) == QPoint(-1912, 71));
    CHECK(reachableWindowPosition({ 4000, 4000 }, size, available, margins) == QPoint(-808, 472));
    // Position clamping cannot shrink an oversized configured grid: keep its frame's leading edge
    // (and therefore its title bar) reachable instead of moving the controls beyond the monitor.
    CHECK(reachableWindowPosition({ 4000, -4000 }, { 3000, 2000 }, available, margins)
          == available.topLeft() + QPoint(8, 31));
}

TEST_CASE("window position side file roundtrips and rejects malformed coordinates",
          "[contour][window-position]")
{
    QTemporaryDir directory;
    REQUIRE(directory.isValid());
    auto const path = std::filesystem::path(directory.path().toStdWString()) / WindowPositionFileName;
    auto store = FileWindowPositionStore {};
    auto const missing = store.load(path);
    REQUIRE(missing);
    CHECK_FALSE(missing->has_value());
    auto const expected = WindowPosition { .position = { -1750, 120 }, .monitor = 1 };
    REQUIRE(store.save(path, expected));
    auto const loaded = store.load(path);
    REQUIRE(loaded);
    REQUIRE(loaded->has_value());
    CHECK((*loaded)->position == expected.position);
    CHECK((*loaded)->monitor == expected.monitor);
    REQUIRE(store.save(path, WindowPosition { .position = { 10, 20 }, .monitor = expected.monitor }));
    auto const replaced = store.load(path);
    REQUIRE(replaced);
    REQUIRE(replaced->has_value());
    CHECK((*replaced)->position == QPoint(10, 20));
    CHECK_FALSE(store.save(std::filesystem::path(directory.path().toStdWString()), expected));

    for (auto const* yaml: { "{}",
                             "{",
                             "x: 1.5\ny: 0\nmonitor: 0\n",
                             "x: 1e30\ny: 0\nmonitor: 0\n",
                             "x: 0\ny: 0\nmonitor: -1\n",
                             "x: 0\ny: 0\nmonitor: name\n" })
    {
        auto file = QFile(directory.filePath(WindowPositionFileName));
        REQUIRE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        REQUIRE(file.write(yaml) > 0);
        file.close();
        CHECK_FALSE(store.load(path));
    }
}
