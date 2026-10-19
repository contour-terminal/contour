// SPDX-License-Identifier: Apache-2.0
#include <contour/platform/WindowResizeGrid.hpp>
#include <contour/platform/XcbProperty.hpp>

#include <QtGui/QGuiApplication>
#include <QtGui/QWindow>

#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
    #include <windows.h>

using namespace contour;

TEST_CASE("WindowResizeGrid.receivesSynchronousNativeResize", "[row-resize][native]")
{
    if (QGuiApplication::platformName() != "windows")
        SKIP("Requires the native Windows platform.");
    QWindow window;
    window.resize(800, 600);
    SECTION("existing native window")
    {
        window.create();
    }
    SECTION("native window created later")
    {
        REQUIRE(window.handle() == nullptr);
    }
    auto const hadHandle = window.handle() != nullptr;
    auto subscription = platform::makeWindowResizeGrid(window, [] {
        return std::optional { geometry::RowResizeGrid {
            .cellHeight = 21, .baseHeight = 38, .minimumHeight = 100 } };
    });
    CHECK((window.handle() != nullptr) == hadHandle);
    window.create();
    auto hwnd = reinterpret_cast<HWND>(window.winId());
    RECT windowRect {};
    RECT clientRect {};
    REQUIRE(GetWindowRect(hwnd, &windowRect));
    REQUIRE(GetClientRect(hwnd, &clientRect));
    auto const frame = windowRect.bottom - windowRect.top - (clientRect.bottom - clientRect.top);
    RECT proposed { 10, 20, 810, 20 + 673 + frame };
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOM, reinterpret_cast<LPARAM>(&proposed));
    CHECK(proposed.bottom - proposed.top - frame == 668);

    window.destroy();
    window.create();
    hwnd = reinterpret_cast<HWND>(window.winId());
    proposed.bottom = proposed.top + 673 + frame;
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOM, reinterpret_cast<LPARAM>(&proposed));
    CHECK(proposed.bottom - proposed.top - frame == 668);

    subscription.reset();
    proposed.bottom = proposed.top + 673 + frame;
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOM, reinterpret_cast<LPARAM>(&proposed));
    CHECK(proposed.bottom - proposed.top - frame == 673);
}

TEST_CASE("WindowResizeGrid.snapsOnlyVerticalInteractiveResizes", "[row-resize][native]")
{
    if (QGuiApplication::platformName() != "windows")
        SKIP("Requires the native Windows platform; CONTOUR_TEST_DISPLAY=1 QT_QPA_PLATFORM=windows.");

    QWindow window;
    SECTION("native frame")
    {
        window.setFlags(Qt::Window);
    }
    SECTION("client decoration")
    {
        window.setFlags(Qt::Window | Qt::FramelessWindowHint);
    }
    window.resize(800, 600);
    window.create(); // A real HWND, without showing a window.
    auto grid = std::optional { geometry::RowResizeGrid {
        .cellHeight = 21, .baseHeight = 38, .minimumHeight = 100 } };
    auto subscription = platform::makeWindowResizeGrid(window, [&] { return grid; });
    REQUIRE(subscription != nullptr);
    auto const hwnd = reinterpret_cast<HWND>(window.winId());
    RECT windowRect {};
    RECT clientRect {};
    REQUIRE(GetWindowRect(hwnd, &windowRect));
    REQUIRE(GetClientRect(hwnd, &clientRect));
    auto const frame = windowRect.bottom - windowRect.top - (clientRect.bottom - clientRect.top);

    for (auto const edge: { WMSZ_TOP, WMSZ_BOTTOM, WMSZ_TOPLEFT, WMSZ_BOTTOMRIGHT })
    {
        RECT rect { 10, 20, 810, 20 + 673 + frame };
        auto const before = rect;
        CHECK(SendMessageW(hwnd, WM_SIZING, edge, reinterpret_cast<LPARAM>(&rect)) == TRUE);
        CHECK(rect.bottom - rect.top - frame == 668);
        CHECK(rect.left == before.left);
        CHECK(rect.right == before.right);
        if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT)
            CHECK(rect.bottom == before.bottom);
        else
            CHECK(rect.top == before.top);
    }

    RECT rect { 10, 20, 810, 20 + 673 + frame };
    auto const before = rect;
    grid.reset(); // Configuration disabled: leave the proposal alone.
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOM, reinterpret_cast<LPARAM>(&rect));
    CHECK(rect.bottom == before.bottom);
    grid = geometry::RowResizeGrid { .cellHeight = 21, .baseHeight = 38, .minimumHeight = 100 };
    SendMessageW(hwnd, WM_SIZING, WMSZ_LEFT, reinterpret_cast<LPARAM>(&rect));
    CHECK(rect.bottom == before.bottom);
    window.setWindowState(Qt::WindowMaximized);
    SendMessageW(hwnd, WM_SIZING, WMSZ_BOTTOM, reinterpret_cast<LPARAM>(&rect));
    CHECK(rect.bottom == before.bottom);
}
#endif

#ifdef CONTOUR_FRONTEND_XCB
TEST_CASE("WindowResizeGrid.preservesOtherX11Hints", "[row-resize][native]")
{
    using namespace contour;
    if (QGuiApplication::platformName() != "xcb")
        SKIP("Requires the X11 platform; CONTOUR_TEST_DISPLAY=1 QT_QPA_PLATFORM=xcb.");

    QWindow window;
    window.setMinimumSize(QSize(100, 80));
    window.setMaximumSize(QSize(1600, 1200));
    window.setBaseSize(QSize(7, 17));
    window.setSizeIncrement(QSize(9, 19));
    window.create();
    auto const info = platform::queryXcbPropertyInfo(&window, "WM_NORMAL_HINTS");
    REQUIRE(info.has_value());
    auto const readHints = [&] {
        auto const cookie = xcb_get_property(
            info->connection, 0, info->window, info->propertyAtom, XCB_ATOM_WM_SIZE_HINTS, 0, 18);
        return platform::XcbReply<xcb_get_property_reply_t> { xcb_get_property_reply(
            info->connection, cookie, nullptr) };
    };
    auto const before = readHints();
    REQUIRE(before != nullptr);
    REQUIRE(xcb_get_property_value_length(before.get()) == 18 * sizeof(uint32_t));
    platform::applyRowResizeHints(
        window, geometry::RowResizeGrid { .cellHeight = 23, .baseHeight = 47, .minimumHeight = 125 });
    auto const after = readHints();
    REQUIRE(after != nullptr);
    REQUIRE(xcb_get_property_value_length(after.get()) == 18 * sizeof(uint32_t));
    auto const* original = static_cast<uint32_t const*>(xcb_get_property_value(before.get()));
    auto const* updated = static_cast<uint32_t const*>(xcb_get_property_value(after.get()));
    CHECK(updated[10] == 23);
    CHECK(updated[16] == 47);
    for (auto const index: { 0, 5, 6, 7, 8, 9, 15, 17 })
        CHECK(updated[index] == original[index]);

    // Identical setters are cached by Qt and do not undo the native override.
    window.setBaseSize(QSize(7, 17));
    window.setSizeIncrement(QSize(9, 19));
    platform::applyRowResizeHints(window, std::nullopt);
    auto const disabled = readHints();
    REQUIRE(disabled != nullptr);
    auto const* restored = static_cast<uint32_t const*>(xcb_get_property_value(disabled.get()));
    CHECK(restored[10] == original[10]);
    CHECK(restored[16] == original[16]);
}
#endif
