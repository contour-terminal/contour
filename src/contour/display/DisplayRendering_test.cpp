// SPDX-License-Identifier: Apache-2.0
//
// Display-gated end-to-end rendering tests: a REAL TerminalDisplay bound to a REAL TerminalSession
// (vtpty::ChannelPty-backed, live read loop) inside a real QQuickWindow, with frames forced through
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
//
// Two cases at the very bottom are the exception and run UNGATED: they need a display that never had a
// window (hence never had a render target), which the offscreen platform supplies perfectly well. See
// the teardown-lifetimes section there.

#include <contour/config/Actions.hpp>
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Primitives.hpp>

#include <vtpty/ChannelPty.hpp>

#include <QtCore/QBuffer>
#include <QtCore/QDir>
#include <QtCore/QRunnable>
#include <QtCore/QSemaphore>
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
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>

#include <QtTest/QTest>
#include <vtworkspace/Pane.hpp>
#include <vtworkspace/SessionModel.hpp>
#include <vtworkspace/Tab.hpp>

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

/// Counts the accessibility events Qt is handed, and how many it would discard.
///
/// Static state in a free function because QAccessible::installUpdateHandler takes a plain function
/// pointer. @see the union hazard documented at notifyAbout() in TerminalAccessible.cpp.
struct AccessibleEventProbe
{
    static inline int Emitted = 0;
    static inline int Undeliverable = 0;

    static void reset() noexcept
    {
        Emitted = 0;
        Undeliverable = 0;
    }

    static void handle(QAccessibleEvent* event)
    {
        ++Emitted;
        // Both halves of "Qt will actually deliver this": a QObject-subject event must carry no child
        // index, and the interface it resolves to must exist.
        if ((event->object() != nullptr && event->child() != -1) || event->accessibleInterface() == nullptr)
            ++Undeliverable;
    }
};

/// Whether the harness stands the display up inside a shown QQuickWindow.
///
/// Windowless is a real production state, not a degenerate one: a closed pane and an invalidated scene
/// graph both leave a live display with no window, hence no render target. Needs no display server, so
/// cases built on it run UNGATED.
enum class HarnessWindow : uint8_t
{
    None,
    Shown,
};

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
    contour::test::TestApp testApp;
    vtpty::ChannelPty* pty = nullptr; // owned by the session's terminal
    std::unique_ptr<contour::session::TerminalSession> session;
    std::unique_ptr<QQuickWindow> window;                    // null under HarnessWindow::None
    contour::display::TerminalDisplay* display = nullptr;    // manually deleted in teardown
    contour::window::WindowController* controller = nullptr; // manager-owned; removed in teardown

    explicit DisplayHarness(HarnessWindow windowMode = HarnessWindow::Shown):
        display(new contour::display::TerminalDisplay())
    {
        auto ptyOwned = std::make_unique<vtpty::ChannelPty>(
            vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
        pty = ptyOwned.get();
        session = std::make_unique<contour::session::TerminalSession>(
            &testApp.app().sessionsManager(), std::move(ptyOwned), testApp.app());

        if (windowMode == HarnessWindow::Shown)
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
        testApp.app().sessionsManager().focusOnDisplay(display);
        return *controller;
    }

    /// Forces one synchronous frame (scene-graph sync + render) and returns the grabbed image.
    ///
    /// Windowless there is no scene graph to drive, so this only drains the GUI queue (what the posted
    /// display/session lambdas need) and returns a null image; pixel assertions are all display-gated.
    QImage pump() const
    {
        QCoreApplication::processEvents();
        auto image = window ? window->grabWindow() : QImage {};
        QCoreApplication::processEvents();
        return image;
    }

    /// Feeds VT output and pumps frames until the read loop consumed it (bounded).
    void feedAndSettle(std::string_view vt) const
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
        delete display; // while the session still lives: ~contour::display::TerminalDisplay detaches from it
        display = nullptr;
        QCoreApplication::processEvents();
        session.reset(); // joins the session threads (PTY already closed)
        window.reset();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCoreApplication::processEvents();
    }
};

/// A font size no profile default uses, so "did the request land?" is unambiguous.
constexpr auto RequestedFontSize = 13.0;

/// An OSC 50 font request of @p size, leaving every family at "inherit" (an empty string).
[[nodiscard]] vtbackend::FontDef fontRequest(double size)
{
    return vtbackend::FontDef {
        .size = size, .regular = "", .bold = "", .italic = "", .boldItalic = "", .emoji = ""
    };
}

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
    CHECK(pixels->size() == static_cast<std::size_t>(4u * 3u * 4u)); // width * height * RGBA

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
    DisplayHarness const h;

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

namespace
{
/// The bounding box of every pixel that differs between @p before and @p after.
///
/// What a selection paints is a background change, not ink, so it cannot be found by brightness.
/// Comparing the two frames names exactly the region the selection touched and nothing else.
[[nodiscard]] QRect changedBounds(QImage const& before, QImage const& after)
{
    if (before.size() != after.size())
        return {};

    auto left = before.width();
    auto right = -1;
    auto top = before.height();
    auto bottom = -1;

    for (auto y = 0; y < before.height(); ++y)
        for (auto x = 0; x < before.width(); ++x)
            if (before.pixel(x, y) != after.pixel(x, y))
            {
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }

    if (right < 0)
        return {};
    return { QPoint(left, top), QPoint(right, bottom) };
}
} // namespace

TEST_CASE("display: a scaled block draws at its full height", "[display][render][textsizing]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    // The property, stated so that it needs no knowledge of the font or the cell size: the same
    // glyph at `s=2` must cover about twice the height it does unscaled. Comparing the two renders
    // is what makes this immune to whichever font the test machine resolves.
    //
    // Each glyph is measured as the difference from the SAME screen without it, which isolates
    // exactly the pixels that glyph is responsible for. The cursor is hidden first, or it would
    // contribute its own block of changed pixels one cell to the right.
    //
    // The cursor is put on the LAST line before writing, which is where a terminal that has printed
    // anything actually sits -- and therefore the state nearly every block a real program writes
    // arrives in. Written at the top of a fresh screen there is always room below, so the block is
    // never asked to make any, and this passes whether or not the backend can.
    auto const measureGlyph = [&](std::string_view payload) {
        h.feedAndSettle("\033[?25l\033[2J\033[999;1H"sv);
        auto const blank = h.pump();
        h.feedAndSettle(payload);
        return changedBounds(blank, h.pump());
    };

    auto const ordinary = measureGlyph("X"sv);
    REQUIRE(ordinary.isValid());

    auto const scaled = measureGlyph("\033]66;s=2;X\a"sv);
    REQUIRE(scaled.isValid());

    INFO("ordinary " << ordinary.width() << "x" << ordinary.height() << ", scaled " << scaled.width() << "x"
                     << scaled.height());

    // Generous bounds: hinting at the larger size legitimately shifts the ink by a pixel or two, so
    // this asserts the SHAPE of the result -- roughly twice as tall and wide -- not exact geometry.
    // A block drawing only its head row lands at ~1x here, which is the failure this gates.
    CHECK(scaled.height() >= ordinary.height() * 3 / 2);
    CHECK(scaled.width() >= ordinary.width() * 3 / 2);
}

TEST_CASE("display: a scaled block is selected over its whole height", "[display][render][textsizing]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    h.feedAndSettle("\033[?25l\033[2J\033[999;1H"sv);
    auto const blank = h.pump();
    h.feedAndSettle("\033]66;s=2;X\a"sv);
    auto const unselected = h.pump();
    auto const glyph = changedBounds(blank, unselected);
    REQUIRE(glyph.isValid());

    // Select the block by dragging across its FIRST row only. A block is indivisible, so the
    // highlight must cover both of its rows -- and therefore reach below the glyph's own ink.
    h.session->terminal().markScreenDirty();
    {
        auto const _ = std::scoped_lock { h.session->terminal() };
        h.session->terminal().setSelector(std::make_unique<vtbackend::LinearSelection>(
            h.session->terminal().selectionHelper(),
            vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                      .column = vtbackend::ColumnOffset(0) },
            [](auto&&...) {}));
        (void) h.session->terminal().selector()->extend(vtbackend::CellLocation {
            .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) });
        h.session->terminal().selector()->complete();
    }

    auto const selected = h.pump();
    auto const highlight = changedBounds(unselected, selected);
    REQUIRE(highlight.isValid());

    INFO("glyph ink " << glyph.top() << ".." << glyph.bottom() << ", highlight " << highlight.top() << ".."
                      << highlight.bottom());

    // The highlight spans the whole block, so it reaches at least as low as the glyph's own ink --
    // the block is indivisible, and selecting its first row selects all of it.
    CHECK(highlight.bottom() >= glyph.bottom());
    CHECK(highlight.height() >= glyph.height());
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
    for (int i = 0; i < 50 && !h.pty->stdinSnapshot().contains('a'); ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().contains('a'));
    CHECK(h.pty->stdinSnapshot().contains('\r'));

    // Enable X10 mouse reporting, then click inside the grid: the terminal must encode a report.
    h.feedAndSettle("\033[?1000h"sv);
    QTest::mouseClick(h.window.get(), Qt::LeftButton, Qt::NoModifier, QPoint(100, 100));
    for (int i = 0; i < 50 && !h.pty->stdinSnapshot().contains("\033[M"); ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().contains("\033[M"));

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
    CHECK(snapshot.contains("\033[<64;"));
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
                     &contour::session::TerminalSession::requestPermissionForFontChange,
                     h.session.get(),
                     [&fontAsks]() { ++fontAsks; });

    h.session->setFontDef(fontRequest(RequestedFontSize));
    for (int i = 0; i < 50 && fontAsks == 0; ++i)
        QTest::qWait(10);
    REQUIRE(fontAsks == 1);
    h.session->applyPendingFontChange(/*allow=*/true, /*remember=*/true);
    QTest::qWait(20);
    h.pump();

    h.session->setFontDef(fontRequest(RequestedFontSize + 1.0));
    for (int i = 0; i < 50 && h.display->fontSize().pt < 13.9; ++i)
        QTest::qWait(10);
    CHECK(fontAsks == 1); // remembered: applied without asking again

    // CaptureBuffer rides the same queue. requestCaptureBuffer() only ENQUEUES the request and posts the
    // gate, so the dialog has to be waited for and answered through it -- answering the executor before
    // the gate arrives would leave the gate to raise a dialog for a request already served.
    auto captureAsks = 0;
    QObject::connect(h.session.get(),
                     &contour::session::TerminalSession::requestPermissionForBufferCapture,
                     h.session.get(),
                     [&captureAsks]() { ++captureAsks; });

    h.feedAndSettle("capture me\r\n"sv);
    h.session->requestCaptureBuffer(vtbackend::LineCount(2), /*logical=*/false);
    for (int i = 0; i < 50 && captureAsks == 0; ++i)
        QTest::qWait(10);
    REQUIRE(captureAsks == 1);
    h.session->executePendingBufferCapture(/*allow=*/true, /*remember=*/false);
    QTest::qWait(20);

    // A remembered deny resolves the next request without asking again -- and still answers it, since a
    // refused capture owes its client the terminating chunk.
    h.session->requestCaptureBuffer(vtbackend::LineCount(1), /*logical=*/true);
    for (int i = 0; i < 50 && captureAsks == 1; ++i)
        QTest::qWait(10);
    REQUIRE(captureAsks == 2);
    h.session->executePendingBufferCapture(/*allow=*/false, /*remember=*/true);
    h.session->requestCaptureBuffer(vtbackend::LineCount(1), /*logical=*/true);
    QTest::qWait(20);
    h.pump();
    CHECK(captureAsks == 2); // remembered: resolved without asking again

    // ShowHostWritableStatusLine rides the display queue too; a remembered deny resolves silently.
    h.session->executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/true);
    h.pump();
}

TEST_CASE("display: bell rings the session signals and the alert path", "[display][bell]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness const h;

    auto bells = 0;
    auto alerts = 0;
    QObject::connect(h.session.get(),
                     &contour::session::TerminalSession::onBell,
                     h.session.get(),
                     [&bells](float) { ++bells; });
    QObject::connect(h.session.get(),
                     &contour::session::TerminalSession::onAlert,
                     h.session.get(),
                     [&alerts]() { ++alerts; });

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
    CHECK(h.display->setFontSize(text::FontSize { 14.0 }));
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
    CHECK(h.display->setFontSize(text::FontSize { 24.0 }));
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
    CHECK(h.display->setFontSize(text::FontSize { 8.0 }));
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
    auto secondPty = std::make_unique<vtpty::ChannelPty>(
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
    auto sessionB = std::make_unique<contour::session::TerminalSession>(
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
    for (auto const shape: { contour::input::MouseCursorShape::Hidden,
                             contour::input::MouseCursorShape::PointingHand,
                             contour::input::MouseCursorShape::IBeam,
                             contour::input::MouseCursorShape::Arrow })
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
    controller.updateSizeHintsFor(*h.display,
                                  contour::window::WindowController::HintApplyMode::RespectWindowState);
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
    for (int i = 0; i < 50 && !h.pty->stdinSnapshot().contains('Z'); ++i)
        QTest::qWait(10);
    CHECK(h.pty->stdinSnapshot().contains('Z'));

    // The IME queries the display for its cursor rectangle / font / anchor — all must return
    // without crashing while a session is attached.
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImCursorRectangle));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImFont));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImAnchorRectangle));
    CHECK_NOTHROW(h.display->inputMethodQuery(Qt::ImEnabled));
}

TEST_CASE("display: the accessibility interface reports the caret", "[display][a11y]")
{
    // The event side cannot be asserted here: delivery needs an attached assistive client, and neither
    // the offscreen platform nor CI has one. So this drives the interface DIRECTLY -- which is also what
    // an AT-SPI or UIA bridge does once a client attaches. The decisions those events are built from are
    // unit-tested without Qt at all (CaretReportGate_test, CellRectangle_test, ViewportTextIndex_test).
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    contour::display::TerminalAccessible::installFactory();

    auto* accessible = QAccessible::queryAccessibleInterface(h.display);
    REQUIRE(accessible != nullptr);
    CHECK(accessible->role() == QAccessible::Terminal);
    CHECK(accessible->isValid());

    auto* textInterface =
        static_cast<QAccessibleTextInterface*>(accessible->interface_cast(QAccessible::TextInterface));
    REQUIRE(textInterface != nullptr);

    // The caret offset tracks what the terminal was told to draw.
    h.feedAndSettle("\033[1;1H"sv);
    auto const atOrigin = textInterface->cursorPosition();
    h.feedAndSettle("\033[3;5H"sv);
    auto const moved = textInterface->cursorPosition();
    CHECK(moved != atOrigin);

    // characterRect is what a magnifier reads to place its viewport, so it must be a real, non-empty
    // rectangle rather than a default-constructed one.
    auto const caretRect = textInterface->characterRect(moved);
    CHECK(caretRect.width() > 0);
    CHECK(caretRect.height() > 0);

    CHECK(textInterface->characterCount() > 0);
    CHECK_NOTHROW(accessible->rect());
    CHECK_NOTHROW(h.display->reportAccessibleCaret());
    CHECK_NOTHROW(h.display->resetAccessibleCaret());
}

TEST_CASE("display: the accessibility text interface reads the viewport column-aligned", "[display][a11y]")
{
    // This is the read a platform bridge issues after every caret move -- macOS turns AXValue into
    // text(0, characterCount()) -- so it runs on the GUI thread, holding the terminal lock, once per
    // keystroke. It used to call lineTextAt() once per CHARACTER, which rebuilds the whole line each
    // time: quadratic in the line length, and wrong besides, because lineTextAt() TRIMS leading
    // whitespace, so a column offset indexed into a shifted string.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    contour::display::TerminalAccessible::installFactory();
    auto* accessible = QAccessible::queryAccessibleInterface(h.display);
    REQUIRE(accessible != nullptr);
    auto* text =
        static_cast<QAccessibleTextInterface*>(accessible->interface_cast(QAccessible::TextInterface));
    REQUIRE(text != nullptr);

    // Read the grid width rather than assuming it: the harness reflows its page to the display's pixel
    // size, so the flat layout's stride is whatever that reflow settled on. A row is its cells, then
    // the newline that ends it.
    auto const columns = [&] {
        auto const lock = std::lock_guard { h.session->terminal() };
        return unbox<int>(h.session->terminal().pageSize().columns);
    }();
    auto const stride = columns + 1;

    // Catch2 cannot print a QString, so every comparison goes through std::string.
    auto const readAt = [&](int from, int to) {
        return text->text(from, to).toStdString();
    };

    // Two LEADING spaces on purpose -- that is the half the trimming bug destroyed.
    h.feedAndSettle("\033[2J\033[1;1H  indented\033[2;1Hsecond"sv);

    CHECK(readAt(0, 10) == "  indented");
    CHECK(readAt(0, 2) == "  ");

    // A row boundary yields the newline, and only it.
    CHECK(readAt(columns, columns + 1) == "\n");
    CHECK(readAt(columns - 1, columns + 2) == " \ns");

    // The next row starts one stride along.
    CHECK(readAt(stride, stride + 6) == "second");

    // The whole viewport is exactly as long as the offset space it is addressed by.
    auto const whole = text->text(0, text->characterCount());
    CHECK(whole.size() == text->characterCount());
    CHECK(whole.toStdString().starts_with("  indented"));

    SECTION("a wide character's continuation cell reads as one padding space")
    {
        // U+4F60 U+597D, two columns each, then an ASCII column: five columns, five codepoints.
        h.feedAndSettle("\033[2J\033[1;1H\xe4\xbd\xa0\xe5\xa5\xbdX"sv);
        CHECK(readAt(0, 5) == "\xe4\xbd\xa0 \xe5\xa5\xbd X");
    }

    SECTION("a non-BMP character survives as a whole surrogate pair")
    {
        // U+1F600, two columns wide and TWO UTF-16 code units, so a column is no longer a QChar index.
        // The old mid(column, 1) sliced the pair and handed the OS a lone high surrogate.
        h.feedAndSettle("\033[2J\033[1;1H\xf0\x9f\x98\x80"sv);
        CHECK(readAt(0, 1) == "\xf0\x9f\x98\x80");
        CHECK(text->text(0, 1).size() == 2); // both halves, not one
    }
}

TEST_CASE("display: every event a caret report emits is one Qt will deliver", "[display][a11y]")
{
    // The union hazard (see notifyAbout() in TerminalAccessible.cpp) makes a mis-subjected event fail
    // SILENTLY -- Qt drops it and prints "Invalid child in QAccessibleEvent". So assert on the whole
    // prompt lifecycle, which is what exercises every emission site: appear, move, vanish.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;

    contour::display::TerminalAccessible::installFactory();
    h.display->forceActiveFocus();

    auto* accessible =
        dynamic_cast<contour::display::TerminalAccessible*>(QAccessible::queryAccessibleInterface(h.display));
    REQUIRE(accessible != nullptr);

    AccessibleEventProbe::reset();
    auto* const previous = QAccessible::installUpdateHandler(&AccessibleEventProbe::handle);

    // Driven directly: the production entry point gates on an attached assistive client, and neither
    // this environment nor CI has one.
    h.feedAndSettle("\033]133;A\033\\$ \033]133;B\033\\"sv); // prompt appears
    accessible->reportCaret();
    auto const afterAppear = AccessibleEventProbe::Emitted;

    h.feedAndSettle("x"sv); // caret walks the prompt
    accessible->reportCaret();

    h.feedAndSettle("\r\n"sv); // prompt scrolls / the command runs
    accessible->reportCaret();

    accessible->reportLocation();

    QAccessible::installUpdateHandler(previous);

    CHECK(afterAppear > 0); // the prompt branch really ran
    CHECK(AccessibleEventProbe::Emitted > afterAppear);
    CHECK(AccessibleEventProbe::Undeliverable == 0);
}

TEST_CASE("display: the prompt interface leaves Qt's accessibility cache with its display", "[display][a11y]")
{
    // Issue #2015, end to end: a real session whose OSC 133 marks make the prompt genuinely appear, so
    // the interface is registered by the production path rather than by the test. ~TerminalAccessible
    // explains the ownership rule this asserts; TerminalAccessible_test.cpp gates it in CI.
    //
    // Asserting on the id rather than on the interface keeps the check itself safe: it compares
    // pointers and never dereferences a freed one.
    REQUIRE_DISPLAY_OR_SKIP();
    auto harness = std::make_unique<DisplayHarness>();

    contour::display::TerminalAccessible::installFactory();
    harness->display->forceActiveFocus();

    // OSC 133 A/B: the prompt-start / prompt-end pair that gives livePromptSpan() something to report,
    // and thus the only thing that brings the prompt child interface into existence.
    harness->feedAndSettle("\033]133;A\033\\$ \033]133;B\033\\"sv);

    auto* accessible = dynamic_cast<contour::display::TerminalAccessible*>(
        QAccessible::queryAccessibleInterface(harness->display));
    REQUIRE(accessible != nullptr);

    // Driven directly: reportAccessibleCaret() gates on an attached assistive client, and neither the
    // test environment nor CI has one.
    accessible->reportCaret();
    REQUIRE(accessible->childCount() == 1); // the prompt branch really ran

    auto* prompt = accessible->child(0);
    REQUIRE(prompt != nullptr);
    auto const promptId = QAccessible::uniqueId(prompt);
    REQUIRE(QAccessible::accessibleInterface(promptId) == prompt);

    harness.reset(); // deletes the display, and with it the terminal's accessible interface

    CHECK(QAccessible::accessibleInterface(promptId) == nullptr);
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
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::input::MouseCursorShape::Hidden));
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::input::MouseCursorShape::IBeam));
    CHECK_NOTHROW(h.display->setMouseCursorShape(contour::input::MouseCursorShape::PointingHand));
    h.pump();
}

namespace
{
/// Sends a synthetic QMouseEvent of @p type/@p button at the display-item-local logical position @p
/// pos, exactly as the real windowing system would deliver it to TerminalDisplay's event handlers.
void sendMouse(contour::display::TerminalDisplay* display,
               QEvent::Type type,
               QPointF pos,
               Qt::MouseButton button = Qt::LeftButton)
{
    QMouseEvent ev(type, pos, display->mapToGlobal(pos), button, button, Qt::NoModifier);
    QCoreApplication::sendEvent(display, &ev);
}

/// The item-local LOGICAL pixel position of a grid cell's top-left corner.
///
/// makeMouseCellLocation() (SessionInput.cpp) walks this mapping in reverse -- logical position *
/// devicePixelRatio -> DEVICE pixel -> minus the renderer's pageMargin -> divided by the device
/// cellSize -> grid row/column. Going forward through the identical quantities (gridMetrics(),
/// devicePixelRatio()) is what makes a synthetic drag name an EXACT cell regardless of font metrics
/// or the test machine's DPI, rather than a guessed pixel offset that happens to land inside one.
[[nodiscard]] QPointF logicalCellTopLeft(contour::display::TerminalDisplay& display,
                                         vtbackend::LineOffset line,
                                         vtbackend::ColumnOffset column)
{
    // Offset by the main page's top row, because this is the INVERSE of what the production hit-test
    // does: geometry::mainPageRowNear subtracts mainPageTopRow() to turn a pixel into a main-page
    // relative line, while mapTopLeft treats its argument as a SCREEN row. The two agree only while
    // that row is zero -- the default bottom status line -- so without this a profile with a top
    // status line would land every synthetic click statusLineHeight() rows above its target, and the
    // failure would read as a selection bug rather than a test-harness one.
    auto const screenLine = line + unbox<int>(display.session().terminal().mainPageTopRow());
    auto const devicePoint = display.gridMetrics().mapTopLeft(screenLine, column);
    auto const dpr = display.devicePixelRatio();
    return { double(devicePoint.x) / dpr, double(devicePoint.y) / dpr };
}

/// The item-local logical pixel position of a point INSIDE a grid cell (its top-left plus a half-cell
/// nudge), so a drag reliably lands inside the target cell rather than exactly on its boundary, where
/// rounding could tip it into the previous row/column.
[[nodiscard]] QPointF logicalCellCenter(contour::display::TerminalDisplay& display,
                                        vtbackend::LineOffset line,
                                        vtbackend::ColumnOffset column)
{
    auto const topLeft = logicalCellTopLeft(display, line, column);
    auto const cellSize = display.gridMetrics().cellSize;
    auto const dpr = display.devicePixelRatio();
    return topLeft
           + QPointF(double(cellSize.width.as<int>()) / dpr / 2.0,
                     double(cellSize.height.as<int>()) / dpr / 2.0);
}
} // namespace

TEST_CASE("display: a mouse drag spanning multiple lines selects the exact cell range", "[display][mouse]")
{
    // The headless Selection/Selector tests (Terminal_selection_test.cpp) inject CellLocation
    // directly and never exercise the pixel math a real drag runs through -- makeMouseCellLocation's
    // device-pixel round trip via gridMetrics().pageMargin, cellSize and mainPageRowNear. Driving
    // real QMouseEvents through the live TerminalDisplay is the only path that also exercises that
    // translation, so this is where a rounding or off-by-one in the pixel->cell mapping would show up
    // for a selection spanning more than one row -- as opposed to a bug in Selector::ranges() itself,
    // which is already covered headlessly.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("0123456789\r\n"
                    "ABCDEFGHIJ\r\n"
                    "abcdefghij\r\n"
                    "9876543210\r\n");

    auto const from =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) };
    auto const to =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(4) };

    // The MouseMove before the press is what a real windowing system always delivers -- the pointer
    // arrives via motion before a button event fires -- so the drags below are written in that order
    // to stay faithful to it, not because the press needs it: sendMousePressEvent now anchors at the
    // cell it was given. The bare-press case, which a fast click really does produce, is covered by
    // its own test rather than by omitting the move here.
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, from.line, from.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, from.line, from.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();

    REQUIRE(h.session->terminal().selectionAvailable());
    CHECK(h.session->terminal().isSelectionComplete());

    // Row 0 from column 3 to the right margin, row 1 whole, row 2 from column 0 up to column 4 --
    // exactly Selection::ranges()'s documented 3-line shape (first partial, inner full, last partial).
    CHECK(h.session->terminal().extractSelectionText()
          == "3456789\n"
             "ABCDEFGHIJ\n"
             "abcde");
}

TEST_CASE("display: dragging a double-clicked word past its line freezes the search highlight",
          "[display][mouse]")
{
    // Regression: updateSelectionMatches() re-derives the search pattern from the CURRENT selection
    // text on every mouse-move of a WordWiseSelection, so its other-occurrences highlight tracks a
    // double-click as it grows into a longer word. But RenderBufferBuilder's search-match scanner
    // matches the pattern's bytes -- newlines included -- against the flat, single-line stream of
    // rendered cells; once the drag crosses a line boundary the "word" becomes a multi-line blob that
    // keeps changing on every further move and essentially never matches anything stable, so the
    // highlight visibly jumped around the screen for as long as the drag continued. The fix freezes
    // the pattern the moment the selection stops fitting on one line, rather than feeding the scanner
    // an ever-growing, newline-bearing target.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("hello world\r\n"
                    "second row\r\n"
                    "third here\r\n");

    // Double-click on "hello" (column 2, line 0): press, release, then a second press at the same
    // spot within the terminal's 1000ms speed-click window.
    auto const wordPos =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(2) };
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, wordPos.line, wordPos.column));
    h.pump();
    sendMouse(
        h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, wordPos.line, wordPos.column));
    h.pump();
    sendMouse(
        h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, wordPos.line, wordPos.column));
    h.pump();
    QTest::qWait(100);
    sendMouse(
        h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, wordPos.line, wordPos.column));
    h.pump();

    REQUIRE(h.session->terminal().extractSelectionText() == "hello");
    auto const patternAfterDoubleClick = h.session->terminal().search().pattern;
    CHECK(patternAfterDoubleClick == U"hello");

    // Drag across two further line boundaries (in one jump, as a fast drag would deliver it).
    auto const dragTo =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(3) };
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, dragTo.line, dragTo.column));
    h.pump();

    // The selected text keeps growing normally...
    CHECK(h.session->terminal().extractSelectionText()
          == "hello world\n"
             "second row\n"
             "third");
    // ...but the search pattern must have frozen at the original single-line word, not followed it.
    CHECK(h.session->terminal().search().pattern == patternAfterDoubleClick);

    sendMouse(
        h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, dragTo.line, dragTo.column));
    h.pump();
}

TEST_CASE("display: a press with no fresh preceding move still anchors at the press position",
          "[display][mouse]")
{
    // Regression for the "fast drag drifts" bug: Terminal::sendMousePressEvent has a pixelPosition
    // argument but never converts it to a cell -- handleMouseSelection() anchors on
    // _currentMousePosition, which ONLY Terminal::sendMouseMoveEvent ever writes. A real windowing
    // system usually delivers a hover/move at the click point before the press, which is why a SLOW,
    // deliberate drag hides this: the anchor is fresh. A fast flick can arrive as a bare press with the
    // pointer's last recorded position stale from wherever it idled beforehand (a prior selection, a
    // different pane, ...), so the drag then extends from that stale point instead of the actual click
    // -- the whole selected region appears to have "moved".
    //
    // This is reproduced here directly: move to one spot, then press and drag at a DIFFERENT spot with
    // no move in between -- exactly the gap between the two event types the fix must close.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("0123456789\r\n"
                    "ABCDEFGHIJ\r\n"
                    "abcdefghij\r\n"
                    "9876543210\r\n");

    auto const stalePosition =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(4) };
    auto const from =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) };
    auto const to =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(4) };

    // Leave the pointer's last-known position somewhere else entirely (no press here -- this only
    // updates the hover-tracked _currentMousePosition, mirroring the pointer idling after a previous,
    // unrelated interaction).
    sendMouse(h.display,
              QEvent::MouseMove,
              logicalCellCenter(*h.display, stalePosition.line, stalePosition.column));
    h.pump();

    // Press directly at `from` with NO intervening MouseMove there -- the gap a fast flick leaves.
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, from.line, from.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();

    // The drag must run from the ACTUAL press position (from) to `to`, not from the stale hover
    // position -- i.e. the selection must not have silently anchored somewhere the pointer never was
    // pressed down.
    CHECK(h.session->terminal().extractSelectionText()
          == "3456789\n"
             "ABCDEFGHIJ\n"
             "abcde");
}

TEST_CASE("display: dragging upward across lines selects the same range as dragging downward",
          "[display][mouse]")
{
    // Selection::ranges() normalizes from/to by lexicographic order before building per-line ranges
    // (Selector.cpp's prepare()); the direction the mouse actually moved must not matter to the
    // resulting text. This pins that symmetry through the real pixel-driven mouse path rather than
    // through Selection::extend() called directly.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("0123456789\r\n"
                    "ABCDEFGHIJ\r\n"
                    "abcdefghij\r\n");

    auto const top =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(2) };
    auto const bottom =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(6) };

    // Downward: press at top, release at bottom. Every press is preceded by a MouseMove to the SAME
    // position, because that is what a real windowing system does -- the pointer arrives via motion
    // before a button event fires. (The press itself now also anchors at the cell it landed on, so
    // this sequence no longer DEPENDS on the preceding move; it stays because it is the realistic
    // event order, and the bare-press case has its own test above.)
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, bottom.line, bottom.column));
    h.pump();
    sendMouse(
        h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, bottom.line, bottom.column));
    h.pump();
    auto const downward = h.session->terminal().extractSelectionText();

    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    REQUIRE(h.session->terminal().extractSelectionText().empty()); // plain click cleared it

    // The terminal's own click-speed counter (_speedClicks in Terminal::handleMouseSelection) counts
    // presses within 1s of REAL elapsed time (Terminal::_currentTime, advanced from steady_clock by the
    // live display's render loop -- unlike the headless MockTerm tests, which advance a simulated clock
    // explicitly). Without a real pause here, this press lands within that window of the deselect click
    // above and the one before it, so it reads as a double/triple-click and selects a word or a whole
    // line instead of starting a plain drag.
    QTest::qWait(1100);

    // Upward: press at bottom, release at top.
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, bottom.line, bottom.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, bottom.line, bottom.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, top.line, top.column));
    h.pump();
    auto const upward = h.session->terminal().extractSelectionText();

    CHECK(downward
          == "23456789\n"
             "ABCDEFGHIJ\n"
             "abcdefg");
    CHECK(upward == downward);
}

TEST_CASE("display: a multi-line drag selection extends and shrinks live as the mouse moves",
          "[display][mouse]")
{
    // A selection is a live, redrawn thing while InProgress, not just a value read once at release.
    // Each intermediate MouseMove must already report the range up to THAT point -- so this checks
    // extractSelectionText() after every move, not only after the final release.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    h.feedAndSettle("first line\r\n"
                    "second row\r\n"
                    "third here\r\n");

    auto const anchor =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(0) };
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, anchor.line, anchor.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, anchor.line, anchor.column));
    h.pump();

    // Still on line 0: single-line partial selection.
    sendMouse(h.display,
              QEvent::MouseMove,
              logicalCellCenter(*h.display, vtbackend::LineOffset(0), vtbackend::ColumnOffset(4)));
    h.pump();
    CHECK(h.session->terminal().extractSelectionText() == "first");

    // Extends onto line 1: two-line selection, first line taken whole to its right margin.
    sendMouse(h.display,
              QEvent::MouseMove,
              logicalCellCenter(*h.display, vtbackend::LineOffset(1), vtbackend::ColumnOffset(5)));
    h.pump();
    CHECK(h.session->terminal().extractSelectionText()
          == "first line\n"
             "second");

    // Extends onto line 2: three-line selection -- first and second line each taken whole, third
    // partial. This is the "middle line(s) rendered full" branch of Selector.cpp's ranges().
    sendMouse(h.display,
              QEvent::MouseMove,
              logicalCellCenter(*h.display, vtbackend::LineOffset(2), vtbackend::ColumnOffset(4)));
    h.pump();
    CHECK(h.session->terminal().extractSelectionText()
          == "first line\n"
             "second row\n"
             "third");

    // Retracting back onto line 1 must shrink the selection again, not merely stop growing it.
    sendMouse(h.display,
              QEvent::MouseMove,
              logicalCellCenter(*h.display, vtbackend::LineOffset(1), vtbackend::ColumnOffset(2)));
    h.pump();
    CHECK(h.session->terminal().extractSelectionText()
          == "first line\n"
             "sec");

    sendMouse(h.display,
              QEvent::MouseButtonRelease,
              logicalCellCenter(*h.display, vtbackend::LineOffset(1), vtbackend::ColumnOffset(2)));
    h.pump();
    CHECK(h.session->terminal().isSelectionComplete());
}

TEST_CASE("display: a multi-line drag selecting a wrapped logical line copies it as one run",
          "[display][mouse]")
{
    // A line that wraps is still ONE logical line: dragging from inside it, across the wrap point,
    // must not insert a newline at the wrap boundary the way it does at a real row break. This drives
    // that through the same pixel-mapped mouse path as the fixed-width case above, so a regression in
    // wrappedLine() handling under the real makeMouseCellLocation() path would show up here even
    // though it is invisible to a direct-CellLocation Selection test.
    //
    // Deliberately does NOT resize the window (CSI 8 needs a bound controller and a settled OS-window
    // round trip to actually change pageSize() -- see the controller-routed resize tests above): the
    // harness reflows the profile's default page to whatever the 800x600 display and its resolved font
    // actually fit, so the column count is read back rather than assumed -- writing a line ten columns
    // longer than that is enough to get a real wrap boundary at a known column.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.display->forceActiveFocus();
    // Settle any pending geometry/font reflow left over from a PREVIOUS test case before reading
    // pageSize() and deriving click positions from gridMetrics() -- both are snapshots of the live
    // renderer, and this suite runs every case in one process, so a font/DPI change a prior test made
    // can still be mid-flight (recomputeGeometryAfterFontReconfig runs async) when this one starts.
    h.pump();
    h.pump();
    auto const columns = unbox<int>(h.session->terminal().pageSize().columns);
    REQUIRE(columns > 0);

    // Row 0 fills the page exactly (built by repeating the alphabet, so it fits whatever width this
    // display/font combination actually resolves to); "mnop" is the wrapped continuation on row 1.
    auto constexpr Alphabet = "abcdefghijklmnopqrstuvwxyz"sv;
    auto rowOne = std::string {};
    while (rowOne.size() < size_t(columns))
        rowOne += Alphabet;
    rowOne.resize(size_t(columns));

    // No CR/LF between rowOne and "mnop": an autowrap only commits the Wrapped line flag
    // (Screen::crlfIfWrapPending) once the NEXT character is WRITTEN, and it does that as an IMPLICIT
    // wrap -- \r\n right after filling the last column instead runs linefeed() directly (LF is handled
    // by the parser, never reaching crlfIfWrapPending), which never sets the flag and would make this
    // a false negative rather than what it looks like it is testing.
    h.feedAndSettle(rowOne + "mnop");
    h.feedAndSettle("\r\nthird\r\n"sv);
    // isLineWrapped() marks the CONTINUATION line (row 1 here), not the line it wrapped from -- see
    // LineFlag::Wrapped, set on the chunk a wrap produces, never on the one it wrapped out of.
    REQUIRE(h.session->terminal().isLineWrapped(vtbackend::LineOffset(1)));

    // Row 0 holds `rowOne` (wrapped), row 1 holds "mnop" then a real line break, row 2 holds "third".
    auto const from = vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                .column = vtbackend::ColumnOffset(columns - 3) };
    auto const to =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(1), .column = vtbackend::ColumnOffset(1) };

    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, from.line, from.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonPress, logicalCellCenter(*h.display, from.line, from.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseMove, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();
    sendMouse(h.display, QEvent::MouseButtonRelease, logicalCellCenter(*h.display, to.line, to.column));
    h.pump();

    // No newline at the wrap point: the tail of row 0 ("xyz", its last 3 columns) concatenates
    // directly with the head of row 1 ("mn", its first 2 columns of "mnop").
    auto const expectedFirstPart = rowOne.substr(size_t(columns - 3), 3);
    CHECK(h.session->terminal().extractSelectionText() == expectedFirstPart + "mn");
}

TEST_CASE("display: focus in/out toggle the terminal's focus state on the live display", "[display][focus]")
{
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    auto& controller = h.bindController();
    auto& manager = h.testApp.app().sessionsManager();

    // A session is born unfocused (Settings::focused is false for every contour session) — focus is
    // granted by the manager, never assumed. @see TerminalSession's createSettingsFromConfig.
    REQUIRE_FALSE(h.session->terminal().focused());

    QFocusEvent focusIn(QEvent::FocusIn);
    QCoreApplication::sendEvent(h.display, &focusIn);
    h.pump();
    // A Qt focus-in on a display routes through the manager, which is the single VT-focus authority:
    // it focuses this session AND records the display's window as the focus owner.
    CHECK(h.session->terminal().focused());
    CHECK(manager.focusedWindow() == controller.windowId());

    QFocusEvent focusOut(QEvent::FocusOut);
    QCoreApplication::sendEvent(h.display, &focusOut);
    h.pump();
    CHECK_FALSE(h.session->terminal().focused());

    // Focus events must not desync the session — it stays alive and renders.
    CHECK(h.session->terminal().pageSize().lines.value > 0);
}

TEST_CASE("display: revoking the window's focus ownership leaves its live session unfocused",
          "[display][focus][window]")
{
    // Alt-tabbing away from Contour revokes the window's focus ownership, which must unfocus the live
    // session so a DECSET 1004 application is told. (That the QWindow::activeChanged signal is wired to
    // this revoke is pinned headlessly in FocusRouting_test; here it runs against a real display.)
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    auto& controller = h.bindController();
    auto& manager = h.testApp.app().sessionsManager();

    // Focus the session the way production does, through a Qt focus-in on its display.
    QFocusEvent focusIn(QEvent::FocusIn);
    QCoreApplication::sendEvent(h.display, &focusIn);
    h.pump();
    REQUIRE(h.session->terminal().focused());
    REQUIRE(manager.focusedWindow() == controller.windowId());

    manager.clearFocusedWindow(controller.windowId());
    h.pump();
    CHECK_FALSE(manager.focusedWindow().has_value());
    CHECK_FALSE(h.session->terminal().focused());
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
    // builds a live session+display but does NOT register it in the controller's vtworkspace window, so the
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
    auto secondPty = std::make_unique<vtpty::ChannelPty>(birthSize);
    auto second = std::make_unique<contour::session::TerminalSession>(
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

TEST_CASE("display: attaching a display to an already-closed session does not resize its dead PTY",
          "[display][session][close]")
{
    // Regression pin for the ConPTY teardown crash: closing a pane makes QML re-run the Loader
    // binding for the surviving panes, which re-enters TerminalDisplay::setSession() ->
    // TerminalSession::attachDisplay() while onClosed() is still unwinding. attachDisplay()
    // unconditionally called Terminal::resizeScreen(), so the resize reached a PTY that had already
    // been closed. On Windows that is fatal rather than merely pointless: ConPty::close()
    // invalidates the HPCON but leaves the slave alive, so the resize passed INVALID_HANDLE_VALUE
    // to ResizePseudoConsole, which dereferences it (access violation reading 0xffffffffffffffff).
    //
    // The unit-level half of this lives in vtpty's ConPty_test; here the whole GUI path is driven:
    // a real display re-attached to a real closed session must push no resize down to the device.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();

    // Close the session the way the shell exiting does: terminate() closes the PTY device and drives
    // the session to its closed state.
    h.session->terminate();
    for (int i = 0; i < 50 && !h.session->isClosed(); ++i)
    {
        QTest::qWait(10);
        QCoreApplication::processEvents();
    }
    REQUIRE(h.pty->isClosed());
    REQUIRE(h.session->isClosed());

    // Whatever the close path itself did is not what this test is about; only what the *re-attach*
    // does after it.
    auto const resizesBeforeReattach = h.pty->resizesAfterClose();

    // The re-attach QML performs on the surviving panes. Detach first so setSession() takes the
    // full attachDisplay() path rather than an early-out on an unchanged session pointer.
    h.display->setSession(nullptr);
    h.pump();
    h.display->setSession(h.session.get());
    h.pump();

    // Attached, and no geometry was pushed into the closed device.
    CHECK(h.session->display() == h.display);
    CHECK(h.pty->resizesAfterClose() == resizesBeforeReattach);
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
    DisplayHarness const h;
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

// {{{ GUI-thread / render-thread teardown lifetimes
//
// Two crashes found by running the gated cases above — a resource the render thread reads mid-frame,
// torn down by the GUI thread:
//
//   Require(_renderTarget != nullptr)  in setFonts()  <- TerminalSession::applyPendingFontChange
//   assert(_session != nullptr)        in terminal()  <- paint() <- prepareFrameRhi <- prepare()
//
// The first three cases run UNGATED: a display that never had a window never had a render target, which
// reproduces the abort exactly and pins it in CI everywhere. The last two need real in-flight frames.

TEST_CASE("display: a font change approved with no render target is deferred, not fatal",
          "[display][font][teardown]")
{
    DisplayHarness h { HarnessWindow::None };
    REQUIRE(h.display->hasSession());
    REQUIRE_FALSE(h.display->hasRenderTarget()); // never had a window, so never had one

    // OSC 50 in, permission answer back. setFonts() used to Require() a render target here and abort.
    h.session->setFontDef(fontRequest(RequestedFontSize));
    CHECK_NOTHROW(h.session->applyPendingFontChange(/*allow=*/true, /*remember=*/false));

    // Deferred, not dropped: recorded as this session's own font, which every re-seeding path applies
    // once a render target exists again.
    CHECK(h.session->profile().fonts.value().size.pt == RequestedFontSize);
}

TEST_CASE("display: a font change approved after the pane detached is a silent no-op",
          "[display][font][teardown]")
{
    DisplayHarness h { HarnessWindow::None };

    // Request while attached (setFontDef needs a display to post through), then lose the pane before the
    // answer — a split collapse or closed tab. applyPendingFontChange() dereferenced _display blindly.
    h.session->setFontDef(fontRequest(RequestedFontSize));
    h.display->setSession(nullptr);
    REQUIRE(h.session->display() == nullptr);

    CHECK_NOTHROW(h.session->applyPendingFontChange(/*allow=*/true, /*remember=*/false));

    // Re-attach so the fixture tears down along its normal path.
    h.display->setSession(h.session.get());
}

TEST_CASE("display: a session destroyed before its display leaves no dangling back-pointer",
          "[display][session][teardown]")
{
    // Teardown in the order no fixture used: SESSION first, while the display still names it. Only
    // ~TerminalDisplay detached, so this direction left _session dangling — and dangling is non-null, so
    // the frame path's guards pass it through. Built by hand: the harness deletes the display first.
    contour::test::TestApp testApp;
    auto display = std::make_unique<contour::display::TerminalDisplay>();
    auto session = std::make_unique<contour::session::TerminalSession>(
        &testApp.app().sessionsManager(),
        std::make_unique<vtpty::ChannelPty>(
            vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) }),
        testApp.app());

    display->setSize(QSizeF(800, 600));
    display->setSession(session.get());
    REQUIRE(display->hasSession());
    REQUIRE(session->display() == display.get());

    session->terminate();
    QCoreApplication::processEvents();
    session.reset();

    // Null rather than dangling, and the display is still safe to use and to destroy.
    CHECK_FALSE(display->hasSession());
    CHECK_NOTHROW(display->scheduleRedraw());
    CHECK_NOTHROW(display.reset());
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCoreApplication::processEvents();
}

TEST_CASE("display: a font change approved across a render-target teardown still lands",
          "[display][font][teardown]")
{
    // The gated half of the first case: the render target really goes and comes back. fontSize() reads
    // the *published* descriptions, so this is the only place the deferral is observable end-to-end.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();
    REQUIRE(h.display->hasRenderTarget());
    auto const sizeBefore = h.display->fontSize().pt;
    REQUIRE(sizeBefore < RequestedFontSize);

    h.session->setFontDef(fontRequest(RequestedFontSize));

    // Destroy it the way a scene-graph invalidation does: on the render thread, where RHI resources
    // must be freed.
    QSemaphore released;
    h.window->scheduleRenderJob(QRunnable::create([display = h.display, &released]() {
                                    display->releaseRenderResources();
                                    released.release();
                                }),
                                QQuickWindow::NoStage);
    released.acquire();
    REQUIRE_FALSE(h.display->hasRenderTarget());

    // The answer arrives with no render target to apply it to: staged only.
    h.session->applyPendingFontChange(/*allow=*/true, /*remember=*/false);
    CHECK(h.display->fontSize().pt == sizeBefore); // nothing published yet

    // The next sync re-enters createRenderer(), which materializes whatever is staged.
    for (int i = 0; i < 50 && h.display->fontSize().pt < RequestedFontSize; ++i)
    {
        QTest::qWait(10);
        h.pump();
    }
    CHECK(h.display->hasRenderTarget());
    CHECK(h.display->fontSize().pt == RequestedFontSize);
}

TEST_CASE("display: rebinding the session under live frames does not tear a frame apart",
          "[display][session][teardown]")
{
    // The second crash: a rebinding pane torn out from under an in-flight frame. Why that races, and why
    // a fence answers it, is at TerminalDisplay::fenceRenderThread() — the correctness rests on that
    // argument, since this can only reproduce the shape, not an interleaving.
    //
    // QTest::qWait, NOT pump(): grabWindow() blocks the GUI thread for the whole frame, which is the one
    // interleaving that cannot crash. Only the free-running render loop can.
    REQUIRE_DISPLAY_OR_SKIP();
    DisplayHarness h;
    h.pump();

    auto secondPty = std::make_unique<vtpty::ChannelPty>(
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) });
    auto* second = secondPty.get();
    auto secondSession = std::make_unique<contour::session::TerminalSession>(
        &h.testApp.app().sessionsManager(), std::move(secondPty), h.testApp.app());

    for (auto round = 0; round < 10; ++round)
    {
        // Keep both terminals dirty so the render loop really has frames to run.
        h.pty->feed("first\r\n"sv);
        second->feed("second\r\n"sv);

        h.display->setSession(secondSession.get());
        QTest::qWait(5);
        h.display->setSession(h.session.get());
        QTest::qWait(5);
    }
    h.pump();

    // Both terminals survived, and the display ended up bound to the one it last refit.
    CHECK(h.session->display() == h.display);
    CHECK(h.session->terminal().totalPageSize().lines > vtbackend::LineCount(0));
    CHECK(secondSession->terminal().totalPageSize().lines > vtbackend::LineCount(0));

    secondSession->terminate();
    QCoreApplication::processEvents();
    secondSession.reset();
}
// }}}
