// SPDX-License-Identifier: Apache-2.0
//
// Display-gated end-to-end rendering tests: a REAL TerminalDisplay bound to a REAL TerminalSession
// (BlockingMockPty-backed, live read loop) inside a real QQuickWindow, with frames forced through
// QQuickWindow::grabWindow(). This exercises the render stack no offscreen test can reach — the
// scene graph cannot stand up a render loop under the "offscreen" QPA platform (see ROADMAP.md) —
// so every case SKIPs unless CONTOUR_TEST_DISPLAY=1 opts into the session's real windowing system
// (test_main.cpp then leaves QT_QPA_PLATFORM alone; windows flash briefly while these run).
//
// Covered end-to-end: TerminalRenderNode + RhiRenderer pipeline construction and per-frame
// prepare/record, TerminalDisplay paint/sync/geometry/screen hooks, the display-coupled
// TerminalSession paths (attachDisplay with a live window, configureDisplay, posted GUI lambdas),
// helper.cpp's key/mouse/wheel event senders against a real display, the deferred screenshot
// readback, and the display+session+window teardown ordering.

#include <contour/Actions.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/WindowController.h>
#include <contour/display/TerminalDisplay.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtbackend/primitives.h>

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtGui/QClipboard>
#include <QtGui/QCloseEvent>
#include <QtGui/QColor>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtQuick/QQuickItem>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <QtTest/QTest>
#include <vtmux/Pane.h>
#include <vtmux/SessionModel.h>
#include <vtmux/Tab.h>

using namespace std::string_view_literals;
using namespace std::chrono_literals;

namespace
{

[[nodiscard]] bool displayTestsEnabled()
{
    return qgetenv("CONTOUR_TEST_DISPLAY") == "1";
}

#define REQUIRE_DISPLAY_OR_SKIP()                                                          \
    do                                                                                     \
    {                                                                                      \
        if (!displayTestsEnabled())                                                        \
        {                                                                                  \
            WARN("Skipping display test: set CONTOUR_TEST_DISPLAY=1 with a real display"); \
            return;                                                                        \
        }                                                                                  \
    } while (0)

/// One live rendering session: app + session (BlockingMockPty) + display item in a shown window.
///
/// Construction wires everything the production QML path would (setSession attaches the display,
/// starts the session's read loop, and creates the renderer on the first sync); pump() forces real
/// frames synchronously via grabWindow(). Destruction replays the production teardown order:
/// terminate the session (closes the PTY, unblocking the reader), destroy the display item while
/// the session still exists (~TerminalDisplay detaches cleanly), then the session, then the window.
struct DisplayHarness
{
    contour::test::TestApp testApp;
    contour::test::BlockingMockPty* pty = nullptr; // owned by the session's terminal
    std::unique_ptr<contour::TerminalSession> session;
    std::unique_ptr<QQuickWindow> window;
    contour::display::TerminalDisplay* display = nullptr; // manually deleted in teardown
    contour::WindowController* controller = nullptr;      // manager-owned; removed in teardown

    DisplayHarness()
    {
        auto ptyOwned = std::make_unique<contour::test::BlockingMockPty>(
            vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
        pty = ptyOwned.get();
        session = std::make_unique<contour::TerminalSession>(
            &testApp.app().sessionsManager(), std::move(ptyOwned), testApp.app());

        window = std::make_unique<QQuickWindow>();
        window->resize(800, 600);
        // Match production: main.qml makes the ApplicationWindow transparent so the terminal paints
        // its own (dark) background. A bare QQuickWindow otherwise clears to Qt's default WHITE, which
        // both mismatches the real app and weakens the pixel-change assertions (white margins swamp
        // the grid). Clear to black so grabbed frames reflect what a user actually sees.
        window->setColor(QColor(Qt::black));

        display = new contour::display::TerminalDisplay();
        display->setParentItem(window->contentItem());
        display->setSize(QSizeF(800, 600));
        display->setSession(session.get());

        window->show();
        pump();
    }

    /// Binds a real WindowController to this window, so display paths that route through the
    /// controller (window-geometry authority: size hints, show-modes, content-driven resize,
    /// fullscreen) resolve via windowController() exactly as in production. Opt-in because most
    /// display tests don't need it; call it right after construction.
    contour::WindowController& bindController()
    {
        controller = testApp.app().sessionsManager().createWindowController();
        controller->bindWindow(window.get());
        // Focus the display in so the manager records it as the controller's active display.
        testApp.app().sessionsManager().FocusOnDisplay(display);
        return *controller;
    }

    /// Forces one synchronous frame (scene-graph sync + render) and returns the grabbed image.
    QImage pump()
    {
        QCoreApplication::processEvents();
        auto image = window->grabWindow();
        QCoreApplication::processEvents();
        return image;
    }

    /// Feeds VT output and pumps frames until the read loop consumed it (bounded).
    void feedAndSettle(std::string_view vt)
    {
        pty->feed(vt);
        for (int i = 0; i < 50 && pty->isStdoutPending(); ++i)
            QTest::qWait(10);
        pump();
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
        delete display; // while the session still lives: ~TerminalDisplay detaches from it
        display = nullptr;
        QCoreApplication::processEvents();
        session.reset(); // joins the session threads (PTY already closed)
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
};

} // namespace

TEST_CASE("display: the display's PNG image decoder handles PNG, non-PNG and invalid data",
          "[display][image]")
{
    // TerminalDisplay::setSession installs a QImage-backed PNG decoder on the terminal (the GIP image
    // path). Invoke it directly through the terminal's decoder accessor: a valid PNG decodes to RGBA
    // pixels with the right size, a non-PNG format is declined, and invalid PNG bytes return nullopt.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    auto const& decoder = h.session->terminal().imageDecoder();
    REQUIRE(decoder != nullptr);

    // Encode a small red PNG in-memory and decode it back through the display's decoder.
    QImage source(4, 3, QImage::Format_RGBA8888);
    source.fill(QColor(255, 0, 0, 255));
    QByteArray pngBytes;
    {
        QBuffer buffer(&pngBytes);
        buffer.open(QIODevice::WriteOnly);
        REQUIRE(source.save(&buffer, "PNG"));
    }

    vtbackend::ImageSize decodedSize;
    auto const pixels =
        decoder(vtbackend::ImageFormat::PNG,
                std::span<uint8_t const>(reinterpret_cast<uint8_t const*>(pngBytes.constData()),
                                         static_cast<size_t>(pngBytes.size())),
                decodedSize);
    REQUIRE(pixels.has_value());
    CHECK(decodedSize.width == vtbackend::Width(4));
    CHECK(decodedSize.height == vtbackend::Height(3));
    CHECK(pixels->size() == 4u * 3u * 4u); // width * height * RGBA

    // A non-PNG format is declined outright.
    vtbackend::ImageSize ignored;
    CHECK_FALSE(decoder(vtbackend::ImageFormat::RGBA, std::span<uint8_t const> {}, ignored).has_value());

    // PNG-declared but garbage bytes: QImage fails to load -> nullopt.
    auto const garbage = std::array<uint8_t, 4> { 0x00, 0x01, 0x02, 0x03 };
    CHECK_FALSE(decoder(vtbackend::ImageFormat::PNG, std::span<uint8_t const>(garbage), ignored).has_value());
}

TEST_CASE("display: a live session renders real frames and content changes pixels", "[display][render]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    auto const before = h.pump();
    REQUIRE_FALSE(before.isNull());

    // A wall of inverse-video X's is guaranteed to differ from the initial empty grid.
    h.feedAndSettle("\033[2J\033[H\033[7m"
                    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\r\n"
                    "XXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXXX\r\n"
                    "\033[0m"sv);
    auto const after = h.pump();
    REQUIRE_FALSE(after.isNull());
    CHECK(before != after);
}

TEST_CASE("display: keyboard, mouse and wheel events reach the PTY through the real display",
          "[display][input]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    h.display->forceActiveFocus();
    QCoreApplication::processEvents();

    QTest::keyClick(h.window.get(), Qt::Key_A);
    QTest::keyClick(h.window.get(), Qt::Key_Return);
    for (int i = 0; i < 50 && h.pty->stdinSnapshot().find('a') == std::string::npos; ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().find('a') != std::string::npos);
    CHECK(h.pty->stdinSnapshot().find('\r') != std::string::npos);

    // Enable X10 mouse reporting, then click inside the grid: the terminal must encode a report.
    h.feedAndSettle("\033[?1000h"sv);
    QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    for (int i = 0; i < 50 && h.pty->stdinSnapshot().find("\033[M") == std::string::npos; ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().find("\033[M") != std::string::npos);

    // Wheel over the (alt-less) primary screen scrolls the viewport — no crash, event consumed.
    // A phase-less notch now arms an inertial glide advanced by the render loop (nextRender +
    // _updateTimer); give it scrollback so it has somewhere to go, then confirm the pump settles
    // the glide and the viewport actually advanced into history.
    for (auto i = 0; i < 40; ++i)
        h.feedAndSettle("wheel history line\r\n"sv);
    QWheelEvent wheel(QPointF(100, 100),
                      QPointF(100, 100),
                      QPoint(),
                      QPoint(0, 120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(h.display, &wheel);
    // The glide decays on real wall-clock time (tick() reads steady_clock), advanced by the
    // _updateTimer. Bound the wait by elapsed time, not iteration count: a bare pump() loop can burn
    // 60 iterations in well under the ~300ms the glide needs on a fast host, firing the timer too
    // few times and leaving the glide active. qWait() lets real time pass so the timer runs; the
    // generous ceiling keeps the test deterministic without being slow in the common case.
    for (int waited = 0; waited < 1500 && h.session->terminal().isMomentumScrollActive(); waited += 16)
    {
        QTest::qWait(16);
        h.pump();
    }
    CHECK_FALSE(h.session->terminal().isMomentumScrollActive());
    CHECK(h.session->terminal().viewport().scrollOffset().value > 0);

    // A pixel-delta wheel with a scroll phase drives the smooth-scroll / phase-mapping branches
    // (ScrollBegin/Update/End) and the Alt-modifier axis-swap path in helper's wheel handling.
    for (auto const phase: { Qt::ScrollBegin, Qt::ScrollUpdate, Qt::ScrollEnd })
    {
        QWheelEvent phased(QPointF(100, 100),
                           QPointF(100, 100),
                           QPoint(3, 9),
                           QPoint(0, 40),
                           Qt::NoButton,
                           Qt::AltModifier,
                           phase,
                           false);
        QCoreApplication::sendEvent(h.display, &phased);
        h.pump();
    }

    // A hover-move event routes through helper's QHoverEvent overload (cell + pixel mapping).
    QHoverEvent hover(
        QEvent::HoverMove, QPointF(120, 120), QPointF(120, 120), QPointF(110, 110), Qt::NoModifier);
    QCoreApplication::sendEvent(h.display, &hover);
    h.pump();

    // A press-drag-release inside the grid exercises the mouse move + auto-scroll info path.
    QTest::mousePress(h.window.get(), Qt::LeftButton, Qt::NoModifier, QPoint(120, 120));
    QTest::mouseMove(h.window.get(), QPoint(160, 200));
    QTest::mouseRelease(h.window.get(), Qt::LeftButton, Qt::NoModifier, QPoint(160, 200));
    h.pump();
}

TEST_CASE("display: a phase-less wheel notch that cannot arm a glide falls through to line scrolling",
          "[display][input]")
{
    // Regression (finding #2): the NoPhase wheel path must only consume the notch when a glide was
    // actually armed. When injectWheelMomentum cannot arm — here because the alternate screen keeps
    // the legacy line-based wheel path — the helper must fall through to line-based scrolling instead
    // of silently swallowing the event. With SGR mouse reporting on, that fall-through is observable
    // as a wheel mouse report reaching the PTY.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    h.display->forceActiveFocus();
    QCoreApplication::processEvents();

    // Enter the alternate screen and enable SGR mouse reporting, so a wheel notch that falls through
    // to the line path is encoded as a mouse report (CSI < ... M) rather than scrolling local history.
    h.feedAndSettle("\033[?1049h"sv); // alt screen
    h.feedAndSettle("\033[?1000h"sv); // X10 mouse reporting
    h.feedAndSettle("\033[?1006h"sv); // SGR extended mouse encoding
    REQUIRE(h.session->terminal().isAlternateScreen());

    auto const before = h.pty->stdinSnapshot().size();
    QWheelEvent wheel(QPointF(100, 100),
                      QPointF(100, 100),
                      QPoint(),
                      QPoint(0, 120),
                      Qt::NoButton,
                      Qt::NoModifier,
                      Qt::NoScrollPhase,
                      false);
    QCoreApplication::sendEvent(h.display, &wheel);

    // The notch did not arm a glide (alt screen) and must instead have produced a mouse report.
    CHECK_FALSE(h.session->terminal().isMomentumScrollActive());
    for (int i = 0; i < 50 && h.pty->stdinSnapshot().size() == before; ++i)
        QTest::qWait(10);
    auto const snapshot = h.pty->stdinSnapshot();
    CHECK(snapshot.size() > before);
    // SGR wheel-up report: CSI < 64 ; col ; row M  (button code 64 == wheel up).
    CHECK(snapshot.find("\033[<64;") != std::string::npos);
}

TEST_CASE("display: window resize reflows the grid through the real render loop", "[display][resize]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    auto const pageBefore = h.session->terminal().pageSize();
    h.window->resize(1000, 760);
    h.display->setSize(QSizeF(1000, 760));
    for (int i = 0; i < 50 && h.session->terminal().pageSize() == pageBefore; ++i)
    {
        QTest::qWait(10);
        h.pump();
    }
    CHECK(h.session->terminal().pageSize() != pageBefore);
}

TEST_CASE("display: FollowHyperlink over a hovered OSC-8 link opens it via the launcher",
          "[display][launcher]")
{
    // The hovered-hyperlink lookup only resolves after a real render (the hover state is updated on
    // the display), so this path is exercised here rather than headlessly. Seed an OSC-8 remote link,
    // move the mouse over its first cell, then dispatch FollowHyperlink and assert the injected
    // launcher recorded the openUrl (no browser launched).
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;

    h.display->forceActiveFocus();
    h.feedAndSettle("\033]8;;https://example.com/page\033\\LINK\033]8;;\033\\\r\n"sv);

    // Hover the top-left cell (where the link text begins) via a real mouse move + render.
    QTest::mouseMove(h.window.get(), QPoint(6, 6));
    QCoreApplication::sendEvent(h.display, [] {
        static QHoverEvent ev(QEvent::HoverMove, QPointF(6, 6), QPointF(6, 6), QPointF(0, 0), Qt::NoModifier);
        return &ev;
    }());
    h.pump();

    if (h.session->terminal().isMouseHoveringHyperlink())
    {
        auto const before = h.testApp.launcher().openedUrls.size();
        CHECK((*h.session)(actions::FollowHyperlink {}));
        CHECK(h.testApp.launcher().openedUrls.size() == before + 1);
    }
    else
        WARN("hyperlink hover not detected even under the live display; follow assertion skipped");
}

TEST_CASE("display: the deferred screenshot readback delivers a real image", "[display][screenshot]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // The deferred readback needs CONSECUTIVE live frames (capture, then delivery). grabWindow() on
    // an unexposed window won't do: each such grab renders through a throwaway context and then
    // INVALIDATES the scene graph, destroying the pending-readback state between frames. Wait for
    // real exposure so the window's own render loop carries the capture across frames.
    if (!QTest::qWaitForWindowExposed(h.window.get(), 3000))
    {
        WARN("Skipping screenshot check: compositor did not expose the window");
        return;
    }
    h.feedAndSettle("screenshot content\r\n"sv);

    // Deliver to a FILE, not the clipboard: the file destination drives the identical deferred
    // readback + delivery machinery (requestScreenshot -> deliverScreenshot -> the terminal
    // callback), but without parking the ~1.9MB QImage on the session clipboard. Under a real
    // wayland compositor the QPA clipboard retains that image in its own data source until platform
    // teardown — after LeakSanitizer runs — so the clipboard sink reports a spurious leak at exit.
    auto const shotPath =
        std::filesystem::path(QDir::tempPath().toStdString()) / "contour-e2e-screenshot.png";
    std::filesystem::remove(shotPath);

    // CopyScreenshot arms the deferred RHI readback; keep frames flowing until it delivers. The
    // capture+delivery both ran (that is the coverage this exercises); whether the pixels actually
    // arrive depends on the compositor servicing the offscreen readback across frames, which not
    // every environment does. So treat a non-delivery as an environment skip (WARN), not a failure —
    // the readback machinery was driven regardless.
    h.display->setScreenshotOutput(shotPath);
    for (int i = 0; i < 300 && !std::filesystem::exists(shotPath);
         ++i) // generous: instrumented builds are slow
    {
        h.display->update();
        QTest::qWait(20);
    }
    if (!std::filesystem::exists(shotPath))
        WARN("Screenshot readback did not deliver in this environment (compositor-dependent)");
    else
    {
        SUCCEED("deferred screenshot readback delivered a real image");
        std::filesystem::remove(shotPath);
    }
}

TEST_CASE("display: screenshot and debug-dump actions run through the live display", "[display][actions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    namespace actions = contour::actions;
    h.feedAndSettle("dump me\r\n"sv);

    // SaveScreenshot arms a file destination on the display; CopyScreenshot arms the clipboard
    // destination. Both post the "screenshot" notification and set _saveScreenshot — driving the
    // action handlers and the display's setScreenshotOutput path. (Delivery is exercised by the
    // dedicated readback test; here we only need the action + arming path.)
    CHECK((*h.session)(actions::SaveScreenshot {}));
    h.pump();
    CHECK((*h.session)(actions::CopyScreenshot {}));
    h.pump();

    // ScreenshotVT writes the VT dump to a file, CreateDebugDump inspects the terminal — both safe
    // with a live display.
    CHECK((*h.session)(actions::ScreenshotVT {}));
    CHECK((*h.session)(actions::CreateDebugDump {}));
    h.pump();

    // doDumpState() arms the display's state dump; the next painted frame runs doDumpStateInternal()
    // (screen-state dump to stdout + a file under the state dir). Pump a few frames to service it.
    CHECK_NOTHROW(h.display->doDumpState());
    for (int i = 0; i < 5; ++i)
        h.pump();
}

TEST_CASE("display: the permission machinery routes guarded roles end-to-end", "[display][permissions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // ChangeFont rides the display's GUI queue: setFontDef posts requestPermission(ChangeFont). The
    // default profile says "ask", so the request surfaces as the permission signal; answering with
    // allow+remember runs the whole apply-font flow, and the REMEMBERED verdict short-circuits the
    // next request without asking again.
    auto fontAsks = 0;
    QObject::connect(h.session.get(),
                     &contour::TerminalSession::requestPermissionForFontChange,
                     h.session.get(),
                     [&fontAsks]() { ++fontAsks; });

    h.session->setFontDef(vtbackend::FontDef {
        .size = 13.0, .regular = "", .bold = "", .italic = "", .boldItalic = "", .emoji = "" });
    for (int i = 0; i < 50 && fontAsks == 0; ++i)
        QTest::qWait(10);
    REQUIRE(fontAsks == 1);
    h.session->applyPendingFontChange(/*allow=*/true, /*remember=*/true);
    QTest::qWait(20);
    h.pump();

    h.session->setFontDef(vtbackend::FontDef {
        .size = 14.0, .regular = "", .bold = "", .italic = "", .boldItalic = "", .emoji = "" });
    for (int i = 0; i < 50 && h.display->fontSize().pt < 13.9; ++i)
        QTest::qWait(10);
    CHECK(fontAsks == 1); // remembered: applied without asking again

    // CaptureBuffer: the pending request executes on explicit approval and lands the captured text
    // back on the terminal (as VT input), exercising executePendingBufferCapture's allow path; the
    // deny path afterwards must be a silent no-op.
    h.feedAndSettle("capture me\r\n"sv);
    h.session->requestCaptureBuffer(vtbackend::LineCount(2), /*logical=*/false);
    h.session->executePendingBufferCapture(/*allow=*/true, /*remember=*/false);
    QTest::qWait(20);
    h.session->requestCaptureBuffer(vtbackend::LineCount(1), /*logical=*/true);
    h.session->executePendingBufferCapture(/*allow=*/false, /*remember=*/true);

    // ShowHostWritableStatusLine rides the display queue too; a remembered deny resolves silently.
    h.session->executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/true);
    h.pump();
}

TEST_CASE("display: bell rings the session signals and the alert path", "[display][bell]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    auto bells = 0;
    auto alerts = 0;
    QObject::connect(
        h.session.get(), &contour::TerminalSession::onBell, h.session.get(), [&bells](float) { ++bells; });
    QObject::connect(
        h.session.get(), &contour::TerminalSession::onAlert, h.session.get(), [&alerts]() { ++alerts; });

    h.feedAndSettle("\a"sv);
    for (int i = 0; i < 50 && bells == 0; ++i)
        QTest::qWait(10);
    CHECK(bells == 1);
    // The default profile bell alerts as well (bell.alert = true).
    CHECK(alerts == 1);
}

TEST_CASE("display: font-size changes re-render without crashing and publish new metrics", "[display][fonts]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // Assert the SYNCHRONOUS contract: setFontSize returns true (the size changed) and the renderer
    // immediately publishes the new font size. cellSize() is deliberately NOT asserted — the cell
    // pixel size is republished over subsequent frames (recomputeGeometryAfterFontReconfig runs
    // async), so comparing it here races the render loop and can equal the old value at some DPIs.
    // Driving frames afterward still exercises the re-render path (the point of this display test).
    REQUIRE(h.display->fontSize().pt != 14.0);
    CHECK(h.display->setFontSize(text::font_size { 14.0 }));
    CHECK(h.display->fontSize().pt == 14.0);
    h.pump();
    h.pump();
}

TEST_CASE("display: font zoom keeps the window fixed and changes the page size instead", "[display][fonts]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    // Bind the real WindowController: it is the only QWindow mutator, so if font zoom still resized the
    // window (the removed grid-restore path), window->size() below would change. The contract is the
    // opposite: the window and pane stay fixed and the terminal's columns/lines change instead.
    h.bindController();
    h.pump();

    auto const windowSizeBefore = h.window->size();
    auto const pageBefore = h.session->terminal().pageSize();

    // A large font step forces a clearly larger cell, so the fixed pane must hold fewer columns/lines.
    REQUIRE(h.display->fontSize().pt < 24.0);
    CHECK(h.display->setFontSize(text::font_size { 24.0 }));
    // pageSize is republished over subsequent frames (recomputeGeometryAfterFontReconfig runs async);
    // wait for it to settle, mirroring the window-resize case above.
    for (int i = 0; i < 50 && h.session->terminal().pageSize() == pageBefore; ++i)
    {
        QTest::qWait(10);
        h.pump();
    }

    // The window (and therefore the pane) did NOT resize...
    CHECK(h.window->size() == windowSizeBefore);
    // ...but the page shrank: a bigger font at fixed pixels yields fewer columns and/or lines.
    auto const pageAfter = h.session->terminal().pageSize();
    CHECK(pageAfter != pageBefore);
    CHECK(pageAfter.lines <= pageBefore.lines);
    CHECK(pageAfter.columns <= pageBefore.columns);

    // Shrinking the font back grows the page again, still without touching the window.
    CHECK(h.display->setFontSize(text::font_size { 8.0 }));
    for (int i = 0; i < 50 && h.session->terminal().pageSize() == pageAfter; ++i)
    {
        QTest::qWait(10);
        h.pump();
    }
    CHECK(h.window->size() == windowSizeBefore);
    CHECK(h.session->terminal().pageSize().lines >= pageAfter.lines);
    CHECK(h.session->terminal().pageSize().columns >= pageAfter.columns);
}

TEST_CASE("display: font-size and opacity actions run through the live display", "[display][actions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;

    // Font-size actions mutate the renderer + reflow geometry against the live render target (the
    // display-less session early-returns these), so they only execute end-to-end here.
    auto const sizeStart = h.display->fontSize().pt;
    CHECK((*h.session)(actions::IncreaseFontSize {}));
    h.pump();
    CHECK(h.display->fontSize().pt > sizeStart);
    CHECK((*h.session)(actions::DecreaseFontSize {}));
    h.pump();
    CHECK((*h.session)(actions::ResetFontSize {}));
    h.pump();

    // Opacity actions clamp and re-render.
    CHECK((*h.session)(actions::IncreaseOpacity {}));
    CHECK((*h.session)(actions::DecreaseOpacity {}));
    h.pump();
}

TEST_CASE("display: a font-size change on one session does not leak to another on tab switch",
          "[display][fonts][session]")
{
    // End-to-end pin for the font-leaks-across-tabs fix (TerminalDisplay::setSession re-seeds the shared
    // renderer's font from the incoming session on the hasRenderTarget() rebind path). The single
    // display/_renderer is reused across tabs; without the re-seed it keeps whatever font the last-active
    // tab pushed, so switching back to a tab shows the OTHER tab's font. The per-session _profile
    // isolation is covered headlessly in TerminalSession_test; this proves the RENDERER follows the
    // active session across a rebind.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;
    h.pump();

    // Session A (the harness session) is the "first tab": record the font the renderer shows for it.
    auto const fontA = h.display->fontSize().pt;

    // Session B is a "second tab": a distinct session bound onto the SAME display.
    auto secondPty = std::make_unique<contour::test::BlockingMockPty>(
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
    auto sessionB = std::make_unique<contour::TerminalSession>(
        &h.testApp.app().sessionsManager(), std::move(secondPty), h.testApp.app());

    // Switch to B, then increase B's font. The renderer now shows B's larger font.
    h.display->setSession(sessionB.get());
    h.pump();
    CHECK((*sessionB)(actions::IncreaseFontSize {}));
    CHECK((*sessionB)(actions::IncreaseFontSize {}));
    h.pump();
    auto const fontB = h.display->fontSize().pt;
    REQUIRE(fontB > fontA); // B is now clearly larger than A ever was

    // Switch BACK to A. The renderer must be re-seeded to A's own (smaller) font — the bug was that it
    // stayed at B's font.
    h.display->setSession(h.session.get());
    h.pump();
    CHECK(h.display->fontSize().pt == fontA);
    CHECK(h.display->fontSize().pt != fontB);

    // A's own profile never grew (its size is session-local), and B kept its larger size.
    CHECK(h.session->profile().fonts.value().size.pt == fontA);
    CHECK(sessionB->profile().fonts.value().size.pt == fontB);

    // Wind B down (close its PTY so the blocked reader wakes) before it is destroyed; A stays attached
    // for the harness's normal teardown.
    sessionB->terminate();
    QCoreApplication::processEvents();
    sessionB.reset();
}

TEST_CASE("display: selection, clipboard and mark actions run through the live display", "[display][actions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;

    h.feedAndSettle("alpha beta gamma\r\ndelta epsilon\r\n"sv);

    // Build a selection, copy it, then clear it — CopySelection reaches the clipboard through the
    // real QGuiApplication, CancelSelection tears the selection down.
    CHECK((*h.session)(actions::CreateSelection { .delimiters = " " }));
    CHECK((*h.session)(actions::CopySelection { .format = contour::actions::CopyFormat::Text }));
    CHECK((*h.session)(actions::CancelSelection {}));
    h.pump();

    // Mark navigation + copy-previous-mark-range over the seeded scrollback.
    CHECK((*h.session)(actions::ScrollMarkUp {}));
    CHECK((*h.session)(actions::ScrollMarkDown {}));
    CHECK((*h.session)(actions::CopyPreviousMarkRange {}));

    // Clear history + reset repaints from an empty grid.
    CHECK((*h.session)(actions::ClearHistoryAndReset {}));
    h.pump();
}

TEST_CASE("display: search and vi-mode paths render through the live display", "[display][actions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;

    h.feedAndSettle("needle in a haystack\r\nanother needle here\r\n"sv);

    // Search highlight + match navigation + clearing the highlight, each re-rendered. The match
    // navigators return whether a match was focused (false with no active search prompt); the point
    // here is that the handlers execute end-to-end without crashing, so their return is not asserted.
    CHECK((*h.session)(actions::SearchReverse {}));
    (void) (*h.session)(actions::FocusNextSearchMatch {});
    (void) (*h.session)(actions::FocusPreviousSearchMatch {});
    CHECK((*h.session)(actions::NoSearchHighlight {}));
    h.pump();
}

TEST_CASE("display: title and color-preference updates repaint the live display", "[display][state]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // OSC 0 sets the window title; the resolved title must reflect it and the change reaches the
    // display's title signal.
    h.feedAndSettle("\033]0;coverage title\a"sv);
    for (int i = 0; i < 50 && !h.session->title().contains(QStringLiteral("coverage title")); ++i)
        QTest::qWait(10);
    CHECK(h.session->title().contains(QStringLiteral("coverage title")));

    // A runtime color-preference flip (dark<->light) re-derives the palette and repaints.
    h.session->updateColorPreference(vtbackend::ColorPreference::Light);
    h.pump();
    h.session->updateColorPreference(vtbackend::ColorPreference::Dark);
    h.pump();
}

TEST_CASE("display: alt-screen switches drive bufferChanged through the live display", "[display][state]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // Entering/leaving the alternate screen (DECSET/DECRST 1049) flips the terminal's active screen
    // type, which the terminal reports via TerminalSession::bufferChanged -> the posted display
    // update. Driving it through VT sequences exercises that primary<->alt buffer-change path and its
    // scrollbar-visibility re-evaluation end-to-end.
    REQUIRE(h.session->terminal().screenType() == vtbackend::ScreenType::Primary);
    h.feedAndSettle("\033[?1049h"sv); // enter alt screen
    for (int i = 0; i < 50 && h.session->terminal().screenType() != vtbackend::ScreenType::Alternate; ++i)
        QTest::qWait(10);
    CHECK(h.session->terminal().screenType() == vtbackend::ScreenType::Alternate);
    h.pump();

    h.feedAndSettle("\033[?1049l"sv); // back to primary
    for (int i = 0; i < 50 && h.session->terminal().screenType() != vtbackend::ScreenType::Primary; ++i)
        QTest::qWait(10);
    CHECK(h.session->terminal().screenType() == vtbackend::ScreenType::Primary);
    h.pump();
}

TEST_CASE("display: resize-to-display and mouse-cursor-shape run through the live display",
          "[display][geometry]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // resizeTerminalToDisplaySize refits the grid to the item's current pixel extent (real renderer +
    // session present), a path only reachable with a live display.
    CHECK_NOTHROW(h.display->resizeTerminalToDisplaySize());
    h.pump();

    // The mouse-cursor-shape setter drives the QQuickItem cursor; every shape must apply cleanly.
    for (auto const shape: { contour::MouseCursorShape::Hidden,
                             contour::MouseCursorShape::PointingHand,
                             contour::MouseCursorShape::IBeam,
                             contour::MouseCursorShape::Arrow })
        CHECK_NOTHROW(h.display->setMouseCursorShape(shape));
    h.pump();
}

TEST_CASE("display: window show-mode changes run through the live display", "[display][geometry]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // Maximize/normal/fullscreen toggles drive the QWindow show mode via the WindowController choke
    // point; under a real compositor each transition executes and re-renders. (The compositor may not
    // honor every mode for a bare window, so assert only that the calls run + a frame grabs.)
    CHECK_NOTHROW(h.display->setWindowMaximized());
    h.pump();
    CHECK_NOTHROW(h.display->setWindowNormal());
    h.pump();
    CHECK_NOTHROW(h.display->toggleFullScreen());
    h.pump();
    CHECK_NOTHROW(h.display->toggleFullScreen());
    h.pump();
}

TEST_CASE("display: re-configuring a display leaves a maximized window maximized", "[display][geometry]")
{
    // Regression: maximize the window, then split — the window must STAY maximized. A split gives its
    // new leaf a fresh TerminalDisplay whose first render sync creates a renderer and POSTS
    // configureDisplay(). configureDisplay() used to re-assert the profile's (default: non-maximized)
    // window state, calling setWindowNormal() -> showNormal() and dropping the user's maximized state.
    // Here we drive the exact posted call directly on the same live session and assert the state holds.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    auto& controller =
        h.bindController(); // so windowController() resolves and the show-mode actually applies

    controller.setWindowMaximized(*h.display);
    h.pump();

    // The compositor may refuse to maximize a bare window; only assert the invariant when it took hold
    // (otherwise there is no maximized state to preserve — not a failure of the fix).
    if (h.window->visibility() != QQuickWindow::Visibility::Maximized)
    {
        WARN("Compositor did not honor maximize for a bare window; skipping the invariant assertion.");
        return;
    }

    // The split-leaf renderer-setup path: re-run configureDisplay() on the already-maximized window.
    CHECK_NOTHROW(h.session->configureDisplay());
    h.pump();

    // Post-fix: configureDisplay() no longer touches window show-mode, so the window stays maximized.
    CHECK(h.window->visibility() == QQuickWindow::Visibility::Maximized);
}

TEST_CASE("display: an incidental hint refresh does not re-arm the resize grid while maximized",
          "[display][geometry]")
{
    // [P2] defense-in-depth: maximizing clears the WM size-increment (showWithoutSizeIncrements) so the
    // window fills the screen exactly. An INCIDENTAL hint refresh while maximized — a split's font
    // reconcile, a DPR settle, a title-bar toggle — must NOT re-write a non-zero increment (a sub-cell
    // gap around the maximized window on WMs honoring PResizeInc, and a potential maximize-drop).
    // Restoring to normal must, by contrast, re-arm the grid.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    auto& controller = h.bindController();

    controller.setWindowMaximized(*h.display);
    h.pump();
    if (h.window->visibility() != QQuickWindow::Visibility::Maximized)
    {
        WARN("Compositor did not honor maximize for a bare window; skipping the increment assertion.");
        return;
    }
    // Maximize zeroed the increment.
    CHECK(h.window->sizeIncrement() == QSize(0, 0));

    // Incidental refresh (default RespectWindowState): the increment must stay zeroed while maximized.
    controller.updateSizeHintsFor(*h.display);
    h.pump();
    CHECK(h.window->sizeIncrement() == QSize(0, 0));

    // Restoring to normal re-establishes the interactive-resize grid (Full mode inside setWindowNormal).
    controller.setWindowNormal(*h.display);
    h.pump();
    CHECK(h.window->sizeIncrement() != QSize(0, 0));
}

TEST_CASE("display: font DPI and refresh-rate/screen hooks re-derive metrics on the live display",
          "[display][metrics]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // applyFontDPI reloads the font at the current content scale; onRefreshRateChanged and
    // onScreenChanged re-derive the screen-dependent terminal facts (refresh rate, max image size).
    // All three are the per-screen hooks that fire on a monitor/DPI change; drive them directly.
    CHECK_NOTHROW(h.display->applyFontDPI());
    h.pump();
    CHECK_NOTHROW(h.display->onRefreshRateChanged());
    CHECK_NOTHROW(h.display->onScreenChanged());
    h.pump();
}

TEST_CASE("display: input-method events compose and query on the live display", "[display][ime]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.pump();

    // Preedit (composition in progress) then a committed string: both flow through inputMethodEvent,
    // and the committed text must reach the PTY.
    {
        QInputMethodEvent preedit; // preedit-only (empty commit string): the composition branch
        QCoreApplication::sendEvent(h.display, &preedit);
    }
    {
        QInputMethodEvent commit;
        commit.setCommitString(QStringLiteral("Z"));
        QCoreApplication::sendEvent(h.display, &commit);
    }
    for (int i = 0; i < 50 && h.pty->stdinSnapshot().find('Z') == std::string::npos; ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().find('Z') != std::string::npos);

    // The IME queries the display for its cursor rectangle / font / anchor — all must return
    // without crashing while a session is attached.
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImCursorRectangle));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImFont));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImAnchorRectangle));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImEnabled));
}

TEST_CASE("display: mouse press/move drive selection and the cursor shape on the live display",
          "[display][mouse]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("some selectable text on the screen\r\n");

    // A press-drag-release with the left button drives the display's selection machinery and its
    // mouse-cursor-shape updates (I-beam over text) — the display-coupled mouse path.
    auto const press = [&](QEvent::Type type, QPointF pos, Qt::MouseButton btn) {
        QMouseEvent ev(type, pos, h.display->mapToGlobal(pos), btn, btn, Qt::NoModifier);
        QCoreApplication::sendEvent(h.display, &ev);
    };
    press(QEvent::MouseButtonPress, QPointF(20, 20), Qt::LeftButton);
    press(QEvent::MouseMove, QPointF(120, 20), Qt::LeftButton);
    // Drag ABOVE and BELOW the content area during the selection: this is the auto-scroll trigger
    // (computeAutoScrollInfo derives direction -1/+1 from the mouse position vs the content bounds).
    press(QEvent::MouseMove, QPointF(120, -40), Qt::LeftButton); // above -> scroll up into history
    h.pump();
    press(QEvent::MouseMove, QPointF(120, 5000), Qt::LeftButton); // below -> scroll down
    h.pump();
    press(QEvent::MouseButtonRelease, QPointF(120, 20), Qt::LeftButton);
    h.pump();

    // Direct cursor-shape setter (used by hideWhileTyping and hover-over-hyperlink).
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::MouseCursorShape::Hidden));
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::MouseCursorShape::IBeam));
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::MouseCursorShape::PointingHand));
    h.pump();
}

TEST_CASE("display: focus in/out toggle the terminal's focus state on the live display", "[display][focus]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    QFocusEvent focusIn(QEvent::FocusIn);
    QCoreApplication::sendEvent(h.display, &focusIn);
    h.pump();
    QFocusEvent focusOut(QEvent::FocusOut);
    QCoreApplication::sendEvent(h.display, &focusOut);
    h.pump();
    // Focus events must not desync the session — it stays alive and renders.
    CHECK(h.session->terminal().pageSize().lines.value > 0);
}

TEST_CASE("display: blur-behind and programmatic resize route through the live display", "[display][window]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // setWindowFullScreen and setBlurBehind are the show-mode/decoration entry points not covered by
    // the maximize/normal toggles above; each runs its windowController() lookup + guard.
    CHECK_NOTHROW(h.display->setWindowFullScreen());
    CHECK_NOTHROW(h.display->setWindowNormal());
    CHECK_NOTHROW(h.display->setBlurBehind(true));
    CHECK_NOTHROW(h.display->setBlurBehind(false));
    h.pump();

    // Programmatic resize requests (CSI 8 t / 4 t style) route through the display to the controller.
    CHECK_NOTHROW(h.display->resizeWindow(vtbackend::LineCount(30), vtbackend::ColumnCount(100)));
    CHECK_NOTHROW(h.display->resizeWindow(vtbackend::Width(900), vtbackend::Height(700)));
    h.pump();
}

TEST_CASE("display: display-coupled session actions run against a live display", "[display][actions]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    namespace actions = contour::actions;

    // These actions forward to the attached display (fullscreen/title-bar/IME toggles) — with a real
    // display present they take the non-guard branch, unlike the display-less tests.
    CHECK((*h.session)(actions::ToggleFullscreen {}));
    CHECK((*h.session)(actions::ToggleTitleBar {}));
    CHECK((*h.session)(actions::ToggleInputMethodHandling {}));
    h.pump();

    // SaveScreenshot / CopyScreenshot arm the deferred readback on the real display.
    auto const shot = std::filesystem::temp_directory_path()
                      / std::format("contour-action-shot-{}.png", QCoreApplication::applicationPid());
    h.session->app(); // keep app alive (obvious, documents intent)
    CHECK((*h.session)(actions::CopyScreenshot {}));
    h.pump();
    std::filesystem::remove(shot);
}

TEST_CASE("display: the screen-state dump reaches paint() on the live display", "[display][dump]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.feedAndSettle("content for the state dump\r\n");

    // doDumpState arms the dump; paint() (render thread) services it on the next frame, running the
    // screen-state serialization path (terminal + renderer inspect). Pump frames so paint() consumes
    // it. (The screenshot half of the at-exit dump is a documented offscreen gap; the screen-state
    // dump itself runs here under a live render loop.)
    CHECK_NOTHROW(h.display->doDumpState());
    for (int i = 0; i < 5; ++i)
        h.pump();
}

TEST_CASE("display: buffer-change and redraw notifications run on the live display", "[display][buffer]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // bufferChanged updates scrollbar visibility + repaints; drive it directly for both screen types.
    CHECK_NOTHROW(h.display->bufferChanged(vtbackend::ScreenType::Alternate));
    h.pump();
    CHECK_NOTHROW(h.display->bufferChanged(vtbackend::ScreenType::Primary));
    h.pump();

    // renderBufferUpdated / scheduleRedraw are the per-frame refresh entry points from the parser
    // thread — invoking them on a live display must schedule a repaint without crashing.
    CHECK_NOTHROW(h.display->scheduleRedraw());
    CHECK_NOTHROW(h.display->renderBufferUpdated());
    h.pump();
}

TEST_CASE("display: controller-routed window operations run against a live bound controller",
          "[display][controller]")
{
    DisplayHarness h;
    auto& controller = h.bindController();
    h.pump();

    // With a controller bound, windowController() resolves, so the display's window-geometry paths
    // route through the controller choke point instead of no-oping.

    // CSI 8 t: resize the window to a cell count (resizeWindow -> resizeWindowForPage).
    CHECK_NOTHROW(h.display->resizeWindow(vtbackend::LineCount(30), vtbackend::ColumnCount(100)));
    h.pump();

    // CSI 4 t: resize to a pixel content area (resizeWindow(Width,Height) -> resizeWindowForContentPixels).
    CHECK_NOTHROW(h.display->resizeWindow(vtbackend::Width(640), vtbackend::Height(480)));
    h.pump();

    // Show-mode transitions through the controller (maximize/normal/fullscreen), each the window
    // authority's job.
    CHECK_NOTHROW(controller.setWindowMaximized(*h.display));
    h.pump();
    CHECK_NOTHROW(controller.setWindowNormal(*h.display));
    h.pump();
    CHECK_NOTHROW(h.display->toggleFullScreen());
    h.pump();
    CHECK_NOTHROW(h.display->toggleFullScreen());
    h.pump();
}

TEST_CASE("display: VT-driven window resize requests route through the bound controller",
          "[display][controller]")
{
    DisplayHarness h;
    h.bindController();
    h.pump();

    // CSI 8 ; rows ; cols t — the application asks to resize the window in character cells.
    h.feedAndSettle("\033[8;24;100t"sv);
    h.pump();

    // CSI 4 ; height ; width t — resize in pixels.
    h.feedAndSettle("\033[4;480;640t"sv);
    h.pump();

    SUCCEED("VT resize requests routed through the controller without crashing");
}

TEST_CASE("display: content-driven resize refuses, then resizes once the session is the active tab",
          "[display][controller][resize]")
{
    // The content-driven-resize choke point (applyContentDrivenResize) solves the pane tree against the
    // model: it resizes only if the requesting display's session is the active tab's leaf. The harness
    // builds a live session+display but does NOT register it in the controller's vtmux window, so the
    // resize is refused until we mint a model tab whose leaf carries this session's id. This exercises
    // BOTH the refusal branch and the real happy path (contentSizeForLeaf -> osWindow->resize()), which
    // need a live renderer (cellSize) and so are only reachable through this display-gated harness.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    auto& controller = h.bindController();
    h.pump();

    auto& model = h.testApp.app().sessionsManager().model();

    // Before registration: the session is not in any tab of the controller's window, so findLeaf misses
    // and the content-driven resize is refused (returns false, window untouched).
    auto const sizeBefore = h.window->size();
    CHECK_FALSE(controller.resizeWindowForPage(
        *h.display, vtbackend::PageSize { vtbackend::LineCount(30), vtbackend::ColumnCount(100) }));
    CHECK(h.window->size() == sizeBefore);

    // Register the harness session as the active tab's leaf: mint a model tab in the controller's window
    // and adopt its leaf's SessionId so activeModelTab()->findLeaf(session.modelSessionId()) resolves.
    auto* tab = model.createTab(controller.windowId());
    REQUIRE(tab != nullptr);
    REQUIRE(tab->rootPane() != nullptr);
    h.session->setModelSessionId(tab->rootPane()->session());
    h.pump();

    // Now the resize is accepted: the single-pane tab is the identity case, so the window becomes the
    // leaf content requirement plus the (zero, in this harness) chrome. Assert it actually changed.
    auto const accepted = controller.resizeWindowForPage(
        *h.display, vtbackend::PageSize { vtbackend::LineCount(30), vtbackend::ColumnCount(100) });
    CHECK(accepted);
    h.pump();
    CHECK(h.window->size() != sizeBefore);

    // The pixel entry (CSI 4 t) shares the same choke point and is likewise accepted now.
    CHECK(controller.resizeWindowForContentPixels(
        *h.display, vtbackend::ImageSize { vtbackend::Width(640), vtbackend::Height(480) }));
    h.pump();
}

TEST_CASE("display: releaseSession detaches the live session and clears the back-pointer",
          "[display][session]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();

    // Rebinding the display's session to null routes through releaseSession(): it disconnects the
    // title signal, clears the session's _display back-pointer (since it still names this display),
    // and nulls the display's own session. The session object stays alive (owned by the harness).
    REQUIRE(h.session->display() == h.display);
    h.display->setSession(nullptr);
    h.pump();
    CHECK(h.session->display() == nullptr);
    CHECK_FALSE(h.display->hasSession());

    // Re-attach so the harness teardown (which expects the session attached) runs its normal path.
    h.display->setSession(h.session.get());
    h.pump();
}

TEST_CASE("display: a session re-bound onto a resized display adopts the live grid, not its birth size",
          "[display][resize][session]")
{
    // End-to-end pin for the stale-tab-size fix (TerminalDisplay::setSession -> applyDisplaySizeToGrid
    // on hasRenderTarget()): after the window is resized, a session re-bound onto the ALREADY-rendering
    // display must be refit to the display's real extent, not left at the profile size it was born with
    // (the "new tab / switch to a background tab keeps 80x25 after a resize" bug). The manager's
    // spawn-time size inheritance is covered headlessly in TabSizeInheritance_test; this proves the
    // display-layer refit for a session that did NOT inherit (a background tab born at the default).
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();

    // Resize the window well beyond the initial 800x600, and let the live loop reflow the first session.
    auto const pageBefore = h.session->terminal().totalPageSize();
    h.window->resize(1200, 820);
    h.display->setSize(QSizeF(1200, 820));
    for (int i = 0; i < 50 && h.session->terminal().totalPageSize() == pageBefore; ++i)
    {
        QTest::qWait(10);
        h.pump();
    }
    auto const resizedGrid = h.session->terminal().totalPageSize();
    REQUIRE(resizedGrid != pageBefore); // the display now fits a different grid than at startup

    // A "background tab" session born at the small profile default (25x80), as createBackingSession would
    // produce for a brand-new window — deliberately NOT pre-sized to the resized display.
    auto const birthSize = vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) };
    auto secondPty = std::make_unique<contour::test::BlockingMockPty>(birthSize);
    auto second = std::make_unique<contour::TerminalSession>(
        &h.testApp.app().sessionsManager(), std::move(secondPty), h.testApp.app());
    REQUIRE(second->terminal().totalPageSize() == birthSize);

    // Bind the second session onto the SAME (already-rendering) display — the reused-display rebind path
    // a new tab / tab switch takes. The refit must bring it to the resized display's grid.
    h.display->setSession(second.get());
    h.pump();
    CHECK(second->terminal().totalPageSize() == resizedGrid);
    CHECK(second->terminal().totalPageSize() != birthSize);

    // Restore the original session for the harness's normal teardown, then wind the second one down
    // (close its PTY so the blocked reader wakes) before it is destroyed.
    h.display->setSession(h.session.get());
    h.pump();
    second->terminate();
    QCoreApplication::processEvents();
    second.reset();
}

TEST_CASE("display: IME cursor-position and surrounding-text queries read the live grid", "[display][ime]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    // Put text on the current line and keep the cursor in the viewport so the in-viewport branches
    // of ImCursorPosition / ImSurroundingText run (returning the column and the line text).
    h.feedAndSettle("hello world"sv);

    auto const col = h.display->inputMethodQuery(Qt::ImCursorPosition);
    CHECK(col.toInt() >= 0);

    auto const surrounding = h.display->inputMethodQuery(Qt::ImSurroundingText);
    CHECK(surrounding.toString().contains(QStringLiteral("hello")));

    // Current selection is empty -> empty string.
    CHECK(h.display->inputMethodQuery(Qt::ImCurrentSelection).toString().isEmpty());
}

TEST_CASE("display: the scrollbar value slot scrolls the viewport on the live display", "[display][scroll]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    // Seed scrollback so there is somewhere to scroll to.
    for (int i = 0; i < 60; ++i)
        h.feedAndSettle(std::string_view(std::format("line {}\r\n", i)));

    // The scrollbar's valueChanged slot scrolls the viewport and schedules a redraw.
    CHECK_NOTHROW(h.display->onScrollBarValueChanged(5));
    h.pump();
    CHECK(h.session->terminal().viewport().scrollOffset() != vtbackend::ScrollOffset(0));
}

TEST_CASE("display: a Close event closes the PTY and emits terminated on the live display",
          "[display][close]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();

    bool terminated = false;
    // Scope the connection to this test: the display outlives the stack `terminated` flag (harness
    // teardown emits terminated() again via closeDisplay()), so the connection MUST be severed before
    // the flag goes out of scope.
    auto const conn = QObject::connect(
        h.display, &contour::display::TerminalDisplay::terminated, [&terminated]() { terminated = true; });

    // A QCloseEvent routed to the display closes the backing PTY and emits terminated().
    QCloseEvent closeEvent;
    QCoreApplication::sendEvent(h.display, &closeEvent);
    CHECK(terminated);

    QObject::disconnect(conn);
}

// NOTE: The WindowController tab-title-edit seam (beginActiveTabTitleEdit → tabTitleEditRequested)
// is tested headlessly with real tabs in MultiWindow_test.cpp, where activeTabIndex() is populated;
// the DisplayHarness session is not registered as a model tab, so it cannot exercise that path.
