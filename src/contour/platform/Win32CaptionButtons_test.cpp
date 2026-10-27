// SPDX-License-Identifier: Apache-2.0
//
// The Win32 caption adapter on a real HWND: WM_NCHITTEST answers HTMAXBUTTON over the maximise button (Snap
// Layouts), and the non-client mouse messages drive that button. Native Windows platform only; the decisions
// themselves are CaptionHitTest_test.cpp's, on every OS.

#ifdef _WIN32

    #include <contour/config/WindowControlStyle.hpp>
    #include <contour/platform/CaptionHitTest.hpp>
    #include <contour/platform/NativeCaptionButtons.hpp>
    #include <contour/platform/Win32CaptionButtons.hpp>
    #include <contour/platform/Win32WindowFrame.hpp>

    #include <QtCore/QCoreApplication>
    #include <QtGui/QGuiApplication>
    #include <QtGui/QWindow>

    #include <catch2/catch_test_macros.hpp>

    #include <cmath>
    #include <cstddef>
    #include <memory>
    #include <vector>

    #include <Windows.h>

    #include <QtTest/QTest>

using contour::config::WindowControlKind;
using contour::platform::CaptionBinding;
using contour::platform::CaptionButtonVisual;
using contour::platform::CaptionLayout;
using contour::platform::LogicalPoint;
using contour::platform::LogicalRect;

namespace
{

/// The caption buttons' extent: the Fluent row's 46 by 40.
constexpr auto ButtonWidth = 46.0;
constexpr auto ButtonHeight = 40.0;

/// The window's logical size.
constexpr auto WindowWidth = 1006;
constexpr auto WindowHeight = 600;

/// How long an HWND state change may take to arrive, in milliseconds.
constexpr auto WindowStateTimeoutMs = 2000;

/// @return Whether the native Windows platform plugin is running.
[[nodiscard]] bool onNativeWindows()
{
    return QGuiApplication::platformName() == QStringLiteral("windows");
}

/// What a recording binding saw.
struct CaptionCalls
{
    std::vector<CaptionButtonVisual> visuals {}; ///< Every visual reported, in order.
    int toggles = 0;                             ///< How often the window was toggled.
};

/// A shown window with the Win32 frame, attached to a Win32 caption adapter with buttons at its trailing edge
/// in the Windows order, minimise, maximise, close.
struct CaptionWindow
{
    CaptionCalls calls {};
    std::unique_ptr<contour::platform::NativeWindowFrame> frame =
        contour::platform::makeWin32WindowFrame([] { return Qt::ColorScheme::Dark; });
    std::unique_ptr<contour::platform::INativeCaptionButtons> adapter =
        contour::platform::makeWin32CaptionButtons();
    QWindow window {};

    CaptionWindow()
    {
        window.resize(WindowWidth, WindowHeight);
        window.create();
        frame->apply(window, contour::platform::WindowDecoration::Client);
        window.show();
        REQUIRE(QTest::qWaitForWindowExposed(&window));
        adapter->attach(window,
                        CaptionBinding {
                            .layout = [this] { return layout(); },
                            .visual = [this](CaptionButtonVisual visual) { calls.visuals.push_back(visual); },
                            .toggleMaximize = [this] { ++calls.toggles; },
                        });
    }

    CaptionWindow(CaptionWindow const&) = delete;
    CaptionWindow& operator=(CaptionWindow const&) = delete;
    CaptionWindow(CaptionWindow&&) = delete;
    CaptionWindow& operator=(CaptionWindow&&) = delete;
    ~CaptionWindow() { adapter->detach(window); }

    /// @return The buttons where WindowControls.qml would report them for the window's current width.
    [[nodiscard]] CaptionLayout layout() const
    {
        auto const width = static_cast<double>(window.width());
        auto result = CaptionLayout { .buttons = {}, .windowWidth = width, .resizeBorder = 6 };
        auto const at = [&](WindowControlKind kind, double trailingIndex) {
            result.buttons[static_cast<size_t>(kind)] =
                LogicalRect { .x = width - (trailingIndex + 1) * ButtonWidth,
                              .y = 0,
                              .width = ButtonWidth,
                              .height = ButtonHeight };
        };
        at(WindowControlKind::Close, 0);
        at(WindowControlKind::Maximize, 1);
        at(WindowControlKind::Minimize, 2);
        return result;
    }

    /// @return @p kind's centre, in logical pixels.
    [[nodiscard]] LogicalPoint centreOf(WindowControlKind kind) const
    {
        auto const& rect = *layout().buttons[static_cast<size_t>(kind)];
        return LogicalPoint { .x = rect.x + (rect.width / 2), .y = rect.y + (rect.height / 2) };
    }

    /// @return The platform window.
    [[nodiscard]] HWND hwnd() const { return reinterpret_cast<HWND>(window.winId()); }

    /// @return @p point on the screen, in physical pixels, packed as a mouse message's lParam.
    [[nodiscard]] LPARAM screenParam(LogicalPoint point) const
    {
        auto const ratio = window.devicePixelRatio();
        auto screen = POINT { .x = static_cast<LONG>(std::lround(point.x * ratio)),
                              .y = static_cast<LONG>(std::lround(point.y * ratio)) };
        REQUIRE(ClientToScreen(hwnd(), &screen));
        return MAKELPARAM(screen.x, screen.y);
    }

    /// @return What WM_NCHITTEST answers at @p point.
    [[nodiscard]] LRESULT hitTest(LogicalPoint point) const
    {
        return SendMessageW(hwnd(), WM_NCHITTEST, 0, screenParam(point));
    }

    /// Sends the non-client mouse @p message over the maximise button.
    void sendOverMaximize(UINT message) const
    {
        SendMessageW(hwnd(),
                     message,
                     static_cast<WPARAM>(HTMAXBUTTON),
                     screenParam(centreOf(WindowControlKind::Maximize)));
    }
};

} // namespace

TEST_CASE("WM_NCHITTEST over the maximise button answers HTMAXBUTTON", "[native][win32][caption][hittest]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};

    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Maximize)) == HTMAXBUTTON);
    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Minimize)) != HTMAXBUTTON);
    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Close)) != HTMAXBUTTON);
    CHECK(fixture.hitTest(LogicalPoint { .x = 400, .y = 300 }) == HTCLIENT);

    // The frame filter still zeroes the frame beside the caption filter: the client area is the whole window.
    SetWindowPos(
        fixture.hwnd(), nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
    auto client = RECT {};
    auto outer = RECT {};
    REQUIRE(GetClientRect(fixture.hwnd(), &client));
    REQUIRE(GetWindowRect(fixture.hwnd(), &outer));
    CHECK(client.right - client.left == outer.right - outer.left);
}

TEST_CASE("the maximised window's edge pixel over the maximise button answers HTMAXBUTTON",
          "[native][win32][caption][hittest]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};
    // The maximise button alone at the trailing edge, so the screen's corner pixel lands on it.
    fixture.adapter->attach(fixture.window,
                            CaptionBinding {
                                .layout =
                                    [&fixture] {
                                        auto layout = CaptionLayout {};
                                        layout.windowWidth = fixture.window.width();
                                        layout.resizeBorder = 6;
                                        layout.buttons[static_cast<size_t>(WindowControlKind::Maximize)] =
                                            LogicalRect { .x = layout.windowWidth - ButtonWidth,
                                                          .y = 0,
                                                          .width = ButtonWidth,
                                                          .height = ButtonHeight };
                                        return layout;
                                    },
                                .visual = [](CaptionButtonVisual) {},
                                .toggleMaximize = [] {},
                            });
    fixture.window.showMaximized();
    REQUIRE(QTest::qWaitFor([&] { return IsZoomed(fixture.hwnd()) != FALSE; }, WindowStateTimeoutMs));
    QCoreApplication::processEvents();

    auto monitor = MONITORINFO {};
    monitor.cbSize = sizeof(monitor);
    REQUIRE(GetMonitorInfoW(MonitorFromWindow(fixture.hwnd(), MONITOR_DEFAULTTONEAREST), &monitor));
    auto const corner = MAKELPARAM(monitor.rcWork.right - 1, monitor.rcWork.top);
    CHECK(SendMessageW(fixture.hwnd(), WM_NCHITTEST, 0, corner) == HTMAXBUTTON);
}

TEST_CASE("the scaled run hits the same button", "[native][win32][caption][hittest]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};
    // contour_windows_native_scaled_test runs this under QT_SCALE_FACTOR=1.5.
    if (qEnvironmentVariable("QT_SCALE_FACTOR") == QStringLiteral("1.5"))
        CHECK(fixture.window.devicePixelRatio() == 1.5);

    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Maximize)) == HTMAXBUTTON);
    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Minimize)) != HTMAXBUTTON);
    CHECK(fixture.hitTest(LogicalPoint { .x = 400, .y = 300 }) == HTCLIENT);
}

TEST_CASE("a non-client click toggles the window once", "[native][win32][caption]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};

    fixture.sendOverMaximize(WM_NCMOUSEMOVE);
    fixture.sendOverMaximize(WM_NCLBUTTONDOWN);
    fixture.sendOverMaximize(WM_NCLBUTTONUP);
    // Queued: the toggle runs after the message, not inside it.
    CHECK(fixture.calls.toggles == 0);
    QCoreApplication::processEvents();

    CHECK(fixture.calls.toggles == 1);
    // The leave the move asked for may follow, as the real pointer is not over the button.
    REQUIRE(fixture.calls.visuals.size() >= 3);
    CHECK(fixture.calls.visuals[0] == CaptionButtonVisual::Hover);
    CHECK(fixture.calls.visuals[1] == CaptionButtonVisual::Pressed);
    CHECK(fixture.calls.visuals[2] == CaptionButtonVisual::Hover);
}

TEST_CASE("press, leave, release does not toggle", "[native][win32][caption]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};

    fixture.sendOverMaximize(WM_NCLBUTTONDOWN);
    SendMessageW(fixture.hwnd(), WM_NCMOUSELEAVE, 0, 0);
    CHECK(fixture.calls.visuals.back() == CaptionButtonVisual::Rest);
    fixture.sendOverMaximize(WM_NCLBUTTONUP);
    QCoreApplication::processEvents();

    CHECK(fixture.calls.toggles == 0);
    CHECK(fixture.calls.visuals
          == std::vector {
              CaptionButtonVisual::Pressed, CaptionButtonVisual::Rest, CaptionButtonVisual::Hover });
}

TEST_CASE("a detach drops a toggle still queued", "[native][win32][caption]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};

    fixture.sendOverMaximize(WM_NCLBUTTONDOWN);
    fixture.sendOverMaximize(WM_NCLBUTTONUP);
    fixture.adapter->detach(fixture.window);
    QCoreApplication::processEvents();

    CHECK(fixture.calls.toggles == 0);
}

TEST_CASE("detach stops answering", "[native][win32][caption]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    auto fixture = CaptionWindow {};
    REQUIRE(fixture.hitTest(fixture.centreOf(WindowControlKind::Maximize)) == HTMAXBUTTON);

    fixture.adapter->detach(fixture.window);
    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Maximize)) == HTCLIENT);
}

TEST_CASE("the factory's adapter, built before the window, answers once it exists",
          "[native][win32][caption]")
{
    if (!onNativeWindows())
        SKIP("Requires the native Windows platform.");
    // As the application builds it: through the shared factory, before any window exists.
    auto const adapter = contour::platform::makeNativeCaptionButtons();
    auto fixture = CaptionWindow {};
    fixture.adapter->detach(fixture.window);
    adapter->attach(fixture.window,
                    CaptionBinding { .layout = [&fixture] { return fixture.layout(); },
                                     .visual = [](CaptionButtonVisual) {},
                                     .toggleMaximize = [] {} });

    CHECK(fixture.hitTest(fixture.centreOf(WindowControlKind::Maximize)) == HTMAXBUTTON);
    adapter->detach(fixture.window);
}

#endif
