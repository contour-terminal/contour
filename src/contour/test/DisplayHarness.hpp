// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/display/TerminalDisplay.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>

#include <core/Environment.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QSizeF>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>

#include <QtTest/QTest>

namespace contour::test
{

/// The variable that opts a run into the display-gated cases.
inline constexpr std::string_view DisplayTestsVariable = "CONTOUR_TEST_DISPLAY";

/// Whether this run renders through a real windowing system (see test_main.cpp).
/// @param env The environment to read.
/// @return True when CONTOUR_TEST_DISPLAY is `1`.
[[nodiscard]] inline bool displayTestsEnabled(core::Environment const& env)
{
    return env.get(DisplayTestsVariable) == "1";
}

/// Whether a DisplayHarness stands its display up inside a shown QQuickWindow, and whether pump() forces
/// frames through that window.
///
/// Windowless is a real production state, not a degenerate one: a closed pane and an invalidated scene
/// graph both leave a live display with no window, hence no render target. Neither it nor a window that is
/// never asked for a frame needs a display server, so cases built on them run under the offscreen platform.
enum class HarnessWindow : uint8_t
{
    None,       ///< No window: the display never has a render target.
    Unrendered, ///< A shown window that pump() never asks for a frame: geometry, input and QML only.
    Shown,      ///< A shown window whose frames pump() forces through the real renderer; needs a display.
};

/// How many times @p needle starts in @p haystack.
/// @param needle   What to count.
/// @param haystack Where to count it.
/// @return The count.
[[nodiscard]] inline std::size_t occurrencesOf(std::string_view needle, std::string_view haystack)
{
    return static_cast<std::size_t>(
        std::ranges::count_if(std::views::iota(std::size_t { 0 }, haystack.size()),
                              [&](std::size_t at) { return haystack.substr(at).starts_with(needle); }));
}

/// One live rendering session: app + session (vtpty::ChannelPty) + display item, in a shown window
/// unless @ref HarnessWindow::None says otherwise.
///
/// Construction wires everything the production QML path would (setSession attaches the display,
/// starts the session's read loop, and creates the renderer on the first sync); pump() forces real
/// frames synchronously via grabWindow(). Destruction replays the production teardown order:
/// terminate the session (closes the PTY, unblocking the reader), destroy the display item while
/// the session still exists (~TerminalDisplay detaches cleanly), then the session, then the window.
struct DisplayHarness
{
    TestApp testApp;
    HarnessWindow windowMode;         ///< How the display is stood up.
    vtpty::ChannelPty* pty = nullptr; ///< Owned by the session's terminal.
    std::unique_ptr<contour::session::TerminalSession> session;
    std::unique_ptr<QQuickWindow> window;                       ///< Null under HarnessWindow::None.
    std::unique_ptr<contour::display::TerminalDisplay> display; ///< Reset in teardown, before the session.
    contour::window::WindowController* controller = nullptr;    ///< Manager-owned; removed in teardown.

    /// @param mode Whether to stand the display up inside a shown window, and whether pump() renders it.
    /// @param configure Adjusts the application's configuration before the session is born from it.
    explicit DisplayHarness(HarnessWindow mode = HarnessWindow::Shown,
                            std::function<void(contour::config::Config&)> const& configure = {}):
        windowMode(mode), display(std::make_unique<contour::display::TerminalDisplay>())
    {
        if (configure)
            configure(testApp.app().config());

        auto ptyOwned = std::make_unique<vtpty::ChannelPty>(
            vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
        pty = ptyOwned.get();
        session = makeTestSession(&testApp.app().sessionsManager(), std::move(ptyOwned), testApp.app());

        if (windowMode != HarnessWindow::None)
        {
            window = std::make_unique<QQuickWindow>();
            window->resize(800, 600);
            // Match production: Main.qml makes the ApplicationWindow transparent so the terminal paints
            // its own (dark) background. A bare QQuickWindow otherwise clears to Qt's default WHITE,
            // which both mismatches the real app and weakens the pixel-change assertions (white margins
            // swamp the grid). Clear to black so grabbed frames reflect what a user actually sees.
            window->setColor(QColor(Qt::black));
            display->setParentItem(window->contentItem());
        }

        display->setSize(QSizeF(800, 600));
        display->setSession(session.get());

        if (window)
            window->show();
        pump();
    }

    /// Binds a real WindowController to this window, so display paths that route through the
    /// controller (window-geometry authority: size hints, show-modes, content-driven resize,
    /// fullscreen) resolve via windowController() exactly as in production. Opt-in because most
    /// display tests don't need it; call it right after construction.
    contour::window::WindowController& bindController()
    {
        controller = testApp.app().sessionsManager().createWindowController();
        controller->bindWindow(window.get());
        // Focus the display in so the manager records it as the controller's active display.
        testApp.app().sessionsManager().focusOnDisplay(display.get());
        return *controller;
    }

    /// Forces one synchronous frame (scene-graph sync + render) and returns the grabbed image.
    ///
    /// Unless the window is HarnessWindow::Shown there is no scene graph to drive, so this only drains the
    /// GUI queue (what the posted display/session lambdas need) and returns a null image; pixel assertions
    /// are all display-gated.
    QImage pump() const
    {
        QCoreApplication::processEvents();
        auto image = windowMode == HarnessWindow::Shown ? window->grabWindow() : QImage {};
        QCoreApplication::processEvents();
        return image;
    }

    /// The device status request (DSR 5) feedAndSettle() puts after the output it feeds.
    static constexpr auto StatusRequest = std::string_view { "\033[5n" };

    /// The terminal's answer to @ref StatusRequest: it is in good order.
    static constexpr auto StatusAnswer = std::string_view { "\033[0n" };

    /// Feeds VT output, as the application's, and pumps a frame once the terminal has parsed all of it.
    ///
    /// The PTY running dry says only that the session's reader has read the output: it parses what it read
    /// once it holds the terminal, which can be after this thread has gone on to act on it. So a status
    /// request follows the output, and this waits for its answer. The terminal answers in order, and writes
    /// an answer only once the parse that raised it is over, so the answer in the PTY means everything before
    /// it was parsed. The answers are application input like any other: sentInput() includes them.
    /// @param vt Output that leaves the parser in its ground state, as complete sequences do: a sequence left
    ///           open would take the request in, and the wait would fail.
    void feedAndSettle(std::string_view vt) const
    {
        // Neither the answers given already nor those the output asks for itself are the one awaited here.
        auto const answered = occurrencesOf(StatusAnswer, sentInput()) + occurrencesOf(StatusRequest, vt);
        pty->feed(std::string { vt } + std::string { StatusRequest });
        REQUIRE(
            QTest::qWaitFor([&] { return occurrencesOf(StatusAnswer, pty->stdinSnapshot()) > answered; }));
        pump();
    }

    /// Everything the application has been sent, once the terminal has nothing left queued for it.
    ///
    /// What the GUI thread raises -- a key, a mouse report, a resize's report -- only queues its bytes: the
    /// session's PTY reader writes them, woken for it (vtbackend::InputWriter::ReaderThread), so they reach
    /// the PTY after whatever raised them has returned. The queue is emptied only once the write is made, so
    /// a drained queue means everything raised so far is in the PTY.
    /// @return The PTY's input so far.
    [[nodiscard]] std::string sentInput() const
    {
        auto const& terminal = session->terminal();
        REQUIRE(QTest::qWaitFor([&] { return !terminal.hasInput(); }));
        return pty->stdinSnapshot();
    }

    ~DisplayHarness()
    {
        session->terminate(); // closes the PTY; the blocked reader wakes and winds down
        QCoreApplication::processEvents();
        // Drop the controller (if bound) before its window: it holds an eventFilter/connection on
        // the QQuickWindow. removeWindowController deleteLater()s it; drain that below.
        if (controller != nullptr)
        {
            testApp.app().sessionsManager().removeWindowController(controller->windowId());
            controller = nullptr;
        }
        display.reset(); // while the session still lives: ~TerminalDisplay detaches from it
        QCoreApplication::processEvents();
        session.reset(); // joins the session threads (PTY already closed)
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
};

} // namespace contour::test
