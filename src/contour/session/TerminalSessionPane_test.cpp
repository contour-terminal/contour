// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession tests, continued: input modes and the threading boundary, scrollbar visibility,
// the size indicator, the link target and drops onto a pane.

#include <contour/ContourGuiApp.hpp>
#include <contour/command/Shortcut.hpp>
#include <contour/config/Actions.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/SizeIndicator.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/session/TerminalSessionTestSupport.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/HostChipState.hpp>
#include <contour/window/PaneState.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Escape.hpp>
#include <core/Utils.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtGui/QClipboard>
#include <QtGui/QColor>
#include <QtGui/QExposeEvent>
#include <QtGui/QGuiApplication>
#include <QtGui/QHoverEvent>
#include <QtGui/QMouseEvent>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vthost/testing/TestHelpers.hpp>

using namespace contour::test::sessionSupport;

namespace
{

/// Output with a finished command block above and below thirty numbered lines and one that reads `needle`:
/// eleven lines of history on a 24-row page, to scroll back, to search, and to select a block's output in.
[[nodiscard]] std::string repliesTestOutput()
{
    auto output = std::string { "\033]133;A\033\\$ first\r\n\033]133;C\033\\" };
    for (auto const line: std::views::iota(0, 30))
        output += std::format("line {}\r\n", line);
    output +=
        "needle\r\n\033]133;D;0\033\\\033]133;A\033\\$ last\r\n\033]133;C\033\\out\r\n\033]133;D;0\033\\";
    return output;
}

/// Presses the left button on row 20 and drags it along that row, leaving it held: a selection in progress,
/// which a scroll of the view extends. Low enough on the page that, wherever the view scrolls, the pointer
/// stays below the history, which a selection report does not cover.
/// @param session The session to drag in.
void dragOnRowTwenty(contour::session::TerminalSession& session)
{
    session.sendMousePressEvent(
        Modifiers {},
        vtbackend::MouseButton::Left,
        vtbackend::CellLocation { .line = vtbackend::LineOffset(20), .column = vtbackend::ColumnOffset(0) },
        vtbackend::PixelCoordinate {});
    session.sendMouseMoveEvent(
        Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(20), .column = vtbackend::ColumnOffset(10) },
        vtbackend::PixelCoordinate {});
}

/// Scrolls @p session's view to the top of the history, so that it can scroll down again.
/// @param session The session.
void scrollToTheTop(contour::session::TerminalSession& session)
{
    auto& terminal = session.terminal();
    auto const _ = std::scoped_lock { terminal };
    (void) terminal.viewport().scrollToTop();
}

/// Enters Visual mode, where a search moving the vi cursor extends the selection.
/// @param session The session.
void enterVisualMode(contour::session::TerminalSession& session)
{
    REQUIRE(session(contour::actions::ViNormalMode {}));
    typeKeys(session, "v");
    REQUIRE(session.terminal().inputHandler().mode() == vtbackend::ViMode::Visual);
}

/// Sends what @p session queued for the PTY and forgets it, so that what follows is all a check reads.
/// @param session The session.
void discardWrittenInput(contour::session::TerminalSession& session)
{
    session.terminal().flushInput();
    mockPtyOf(session).stdinBuffer().clear();
}

/// The TestApp a motion test runs in: one whose accessibility preferences are read from @p accessibility.
/// @param accessibility The switch the app's accessibility source reads; it must outlive the app.
/// @param appearance The switch the app's appearance source reads; it must outlive the app.
/// @return The app.
[[nodiscard]] std::unique_ptr<contour::test::TestApp> makeMotionTestApp(
    contour::test::AccessibilitySwitch& accessibility, contour::test::AppearanceSwitch& appearance)
{
    return std::make_unique<contour::test::TestApp>(
        std::make_unique<contour::test::MockPtySessionFactory>(),
        nullptr,
        nullptr,
        nullptr,
        contour::test::TestApp::testHostIdentity(),
        nullptr,
        contour::test::TestEnvironment::Overrides {},
        nullptr,
        contour::test::appearanceServicesWith(accessibility, appearance));
}

/// A session in a window of its own, with a recording surface: what an explicit copy or a refused paste
/// tells its window about. The surface is detached before the window goes.
struct WindowedSession
{
    contour::test::ScopedController const& controller;
    contour::session::TerminalSession& session;
    contour::test::FakeDisplaySurface surface {};

    explicit WindowedSession(contour::test::ScopedController const& c):
        controller { c }, session { *(c->createNewTab(), c->activeSession()) }
    {
        surface.attachedSession = &session;
        session.attachDisplay(surface);
    }
    ~WindowedSession()
    {
        if (session.display() == &surface)
            session.detachDisplay(surface);
    }
    WindowedSession(WindowedSession const&) = delete;
    WindowedSession& operator=(WindowedSession const&) = delete;
    WindowedSession(WindowedSession&&) = delete;
    WindowedSession& operator=(WindowedSession&&) = delete;
};

/// Registers a profile that shows the scrollbar on the right under @p mode, drawing every kind of mark.
/// @param mutate Applied to the scrollbar section last, for a case that needs more than the mode.
std::string registerScrollbarModeProfile(
    contour::ContourGuiApp& app,
    std::string const& name,
    contour::config::ScrollBarModePreference mode,
    std::function<void(contour::config::ScrollBarConfig&)> const& mutate = [](auto&) {})
{
    return registerProfile(app, name, [mode, &mutate](contour::config::TerminalProfile& p) {
        auto scrollbar = p.scrollbar.value();
        scrollbar.position = contour::config::ScrollBarPosition::Right;
        scrollbar.mode = mode;
        scrollbar.marks = vtbackend::AllScrollbarMarkSources;
        mutate(scrollbar);
        p.scrollbar = scrollbar;
    });
}

/// Publishes what the scrollbar is decided from -- the history, the marks -- as the frame timer would.
void publishScrollbarInputs(contour::session::TerminalSession& session)
{
    session.refreshScrollbarMarks();
    QCoreApplication::processEvents();
}

/// The time a session waits before its first size indicator, plus a moment.
constexpr auto PastSizeIndicatorQuiet =
    std::chrono::duration_cast<std::chrono::milliseconds>(contour::session::SizeIndicatorStartupQuiet)
    + std::chrono::milliseconds { 1 };

/// What the pill says for @p session's main page.
[[nodiscard]] QString pageTextOf(contour::session::TerminalSession const& session)
{
    return QString::fromStdString(
        std::format("{} × {}", session.pageColumnsCount(), session.pageLineCount()));
}

/// A total page of @p columns by @p lines, status line included.
[[nodiscard]] constexpr vtbackend::PageSize totalPageOf(int columns, int lines)
{
    return vtbackend::PageSize { .lines = vtbackend::LineCount(lines),
                                 .columns = vtbackend::ColumnCount(columns) };
}

/// The first cell of an OSC 8 link written at the page's top left.
constexpr auto OnFirstLink =
    vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) };

/// Writes an OSC 8 link to https://example.com/page at the cursor.
void writePageLink(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033]8;;https://example.com/page\033\\link\033]8;;\033\\");
}

/// Presses (or releases) Ctrl, the Classic follow modifier, as a key event alone.
void sendControl(contour::session::TerminalSession& session, vtbackend::KeyboardEventType type)
{
    auto const held = type == vtbackend::KeyboardEventType::Release
                          ? vtbackend::KeyboardModifiers { vtbackend::Modifier::Control }
                          : vtbackend::KeyboardModifiers {};
    session.sendKeyEvent(vtbackend::Key::LeftControl, held, type, std::chrono::steady_clock::now());
}

/// The pill's text for https://example.com/page.
[[nodiscard]] QString pageBreadcrumb()
{
    return QStringLiteral("example.com › page");
}

/// The ints of the list @p map holds under @p key.
[[nodiscard]] std::vector<int> intsAt(QVariantMap const& map, QString const& key)
{
    return std::ranges::to<std::vector>(map.value(key).toList()
                                        | std::views::transform([](QVariant const& v) { return v.toInt(); }));
}

/// A failed `make` and then a succeeded `ls`, with a live prompt after them, in a displayless session whose
/// announcements @p recorder takes.
struct BlockFocusSession
{
    explicit BlockFocusSession(contour::ContourGuiApp& app): session { makeDisplaylessSession(app) }
    {
        auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
        recorder = announcer.get();
        session->setAnnouncer(std::move(announcer));
        auto& terminal = session->terminal();
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(2), 2));
        terminal.writeToScreen(vtbackend::test::commandRun("ls", numberedOutput(2), 0));
        terminal.writeToScreen(std::string { vtbackend::test::PromptStart });
    }

    /// Presses @p key with @p modifiers.
    void press(vtbackend::Key key, vtbackend::KeyboardModifiers modifiers = {}) const
    {
        session->sendKeyEvent(key, modifiers, KeyboardEventType::Press, std::chrono::steady_clock::now());
    }

    /// The block now focused.
    [[nodiscard]] std::optional<vtbackend::CommandBlockId> focused() const
    {
        return core::locked(session->terminal(), [&] { return session->terminal().focusedCommandBlock(); });
    }

    std::unique_ptr<contour::session::TerminalSession> session;
    contour::test::RecordingAnnouncer* recorder = nullptr;
};

/// A session over a MockPty whose foreground process is @p foreground, its application having enabled
/// bracketed paste when @p bracketed.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeDropSession(TestApp& testApp,
                                                                                 std::string_view foreground,
                                                                                 bool bracketed)
{
    auto session = makeDisplaylessSession(testApp.app());
    mockPtyOf(*session).setForegroundProcessName(std::string { foreground });
    if (bracketed)
        session->terminal().writeToScreen("\033[?2004h");
    mockPtyOf(*session).stdinBuffer().clear();
    return session;
}

} // namespace

TEST_CASE("TerminalSession: a colour preference change reported with local echo on does not deadlock",
          "[contour][session]")
{
    // With palette updates reported (DECSET 2031), re-seeding the palette writes a report under the terminal
    // lock the change holds, and with SRM reset the report is echoed, which parses and takes that lock again:
    // the report goes out once the lock is released.
    TestApp testApp;
    auto const name = registerProfile(testApp.app(), "dual", [](contour::config::TerminalProfile& p) {
        auto dual = contour::config::DualColorConfig {};
        dual.darkMode.defaultBackground = vtbackend::RGBColor { 0x101010 };
        dual.lightMode.defaultBackground = vtbackend::RGBColor { 0xF0F0F0 };
        p.colors.value() = dual;
    });
    auto session = makeSessionWithProfile(testApp.app(), name);
    session->terminal().writeToScreen("\033[?2031h");
    echoLocally(*session);
    auto const other = testApp.app().colorPreference() == vtbackend::ColorPreference::Light
                           ? vtbackend::ColorPreference::Dark
                           : vtbackend::ColorPreference::Light;

    failOnDeadlock(std::chrono::seconds(10), [&] { session->updateColorPreference(other); });

    CHECK(mockPtyOf(*session).stdinBuffer().starts_with("\033[?997;"));
}

TEST_CASE("TerminalSession: a buffer capture answered with local echo on does not deadlock",
          "[contour][session][permission]")
{
    // The capture is written under the terminal lock and flushed, and with SRM reset the flush echoes it,
    // which parses and takes that lock again: the capture goes out once the lock is released.
    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), contour::config::Permission::Allow);
    echoLocally(*held);

    failOnDeadlock(std::chrono::seconds(10), [&] { captureWholePage(*held); });

    CHECK(capturedSeededScreen(*held));
}

TEST_CASE("TerminalSession: the host-writable status line shown with resize reports echoed locally does not "
          "deadlock",
          "[contour][session][permission]")
{
    // Putting the line up shrinks the page, under the terminal lock, and the resize reports the page.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] {
        session->executeShowHostWritableStatusLine(PermissionDecision::Allow, PermissionMemory::Once);
    });

    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: a mouse press reported with local echo on does not deadlock",
          "[contour][session][input]")
{
    // With mouse tracking on (DECSET 1000) the press is reported under the terminal lock the press takes, and
    // with SRM reset the report is echoed, which parses and takes that lock again.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033[?1000h");
    echoLocally(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] {
        session->sendMousePressEvent(Modifiers {},
                                     vtbackend::MouseButton::Left,
                                     vtbackend::CellLocation {},
                                     vtbackend::PixelCoordinate {});
    });

    CHECK(mockPtyOf(*session).stdinBuffer().starts_with("\033[M"));
}

TEST_CASE("TerminalSession: a mouse move reported with local echo on does not deadlock",
          "[contour][session][input]")
{
    // With every motion tracked (DECSET 1003) a move to another cell is reported, as a press is.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033[?1003h");
    echoLocally(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] {
        session->sendMouseMoveEvent(Modifiers {},
                                    vtbackend::CellLocation { .line = vtbackend::LineOffset(1),
                                                              .column = vtbackend::ColumnOffset(1) },
                                    vtbackend::PixelCoordinate {});
    });

    CHECK(mockPtyOf(*session).stdinBuffer().starts_with("\033[M"));
}

TEST_CASE("TerminalSession: a mouse release reported with local echo on does not deadlock",
          "[contour][session][input]")
{
    // With mouse tracking on the release is reported, as the press before it was.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033[?1000h");
    session->sendMousePressEvent(Modifiers {},
                                 vtbackend::MouseButton::Left,
                                 vtbackend::CellLocation {},
                                 vtbackend::PixelCoordinate {});
    echoLocally(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] {
        session->sendMouseReleaseEvent(
            Modifiers {}, vtbackend::MouseButton::Left, vtbackend::PixelCoordinate {});
    });

    CHECK(mockPtyOf(*session).stdinBuffer().starts_with("\033[M"));
}

TEST_CASE("TerminalSession: cancelling a dragged selection with resize reports echoed locally does not "
          "deadlock",
          "[contour][session][vi]")
{
    // A selection dragged in Normal mode is a Visual one, and cancelling it returns to Normal mode, which
    // puts the indicator status line up: the resize reports the page.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE((*session)(contour::actions::ViNormalMode {}));
    session->sendMousePressEvent(Modifiers {},
                                 vtbackend::MouseButton::Left,
                                 vtbackend::CellLocation {},
                                 vtbackend::PixelCoordinate {});
    session->sendMouseMoveEvent(
        Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(5) },
        vtbackend::PixelCoordinate {});
    REQUIRE(session->terminal().inputHandler().mode() == vtbackend::ViMode::Visual);
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::CancelSelection {})); });

    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Normal);
    CHECK(reportedItsPage(*session));
}

TEST_CASE(
    "TerminalSession: a hard reset from 132 columns with resize reports echoed locally does not deadlock",
    "[contour][session]")
{
    // A hard reset returns a 132-column page to 80 columns (DECCOLM) before it clears the modes, so the
    // resize reports the page while local echo is still on.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("\033[?40h\033[?3h");
    REQUIRE(session->terminal().pageSize().columns == vtbackend::ColumnCount(132));
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::ClearHistoryAndReset {})); });

    CHECK(session->terminal().pageSize().columns == vtbackend::ColumnCount(80));
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: a soft reset unlocking the keyboard with resize reports echoed locally does not "
          "deadlock",
          "[contour][session]")
{
    // Locking the keyboard (KAM) puts the indicator status line up, and a soft reset unlocks it before it
    // turns local echo off, taking the line down again: the resize reports the page.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    session->terminal().writeToScreen("\033[2h");
    REQUIRE(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] { CHECK((*session)(contour::actions::SoftReset {})); });

    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::None);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: hint mode with resize reports echoed locally does not deadlock",
          "[contour][session][hintmode]")
{
    // Hint mode puts the indicator status line up, as Normal mode does.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] { CHECK((*session)(contour::actions::HintMode {})); });

    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Hint);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: a selection pasted into the shell with local echo on does not deadlock",
          "[contour][session][clipboard]")
{
    // Evaluated in the shell, the paste ends in a newline, which is sent and flushed whatever the selection
    // clipboard holds, and with SRM reset it is echoed.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    echoLocally(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::PasteSelection { .evaluateInShell = true })); });

    CHECK(mockPtyOf(*session).stdinBuffer().ends_with("\n"));
}

TEST_CASE("TerminalSession: characters sent with local echo on do not deadlock", "[contour][session][input]")
{
    // What the action sends is flushed, and with SRM reset it is echoed.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    echoLocally(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::SendChars { .chars = "x" })); });

    CHECK(mockPtyOf(*session).stdinBuffer() == "x");
}

TEST_CASE("TerminalSession: key bindings toggled off with resize reports echoed locally do not deadlock",
          "[contour][session][input]")
{
    // Turning the bindings off puts up an indicator status line saying so.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::ToggleAllKeyMaps {})); });

    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: input protection toggled on with resize reports echoed locally does not deadlock",
          "[contour][session][input]")
{
    // Protecting the input locks the keyboard with the user's lock, which puts the indicator status line up.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::ToggleInputProtection {})); });

    CHECK_FALSE(session->terminal().allowInput());
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: toggling the status line selects its type and leaves a host-writable one",
          "[contour][session]")
{
    // DEC STD 070 ch. 14 §3 DECSSDT: "The type of Status Line can also be changed through a Set-Up field",
    // and the user's toggle is that field; §2.3: "If the Status Display Type is chosen to be something other
    // than "Host Writable", the Status Display will be exited". So the toggle that hides the host-writable
    // status line leaves it, and DECSASD can no longer select it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen("\033[2$~"); // DECSSDT: the host-writable status line
    session->executeShowHostWritableStatusLine(PermissionDecision::Allow, PermissionMemory::Once);
    terminal.writeToScreen("\033[1$}"); // DECSASD: write to it
    REQUIRE(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);

    CHECK((*session)(contour::actions::ToggleStatusLine {})); // the indicator instead
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);

    terminal.writeToScreen("\033[1$}X"); // DECSASD again, and text
    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
    CHECK(terminal.primaryScreen().renderMainPageText().contains('X'));
    CHECK_FALSE(terminal.hostWritableStatusLineDisplay().renderMainPageText().contains('X'));
}

TEST_CASE("TerminalSession: the status line toggled with resize reports echoed locally does not deadlock",
          "[contour][session]")
{
    // Taking the indicator status line down grows the page.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::ToggleStatusLine {})); });

    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::None);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: a profile change with resize reports echoed locally does not deadlock",
          "[contour][session][profile]")
{
    // Configuring the terminal for the new profile applies its status line, which resizes the page.
    TestApp testApp;
    auto const bare = registerProfile(testApp.app(), "bare", [](contour::config::TerminalProfile& p) {
        p.statusLine.value().initialType = vtbackend::StatusDisplayType::None;
    });
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { CHECK((*session)(contour::actions::ChangeProfile { .name = bare })); });

    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::None);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: a key is looked up and sent while another thread holds the terminal lock",
          "[contour][session][input][threading]")
{
    // The parser thread holds the terminal lock for a whole batch, and Windows' lock is not fair: a key that
    // waited for it -- Ctrl+C above all, in a flood of output -- could wait for minutes. Neither the binding
    // lookup nor the send takes it. Another thread holds the lock for 500 ms; the key is sent within 100 ms.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();

    auto held = std::latch { 1 };
    auto holder = std::jthread { [&]() {
        auto const _ = std::scoped_lock { terminal };
        held.count_down();
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    } };
    held.wait();

    auto const started = std::chrono::steady_clock::now();
    session->sendCharEvent(U'c',
                           vtbackend::KeyIdentity { .unshiftedKey = U'c' },
                           Modifiers { vtbackend::Modifier::Control },
                           KeyboardEventType::Press,
                           started);
    auto const took = std::chrono::steady_clock::now() - started;
    holder.join();

    CHECK(took < std::chrono::milliseconds(100));
    CHECK(mockPtyOf(*session).stdinBuffer() == "\x03");
}

TEST_CASE("TerminalSession: a selection report raised on the GUI thread reaches the PTY at once",
          "[contour][session][selection]")
{
    // With selection reports on (DECSET 2030), every change to the selection writes one. A change the parser
    // makes goes out with its batch; one made on the GUI thread has no batch, and waited in the queue until
    // some later key or output flushed it -- never, for an application blocked on reading the report. Each
    // is sent once the lock it was raised under is released. Every row is one place on the GUI thread that
    // changes the selection.
    namespace actions = contour::actions;
    using contour::session::TerminalSession;
    struct Row
    {
        std::string_view site;
        std::function<void(TerminalSession&)> prepare;
        std::function<void(TerminalSession&)> act;
    };
    auto const none = [](TerminalSession&) {
    };
    auto const rows = std::array {
        Row { .site = "SelectAll", .prepare = none, .act = [](auto& s) { (void) s(actions::SelectAll {}); } },
        Row { .site = "SelectCommandBlock",
              .prepare = none,
              .act = [](auto& s) { (void) s(actions::SelectCommandBlock {}); } },
        Row { .site = "CreateSelection",
              .prepare =
                  [](auto& s) {
                      s.sendMouseMoveEvent(Modifiers {},
                                           vtbackend::CellLocation { .line = vtbackend::LineOffset(2),
                                                                     .column = vtbackend::ColumnOffset(1) },
                                           vtbackend::PixelCoordinate {});
                  },
              .act = [](auto& s) { (void) s(actions::CreateSelection { .delimiters = " " }); } },
        Row { .site = "performAutoScroll",
              .prepare =
                  [](auto& s) {
                      {
                          auto const _ = std::scoped_lock { s.terminal() };
                          (void) s.terminal().viewport().scrollUp(vtbackend::LineCount(1));
                      }
                      dragOnRowTwenty(s);
                  },
              .act = [](auto& s) { s.performAutoScroll(1, vtbackend::LineCount(1)); } },
        Row { .site = "FocusNextSearchMatch",
              .prepare =
                  [](auto& s) {
                      enterVisualMode(s);
                      s.setSearchPattern(QStringLiteral("line"));
                      (void) s(actions::FocusPreviousSearchMatch {});
                      (void) s(actions::FocusPreviousSearchMatch {});
                  },
              .act = [](auto& s) { (void) s(actions::FocusNextSearchMatch {}); } },
        Row { .site = "FocusPreviousSearchMatch",
              .prepare =
                  [](auto& s) {
                      enterVisualMode(s);
                      s.setSearchPattern(QStringLiteral("line"));
                  },
              .act = [](auto& s) { (void) s(actions::FocusPreviousSearchMatch {}); } },
        Row { .site = "setSearchPattern",
              .prepare = enterVisualMode,
              .act = [](auto& s) { s.setSearchPattern(QStringLiteral("line 29")); } },
        Row { .site = "toggleSearchOption(Case)",
              .prepare =
                  [](auto& s) {
                      enterVisualMode(s);
                      s.setSearchPattern(QStringLiteral("line"));
                  },
              .act =
                  [](auto& s) {
                      s.toggleSearchOption(static_cast<int>(contour::session::SearchToggle::Case));
                  } },
        Row { .site = "searchNext, wrapping round from the only match",
              .prepare =
                  [](auto& s) {
                      enterVisualMode(s);
                      s.setSearchPattern(QStringLiteral("needle"));
                  },
              .act = [](auto& s) { s.searchNext(); } },
        Row { .site = "ScrollUp",
              .prepare = dragOnRowTwenty,
              .act = [](auto& s) { (void) s(actions::ScrollUp {}); } },
        Row { .site = "ScrollDown",
              .prepare =
                  [](auto& s) {
                      scrollToTheTop(s);
                      dragOnRowTwenty(s);
                  },
              .act = [](auto& s) { (void) s(actions::ScrollDown {}); } },
        Row { .site = "ScrollMarkUp",
              .prepare = dragOnRowTwenty,
              .act = [](auto& s) { (void) s(actions::ScrollMarkUp {}); } },
        Row { .site = "ScrollMarkDown",
              .prepare =
                  [](auto& s) {
                      scrollToTheTop(s);
                      dragOnRowTwenty(s);
                  },
              .act = [](auto& s) { (void) s(actions::ScrollMarkDown {}); } },
        Row { .site = "ScrollToTop",
              .prepare = dragOnRowTwenty,
              .act = [](auto& s) { (void) s(actions::ScrollToTop {}); } },
        Row { .site = "ScrollToBottom",
              .prepare =
                  [](auto& s) {
                      scrollToTheTop(s);
                      dragOnRowTwenty(s);
                  },
              .act = [](auto& s) { (void) s(actions::ScrollToBottom {}); } },
    };

    TestApp testApp;
    for (auto const& row: rows)
    {
        INFO(row.site);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().writeToScreen(repliesTestOutput());
        session->terminal().writeToScreen("\033[?2030h");
        row.prepare(*session);
        discardWrittenInput(*session);

        row.act(*session);

        // What is still queued shows on a failure: the report raised, and not sent.
        INFO("queued: " << core::escape(session->terminal().peekInput()));
        CHECK(mockPtyOf(*session).stdinBuffer().contains("\033[>"));
    }
}

TEST_CASE("TerminalSession: a selection report the wheel raises reaches the PTY at once",
          "[contour][session][selection][input]")
{
    // A touchpad's scroll moves the view at once, which extends a selection being dragged, as the actions
    // above do. Posted work is held back, as a posted flush would send the report whether or not the wheel
    // did.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    pane.surface->runPostsImmediately = false;
    pane->terminal().writeToScreen(repliesTestOutput());
    pane->terminal().writeToScreen("\033[?2030h");
    dragOnRowTwenty(*pane);
    discardWrittenInput(*pane);

    auto wheel = QWheelEvent { QPointF(10, 10), QPointF(10, 10), QPoint(0, 60),    QPoint(0, 0),
                               Qt::LeftButton,  Qt::NoModifier,  Qt::ScrollUpdate, false };
    contour::session::sendWheelEvent(&wheel, *pane);

    INFO("queued: " << core::escape(pane->terminal().peekInput()));
    CHECK(mockPtyOf(*pane).stdinBuffer().contains("\033[>"));
}

TEST_CASE("TerminalSession: a horizontal wheel notch is reported at the cell it turns over",
          "[contour][session][input]")
{
    // A notch is not a press at a new spot, but it happens where the pointer is, and its event says where
    // that is. Remembered positions are not the pointer's: before any pointer event the terminal's is only
    // its default, the top-left cell, and once the pointer has left the grid it is the cell it left from.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    pane->terminal().writeToScreen("\033[?1000h\033[?1006h"); // button events, SGR encoding
    auto const& cellSize = pane.surface->cellSizeValue;
    auto const pixelCentreOf = [&](vtbackend::CellLocation cell) {
        return QPointF((unbox<double>(cell.column) + 0.5) * unbox<double>(cellSize.width),
                       (unbox<double>(cell.line) + 0.5) * unbox<double>(cellSize.height));
    };
    auto const underTheWheel =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(3) };

    SECTION("before any pointer event")
    {
    }

    SECTION("once the pointer has left the grid")
    {
        pane->terminal().sendMouseMoveEvent(Modifiers {},
                                            vtbackend::CellLocation { .line = vtbackend::LineOffset(5),
                                                                      .column = vtbackend::ColumnOffset(7) },
                                            vtbackend::PixelCoordinate {},
                                            false);
        pane->terminal().sendMouseLeaveEvent();
    }

    SECTION("with the pointer moved onto that cell")
    {
        pane->terminal().sendMouseMoveEvent(
            Modifiers {}, underTheWheel, vtbackend::PixelCoordinate {}, false);
    }

    discardWrittenInput(*pane);
    auto const position = pixelCentreOf(underTheWheel);
    auto wheel = QWheelEvent { position,     position,       QPoint(0, 0),      QPoint(120, 0),
                               Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false };
    contour::session::sendWheelEvent(&wheel, *pane);

    auto const& reports = mockPtyOf(*pane).stdinBuffer();
    INFO("reports: " << core::escape(reports));
    CHECK(reports.contains(";4;3M"));
    CHECK_FALSE(reports.contains(";1;1M"));
    CHECK_FALSE(reports.contains(";8;6M"));
    CHECK(pane->terminal().currentMouseGridPosition() == underTheWheel);
}

TEST_CASE("TerminalSession: a desktop notification's answer reaches the PTY at once",
          "[contour][session][notification]")
{
    // The desktop reports a notification closed or clicked on the GUI thread, and the session answers it over
    // the PTY (OSC 99). The answer used to wait in the queue for some later flush.
    struct Row
    {
        std::string_view event;
        std::string_view answer;
        std::function<void(RecordingNotifier&)> fire;
    };
    auto const rows = std::array {
        Row { .event = "close",
              .answer = "\033]99;i=n:p=close;",
              .fire = [](RecordingNotifier& notifier) { notifier.fireClosed("n", 2); } },
        Row { .event = "activation",
              .answer = "\033]99;i=n:p=activated;",
              .fire = [](RecordingNotifier& notifier) { notifier.fireActivated("n"); } },
    };

    TestApp testApp;
    for (auto const& row: rows)
    {
        INFO(row.event);
        auto notifier = std::make_unique<RecordingNotifier>();
        auto* const raised = notifier.get();
        auto session = makeDisplaylessSession(testApp.app(), std::move(notifier));

        auto notification = vtbackend::DesktopNotification {};
        notification.identifier = "n";
        notification.title = "Job";
        notification.closeEventRequested = true;
        notification.reportOnActivation = true;
        session->showDesktopNotification(notification);
        QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);
        REQUIRE(raised->raised.size() == 1);
        discardWrittenInput(*session);

        row.fire(*raised);

        INFO("queued: " << core::escape(session->terminal().peekInput()));
        CHECK(mockPtyOf(*session).stdinBuffer().contains(row.answer));
    }
}

TEST_CASE("TerminalSession: output brings a scrolled-back view down without racing the GUI thread",
          "[contour][session][view][threading]")
{
    // Output brings a view scrolled back into the history down to the bottom, as auto_scroll_on_update asks.
    // The GUI thread moves the view as well, under the terminal lock: here it scrolls it back a line at a
    // time while another thread writes output. The case to run under ThreadSanitizer, which reports the
    // output's move of the view against the GUI thread's unless the output moves it under that lock too.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    auto& terminal = pane->terminal();
    terminal.settings().autoScrollOnUpdate = true; // off by default
    for (auto const line: std::views::iota(0, 40))
        terminal.writeToScreen(std::format("line {}\r\n", line));

    auto constexpr ScrollCount = 200;
    {
        auto const writer = vtbackend::test::writeOnOwnThread([&] { terminal.writeToScreen("x\r\n"); });
        for ([[maybe_unused]] auto const scroll: std::views::iota(0, ScrollCount))
            (*pane)(contour::actions::ScrollOneUp {});
    }

    // And it still does: the next output brings the view down.
    (*pane)(contour::actions::ScrollOneUp {});
    REQUIRE(terminal.viewport().scrolled());
    terminal.writeToScreen("y\r\n");
    CHECK_FALSE(terminal.viewport().scrolled());
}

TEST_CASE("TerminalSession: the pointer leaves the terminal's clock to the render thread",
          "[contour][session][input][threading]")
{
    // The render thread ticks the terminal's clock before and after each frame, and asks it when the next one
    // is due. The mouse handlers ticked it too, on the GUI thread. The case to run under ThreadSanitizer,
    // which reports the two threads' ticks against each other.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto const pixels = vtbackend::PixelCoordinate {};

    auto constexpr ClickCount = 100;
    {
        auto const renderThread = vtbackend::test::writeOnOwnThread([&] {
            terminal.tick(std::chrono::steady_clock::now());
            (void) terminal.nextRender();
        });
        for (auto const click: std::views::iota(0, ClickCount))
        {
            auto const at = vtbackend::CellLocation { .line = vtbackend::LineOffset(click % 10),
                                                      .column = vtbackend::ColumnOffset(click % 20) };
            session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Left, at, pixels);
            session->sendMouseMoveEvent(Modifiers {}, at, pixels);
            session->sendMouseReleaseEvent(Modifiers {}, vtbackend::MouseButton::Left, pixels);
        }
    }

    // The pointer reads nothing off the terminal's clock, so it does not set it.
    auto const ticked = terminal.currentTime();
    session->sendMouseMoveEvent(Modifiers {}, vtbackend::CellLocation {}, pixels);
    CHECK(terminal.currentTime() == ticked);

    CHECK(terminal.currentMousePosition() == vtbackend::CellLocation {});
}

TEST_CASE("TerminalSession: the scrollbar moves the view without racing the parser thread",
          "[contour][session][view][threading]")
{
    // The QML scrollbar writes scrollOffset on the GUI thread. The parser thread moves the view under the
    // terminal lock: output keeps a scrolled-back view on the lines it shows, or brings it down to the
    // bottom. The case to run under ThreadSanitizer, which reports the scrollbar's move against the parser's
    // unless the scrollbar takes that lock as well.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    auto& terminal = pane->terminal();
    for (auto const line: std::views::iota(0, 40))
        terminal.writeToScreen(std::format("line {}\r\n", line));

    auto constexpr MoveCount = 200;
    {
        auto const writer = vtbackend::test::writeOnOwnThread([&] { terminal.writeToScreen("x\r\n"); });
        for (auto const move: std::views::iota(0, MoveCount))
            pane->setScrollOffset(1 + (move % 10));
    }

    pane->setScrollOffset(5);
    CHECK(pane->scrollOffset() == 5);
}

TEST_CASE("TerminalSession: the scrollbar answering a scroll made under the terminal lock does not deadlock",
          "[contour][session][view][deadlock]")
{
    // While hovered, the QML scrollbar writes back at once the offset a scroll announces: scrollOffsetChanged
    // moves the bar, and the bar's move writes scrollOffset. A Vi motion and the scroll actions announce it
    // on the GUI thread with the terminal lock held, so the write must not take that lock again. The
    // connection stands in for the bar.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    for (auto const line: std::views::iota(0, 60))
        terminal.writeToScreen(std::format("line {}\r\n", line));

    auto writtenBack = 0;
    QObject::connect(
        session.get(),
        &contour::session::TerminalSession::scrollOffsetChanged,
        session.get(),
        [&](int offset) {
            session->setScrollOffset(offset);
            ++writtenBack;
        },
        Qt::DirectConnection);

    failOnDeadlock(std::chrono::seconds(10), [&] {
        (*session)(contour::actions::ScrollUp {});
        (*session)(contour::actions::ViNormalMode {});
        typeKeys(*session, std::string(40, 'k'));
    });

    CHECK(writtenBack > 1);
    CHECK(terminal.viewport().scrolled());
}

TEST_CASE("TerminalSession: the page's size is read without racing the parser thread",
          "[contour][session][threading]")
{
    // The GUI thread reads the main page's size without the terminal lock: the QML asks for its lines and
    // columns, a pointer's move is bounded by it, and a page scroll steps half of it. The parser thread
    // resizes the page under that lock as it puts the status line up and takes it down (DECSSDT), and the
    // PTY's page size with it. The case to run under ThreadSanitizer, which reports each read against the
    // resize unless it is race-free.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    for (auto const line: std::views::iota(0, 40))
        terminal.writeToScreen(std::format("line {}\r\n", line));

    auto strayPages = size_t { 0 };
    auto const reads = vtbackend::test::runWhileWriting(
        [&] { terminal.writeToScreen("\033[1$~\033[0$~"); },
        [&] {
            if (session->pageLineCount() < 1 || session->pageColumnsCount() < 1)
                ++strayPages;
            // Off the page, so that the move ends at the bound it is checked against.
            session->sendMouseMoveEvent(Modifiers {},
                                        vtbackend::CellLocation { .line = vtbackend::LineOffset(1000),
                                                                  .column = vtbackend::ColumnOffset(0) },
                                        vtbackend::PixelCoordinate {});
            (void) (*session)(contour::actions::ScrollPageUp {});
            (void) (*session)(contour::actions::ScrollPageDown {});
        });

    CHECK(strayPages == 0);
    CHECK(reads > 0);
}

TEST_CASE("TerminalSession: input into a program that reads nothing leaves the GUI thread free",
          "[contour][session][threading]")
{
    // A key or a paste is written to the PTY, and a PTY write parks once the program leaves its input unread.
    // Written on the GUI thread -- or queued behind a write the session's reader thread had parked -- it
    // froze the window until the program read. The GUI thread queues it and wakes the session's reader, which
    // writes it, in the order it was typed.
    TestApp testApp;
    auto ownedPty = std::make_unique<vthost::testing::StallingPty>(TestPageSize);
    auto* const pty = ownedPty.get();
    auto session = makeSessionWith(testApp.app(), std::move(ownedPty));
    session->start();
    pty->stall();

    // Drained once the input is handed over, or after five seconds of it: a GUI thread parked on a write then
    // ends the case rather than the run.
    auto handedOver = std::promise<void> {};
    auto returned = std::future_status::timeout;
    auto watchdog = std::jthread { [&returned, pty, done = handedOver.get_future()] {
        returned = done.wait_for(std::chrono::seconds { 5 });
        pty->drain();
    } };
    auto const type = [&session](char32_t ch) {
        session->sendCharEvent(ch,
                               vtbackend::KeyIdentity { .unshiftedKey = ch },
                               Modifiers {},
                               KeyboardEventType::Press,
                               std::chrono::steady_clock::now());
    };
    type(U'a');
    session->terminal().sendPaste("bc");
    type(U'd');
    handedOver.set_value();
    watchdog.join();

    CHECK(returned == std::future_status::ready);
    for ([[maybe_unused]] auto const tick: std::views::iota(0, 500))
    {
        if (pty->written() == "abcd")
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    CHECK(pty->written() == "abcd");
}

TEST_CASE("TerminalSession: a mirrored pane follows its host connection, a local one ignores it",
          "[contour][session][reconnect]")
{
    using vthost::client::ConnectionState;
    TestApp testApp;
    auto const remote =
        std::make_unique<contour::session::TerminalSession>(&testApp.app().sessionsManager(),
                                                            std::make_unique<vtpty::MockPty>(TestPageSize),
                                                            testApp.app(),
                                                            std::string {},
                                                            std::nullopt,
                                                            std::nullopt,
                                                            contour::test::makeTestNotifier(),
                                                            contour::session::SessionRole::Shell,
                                                            contour::session::SessionHosting::Remote);
    auto changes = 0;
    QObject::connect(
        remote.get(), &contour::session::TerminalSession::reconnectingChanged, [&changes] { ++changes; });
    CHECK_FALSE(remote->isReconnecting()); // born connected

    remote->setHostConnection(ConnectionState::Reconnecting);
    CHECK(remote->isReconnecting());
    CHECK(changes == 1);
    remote->setHostConnection(ConnectionState::Reconnecting); // no change, no signal
    CHECK(changes == 1);
    remote->setHostConnection(ConnectionState::Connected);
    CHECK_FALSE(remote->isReconnecting());
    CHECK(changes == 2);

    auto const local = makeDisplaylessSession(testApp.app());
    auto localChanges = 0;
    QObject::connect(local.get(), &contour::session::TerminalSession::reconnectingChanged, [&localChanges] {
        ++localChanges;
    });
    local->setHostConnection(ConnectionState::Reconnecting);
    CHECK_FALSE(local->isReconnecting());
    CHECK(localChanges == 0);
}

TEST_CASE("TerminalSession: paneState is window::paneStateOf over the end and the host connection",
          "[contour][session][reconnect][panestate]")
{
    using contour::window::PaneState;
    using vthost::client::ConnectionState;
    TestApp testApp;
    auto const remote =
        std::make_unique<contour::session::TerminalSession>(&testApp.app().sessionsManager(),
                                                            std::make_unique<vtpty::MockPty>(TestPageSize),
                                                            testApp.app(),
                                                            std::string {},
                                                            std::nullopt,
                                                            std::nullopt,
                                                            contour::test::makeTestNotifier(),
                                                            contour::session::SessionRole::Shell,
                                                            contour::session::SessionHosting::Remote);
    auto changes = QSignalSpy(remote.get(), &contour::session::TerminalSession::paneStateChanged);
    auto const stateOf = [&remote] {
        return static_cast<PaneState>(remote->property("paneState").toInt());
    };
    CHECK(stateOf() == PaneState::Live); // born connected

    remote->setHostConnection(ConnectionState::Reconnecting);
    CHECK(stateOf() == PaneState::Reconnecting);
    CHECK(changes.count() == 1);
    remote->setHostConnection(ConnectionState::Connecting); // the same pane state: no signal
    CHECK(changes.count() == 1);
    remote->setHostConnection(ConnectionState::Connected);
    CHECK(stateOf() == PaneState::Live);
    CHECK(changes.count() == 2);

    // A pane lost with the connection is ended -- until the connection is sought again from its footer, when
    // it shows the pill over its frozen grid; refused, its footer is back.
    remote->noteTransportLost();
    mockPtyOf(*remote).close();
    remote->onClosed();
    CHECK(stateOf() == PaneState::Ended);
    CHECK(changes.count() == 3);
    remote->setHostConnection(ConnectionState::Reconnecting);
    CHECK(stateOf() == PaneState::Reconnecting);
    CHECK(changes.count() == 4);
    remote->setHostConnection(ConnectionState::Refused);
    CHECK(stateOf() == PaneState::Ended);
    CHECK(changes.count() == 5);
}

TEST_CASE("TerminalSession: a remote pane reconnects exactly while its window's host chip says so",
          "[contour][session][reconnect]")
{
    using vthost::client::ConnectionState;
    TestApp testApp;
    auto const remote =
        std::make_unique<contour::session::TerminalSession>(&testApp.app().sessionsManager(),
                                                            std::make_unique<vtpty::MockPty>(TestPageSize),
                                                            testApp.app(),
                                                            std::string {},
                                                            std::nullopt,
                                                            std::nullopt,
                                                            contour::test::makeTestNotifier(),
                                                            contour::session::SessionRole::Shell,
                                                            contour::session::SessionHosting::Remote);
    // A first connection is a reconnect too, as the chip says: Connecting reads as reconnecting.
    remote->setHostConnection(ConnectionState::Connecting);
    CHECK(remote->isReconnecting());
    // The pane's pill and the chip fold the connection the same way, so the two never disagree.
    for (auto const index: std::views::iota(size_t { 0 }, vthost::client::ConnectionStateCount))
    {
        auto const state = static_cast<ConnectionState>(index);
        CAPTURE(index);
        remote->setHostConnection(state);
        CHECK(remote->isReconnecting()
              == (contour::window::hostChipStateOf(state) == contour::window::HostChipState::Reconnecting));
    }
}

TEST_CASE("TerminalSessionManager: the host connection reaches every remotely hosted session",
          "[contour][session][reconnect]")
{
    using vthost::client::ConnectionState;
    auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
    owned->reportedHosting = contour::session::SessionHosting::Remote;
    auto app = TestApp { std::move(owned) };
    auto const win = contour::test::ScopedController { app.manager() };
    auto* first = app.manager().createSession(win.id);
    auto* second = app.manager().createSession(win.id);
    REQUIRE(first != nullptr);
    REQUIRE(second != nullptr);

    app.manager().setHostConnection(ConnectionState::Reconnecting);
    CHECK(first->isReconnecting());
    CHECK(second->isReconnecting());
    app.manager().setHostConnection(ConnectionState::Connected);
    CHECK_FALSE(first->isReconnecting());
    CHECK_FALSE(second->isReconnecting());
}

TEST_CASE("ContourGuiApp: the chrome's inputs ignore a scroller-style flip", "[contour][chromehost]")
{
    // The scroller style is a session's (the overlay scrollbar's mode), not the chrome's: a mouse plugged in
    // must not rebuild every window's chromeStyle. What the app builds the chrome from is pinned, through the
    // same appearance source the OS reports to.
    using contour::config::ScrollBarMode;
    auto accessibility = contour::test::AccessibilitySwitch {};
    auto appearance = contour::test::AppearanceSwitch {};
    auto const testApp = makeMotionTestApp(accessibility, appearance);
    auto const before = testApp->app().chromeInputs();

    auto flipped = appearance.appearance;
    flipped.scrollBars = ScrollBarMode::Always;
    appearance.set(flipped);
    CHECK(testApp->app().chromeInputs() == before);

    // The path does read the appearance: an accent change reaches the chrome.
    flipped.accent = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    appearance.set(flipped);
    CHECK_FALSE(testApp->app().chromeInputs() == before);
}

TEST_CASE("TerminalSession: a session born under Reduce Motion has no glide and no transition",
          "[contour][session][motion]")
{
    using namespace std::chrono_literals;
    // Declared before the app, so the switches outlive the sources reading them.
    auto accessibility = contour::test::AccessibilitySwitch {};
    auto appearance = contour::test::AppearanceSwitch {};
    accessibility.preferences.motion = contour::config::MotionPreference::Reduced;
    auto const testApp = makeMotionTestApp(accessibility, appearance);

    auto const session = makeDisplaylessSession(testApp->app());
    CHECK(session->terminal().settings().screenTransitionDuration == 0ms);
    CHECK(session->terminal().settings().cursorMotionAnimationDuration == 0ms);
    // The configuration keeps what the user asked for.
    CHECK(session->profile().screenTransitionDuration.value() != 0ms);
}

TEST_CASE("TerminalSession: turning Reduce Motion on and off follows live", "[contour][session][motion]")
{
    using namespace std::chrono_literals;
    using contour::config::MotionPreference;
    auto accessibility = contour::test::AccessibilitySwitch {};
    auto appearance = contour::test::AppearanceSwitch {};
    auto const testApp = makeMotionTestApp(accessibility, appearance);

    // Through the manager, so updateMotionPreference() reaches it.
    contour::test::ScopedController const win { testApp->manager() };
    auto* session = testApp->manager().createSession(win.id);
    REQUIRE(session != nullptr);
    auto const& settings = session->terminal().settings();
    REQUIRE(settings.screenTransitionDuration == 250ms);
    REQUIRE(settings.cursorMotionAnimationDuration == 80ms);

    accessibility.set(contour::config::AccessibilityPreferences {
        .motion = MotionPreference::Reduced, .transparency = {}, .contrast = {} });
    CHECK(settings.screenTransitionDuration == 0ms);
    CHECK(settings.cursorMotionAnimationDuration == 0ms);
    // The factory settings follow, so a RIS keeps what the preference allows.
    CHECK(session->terminal().factorySettings().cursorMotionAnimationDuration == 0ms);

    accessibility.set(contour::config::AccessibilityPreferences {
        .motion = MotionPreference::Full, .transparency = {}, .contrast = {} });
    CHECK(settings.screenTransitionDuration == 250ms);
    CHECK(settings.cursorMotionAnimationDuration == 80ms);
    CHECK(session->terminal().factorySettings().cursorMotionAnimationDuration == 80ms);
}

TEST_CASE("TerminalSession: a configuration reload keeps Reduce Motion", "[contour][session][motion]")
{
    using namespace std::chrono_literals;
    auto accessibility = contour::test::AccessibilitySwitch {};
    auto appearance = contour::test::AppearanceSwitch {};
    accessibility.preferences.motion = contour::config::MotionPreference::Reduced;
    contour::test::ConfiguredTestApp testApp {
        nullptr, contour::test::appearanceServicesWith(accessibility, appearance)
    };

    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE(session->terminal().settings().screenTransitionDuration == 0ms);

    reloadSessionWith(*session, "", "        screen_transition_duration: 333\n");
    CHECK(session->terminal().settings().screenTransitionDuration == 0ms);
    CHECK(session->profile().screenTransitionDuration.value() == 333ms);
}

TEST_CASE("TerminalSession: an explicit copy tells its window how many lines it copied",
          "[contour][session][clipboard][notices]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    SECTION("a selection of two lines")
    {
        writeFinishedBlock(windowed.session, "make", "built\r\nlinked\r\n");
        REQUIRE(windowed.session(
            contour::actions::SelectCommandBlock { .part = vtbackend::CommandBlockPart::Output }));
        CHECK(windowed.session(contour::actions::CopySelection {}));
        CHECK(controller->notices().toastText() == "Copied 2 lines");
    }

    SECTION("an empty selection without a fallback posts nothing")
    {
        CHECK(windowed.session(contour::actions::CopySelection {}));
        CHECK_FALSE(controller->notices().toastShown());
    }

    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: a clipboard over the hard limit is refused with a toast, not a dialog",
          "[contour][session][clipboard][notices]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    constexpr auto FiftyMegabytes = 50 * 1000 * 1000;
    mockPtyOf(windowed.session).stdinBuffer().clear();
    clipboard->setText(QString(FiftyMegabytes, QChar('z')));
    windowed.session.pasteFromClipboard(1, vtbackend::PasteStrip::Keep);

    CHECK_FALSE(windowed.session.pendingPermissionAsk().has_value());
    CHECK(mockPtyOf(windowed.session).stdinBuffer().empty());
    CHECK(
        controller->notices().toastText().toStdString()
        == contour::window::pasteTooLargeNoticeText(contour::session::TerminalSession::PasteHardLimitBytes));

    clipboard->clear();
    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: a vi yank tells its window once, outside the terminal lock",
          "[contour][session][clipboard][notices][vi]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };
    windowed.session.terminal().writeToScreen("yanked line\r\n");
    windowed.session.terminal().writeToScreen("\033[A");

    // What a shown toast does as it shows: asks where to go, which reads the cursor under the terminal lock.
    auto toasts = 0;
    QObject::connect(&controller->notices(), &contour::window::NoticeModel::toastChanged, [&] {
        if (!controller->notices().toastShown())
            return;
        ++toasts;
        std::ignore = controller->toastHost(QStringLiteral("hidden"), QStringLiteral("top"), 0, 0);
    });

    vtbackend::test::failOnDeadlock(std::chrono::seconds(10), [&] {
        windowed.session.terminal().inputHandler().setMode(vtbackend::ViMode::Normal);
        typeKeys(windowed.session, "yy");
    });
    CHECK(toasts == 1);
    CHECK(controller->notices().toastText() == "Copied 1 line");

    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: a program's clipboard write tells no one",
          "[contour][session][clipboard][notices]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    // OSC 52, "c" (the clipboard), "aGk=" ("hi").
    windowed.session.terminal().writeToScreen("\033]52;c;aGk=\033\\");
    QCoreApplication::processEvents();
    CHECK_FALSE(controller->notices().toastShown());
    CHECK(clipboard.text() == "hi"); // it did write

    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: a hint's copy tells its window", "[contour][session][clipboard][notices]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };
    windowed.session.terminal().writeToScreen("see https://contour-terminal.org/ for more\r\n");

    REQUIRE(windowed.session(contour::actions::HintMode { .patterns = "url",
                                                          .hintAction = vtbackend::HintAction::Copy,
                                                          .scope = vtbackend::HintScope::Visible }));
    auto const matches = windowed.session.terminal().hintMatches();
    REQUIRE(matches.size() == 1);
    // The label is typed through the key path, which holds the terminal lock: the copy is told once it is
    // released, as a vi yank's is.
    vtbackend::test::failOnDeadlock(std::chrono::seconds(10),
                                    [&] { typeKeys(windowed.session, matches[0].label); });
    CHECK(clipboard.text() == "https://contour-terminal.org/");
    CHECK(controller->notices().toastText() == "Copied 1 line");

    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: CopyHyperlink tells its window", "[contour][session][clipboard][notices]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const controller { testApp.manager() };
    auto windowed = WindowedSession { controller };
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    REQUIRE(windowed.session(contour::actions::CopyHyperlink { .uri = "https://contour-terminal.org/" }));
    CHECK(clipboard.text() == "https://contour-terminal.org/");
    CHECK(controller->notices().toastText() == "Copied 1 line");

    contour::test::closeAllTabs(*controller);
}

TEST_CASE("TerminalSession: the overlay scrollbar hides at the bottom and shows when scrolled back",
          "[contour][session][scrollbar]")
{
    using contour::config::ScrollBarModePreference;
    TestApp testApp;
    auto session = makeSessionWithProfile(
        testApp.app(),
        registerScrollbarModeProfile(testApp.app(), "overlay", ScrollBarModePreference::Overlay));

    // No history at all: nothing to scroll through.
    publishScrollbarInputs(*session);
    CHECK_FALSE(session->getIsScrollbarVisible());

    writeFiller(session->terminal(), 60);
    publishScrollbarInputs(*session);
    REQUIRE(session->historyLineCount() > 5);
    CHECK_FALSE(session->getIsScrollbarVisible());

    auto changes = QSignalSpy(session.get(), &contour::session::TerminalSession::isScrollbarVisibleChanged);
    session->setScrollOffset(5);
    QCoreApplication::processEvents();
    CHECK(session->getIsScrollbarVisible());
    CHECK(changes.count() == 1);

    session->setScrollOffset(0);
    QCoreApplication::processEvents();
    CHECK_FALSE(session->getIsScrollbarVisible());
    CHECK(changes.count() == 2);
}

TEST_CASE("TerminalSession: a failed command above the viewport keeps the overlay scrollbar",
          "[contour][session][scrollbar]")
{
    using contour::config::ScrollBarModePreference;
    TestApp testApp;

    // Overlay, a command with @p exitCode and a page of output below it, published as the frame timer would.
    auto const visibleAtTheBottomAfter = [&](int exitCode,
                                             vtbackend::ScrollbarMarkSources marks,
                                             int trackPixels) {
        auto session = makeSessionWithProfile(
            testApp.app(),
            registerScrollbarModeProfile(
                testApp.app(), "overlay", ScrollBarModePreference::Overlay, [marks](auto& scrollbar) {
                    scrollbar.marks = marks;
                }));
        session->terminal().writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), exitCode));
        writeFiller(session->terminal(), 60);
        session->setScrollbarMarkTrackPixels(trackPixels);
        publishScrollbarInputs(*session);
        REQUIRE(session->historyLineCount() > 0);
        REQUIRE(session->scrollOffset() == 0);
        return session->getIsScrollbarVisible();
    };

    CHECK(visibleAtTheBottomAfter(2, vtbackend::AllScrollbarMarkSources, 200));
    // A success leaves a Command mark, which holds nothing.
    CHECK_FALSE(visibleAtTheBottomAfter(0, vtbackend::AllScrollbarMarkSources, 200));
    // A failure the bar does not draw holds nothing either.
    CHECK_FALSE(visibleAtTheBottomAfter(2, vtbackend::ScrollbarMarkSources {}, 200));
    // Before QML has published a track, the failure still holds the bar: it is what makes the track appear.
    CHECK(visibleAtTheBottomAfter(2, vtbackend::AllScrollbarMarkSources, 0));
}

TEST_CASE("TerminalSession: a profile switch decides the overlay bar from the new profile's marks",
          "[contour][session][scrollbar]")
{
    // The marks that hold the bar are profile state (scrollbar.marks): the switch re-reads them before it
    // re-announces the visibility, so no frame shows the old profile's answer.
    using contour::config::ScrollBarModePreference;
    TestApp testApp;
    auto const unmarked = registerScrollbarModeProfile(
        testApp.app(), "unmarked", ScrollBarModePreference::Overlay, [](auto& scrollbar) {
            scrollbar.marks = vtbackend::ScrollbarMarkSources {};
        });
    auto const marked =
        registerScrollbarModeProfile(testApp.app(), "marked", ScrollBarModePreference::Overlay);
    auto session = makeSessionWithProfile(testApp.app(), unmarked);
    session->terminal().writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
    writeFiller(session->terminal(), 60);
    publishScrollbarInputs(*session);
    REQUIRE_FALSE(session->getIsScrollbarVisible());

    auto changes = QSignalSpy(session.get(), &contour::session::TerminalSession::isScrollbarVisibleChanged);
    REQUIRE((*session)(contour::actions::ChangeProfile { .name = marked }));
    // No event loop between: the answer is the new profile's at once.
    CHECK(session->getIsScrollbarVisible());
    CHECK(changes.count() == 1);

    REQUIRE((*session)(contour::actions::ChangeProfile { .name = unmarked }));
    CHECK_FALSE(session->getIsScrollbarVisible());
    CHECK(changes.count() == 2);
}

TEST_CASE("TerminalSession: the system's scroller style switches the mode live",
          "[contour][session][scrollbar]")
{
    using contour::config::ScrollBarMode;
    using contour::config::ScrollBarModePreference;

    // Every change goes through the switched appearance fake, as the OS reports it: the app hands it to the
    // manager, which hands it to every session.
    auto accessibility = contour::test::AccessibilitySwitch {};
    auto appearance = contour::test::AppearanceSwitch {};
    auto const styled = [](std::optional<ScrollBarMode> scrollBars) {
        return contour::platform::PlatformAppearance { .accent = std::nullopt,
                                                       .windowCornerRadius = std::nullopt,
                                                       .scrollBars = scrollBars,
                                                       .materials = contour::config::MaterialSupport::Unknown,
                                                       .colorScheme = std::nullopt,
                                                       .buttonLayout = std::nullopt };
    };

    // A session the manager hosts under the default profile, made @p mode, with a page of history published.
    auto const hostedSession = [](contour::test::TestApp& app,
                                  contour::test::ScopedController const& win,
                                  ScrollBarModePreference mode) {
        registerScrollbarModeProfile(app.app(), app.app().profileName(), mode);
        auto* session = app.manager().createSession(win.id);
        REQUIRE(session != nullptr);
        writeFiller(session->terminal(), 60);
        publishScrollbarInputs(*session);
        return session;
    };

    SECTION("a system profile follows each style the OS reports, in every session")
    {
        auto const testApp = makeMotionTestApp(accessibility, appearance);
        contour::test::ScopedController const win { testApp->manager() };
        auto* first = hostedSession(*testApp, win, ScrollBarModePreference::System);
        auto* second = hostedSession(*testApp, win, ScrollBarModePreference::System);
        // No style published yet, which is Overlay.
        CHECK_FALSE(first->getIsScrollbarVisible());
        CHECK_FALSE(second->getIsScrollbarVisible());

        auto changes = QSignalSpy(first, &contour::session::TerminalSession::isScrollbarVisibleChanged);
        appearance.set(styled(ScrollBarMode::Always));
        CHECK(first->getIsScrollbarVisible());
        CHECK(second->getIsScrollbarVisible());
        CHECK(changes.count() == 1);

        // An appearance change that leaves the style be (an accent change, say) announces nothing.
        appearance.set(styled(ScrollBarMode::Always));
        CHECK(changes.count() == 1);

        appearance.set(styled(ScrollBarMode::Overlay));
        CHECK_FALSE(first->getIsScrollbarVisible());
        CHECK_FALSE(second->getIsScrollbarVisible());
        appearance.set(styled(std::nullopt));
        CHECK_FALSE(first->getIsScrollbarVisible());
        CHECK(changes.count() == 2);
    }

    SECTION("an always profile ignores the system")
    {
        auto const testApp = makeMotionTestApp(accessibility, appearance);
        contour::test::ScopedController const win { testApp->manager() };
        auto* session = hostedSession(*testApp, win, ScrollBarModePreference::Always);
        appearance.set(styled(ScrollBarMode::Overlay));
        CHECK(session->getIsScrollbarVisible());
    }

    SECTION("a session reads the style it is born under")
    {
        // Published before the app exists, so only the session's constructor can have read it.
        appearance.appearance = styled(ScrollBarMode::Always);
        auto const testApp = makeMotionTestApp(accessibility, appearance);
        contour::test::ScopedController const win { testApp->manager() };
        auto* session = hostedSession(*testApp, win, ScrollBarModePreference::System);
        CHECK(session->getIsScrollbarVisible());

        // And one born after a change reads the new style.
        appearance.set(styled(ScrollBarMode::Overlay));
        auto* later = hostedSession(*testApp, win, ScrollBarModePreference::System);
        CHECK_FALSE(later->getIsScrollbarVisible());
    }

    SECTION("a Classic session is shown at the bottom, as before")
    {
        TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        REQUIRE(session->profile().scrollbar.value().mode == ScrollBarModePreference::Always);
        writeFiller(session->terminal(), 60);
        publishScrollbarInputs(*session);
        CHECK(session->getIsScrollbarVisible());
    }
}

TEST_CASE("TerminalSession: position hidden and the alternate screen stay hidden",
          "[contour][session][scrollbar]")
{
    using contour::config::ScrollBarModePreference;
    TestApp testApp;

    SECTION("position: Hidden, even under always")
    {
        auto session = makeSessionWithProfile(
            testApp.app(),
            registerScrollbarModeProfile(
                testApp.app(), "hidden", ScrollBarModePreference::Always, [](auto& scrollbar) {
                    scrollbar.position = contour::config::ScrollBarPosition::Hidden;
                }));
        writeFiller(session->terminal(), 60);
        publishScrollbarInputs(*session);
        CHECK_FALSE(session->getIsScrollbarVisible());
    }

    SECTION("the alternate screen under hide_in_alt_screen, without a display")
    {
        auto session = makeSessionWithProfile(
            testApp.app(),
            registerScrollbarModeProfile(
                testApp.app(), "alt", ScrollBarModePreference::Always, [](auto& scrollbar) {
                    scrollbar.hideScrollbarInAltScreen = true;
                }));
        writeFiller(session->terminal(), 60);
        publishScrollbarInputs(*session);
        REQUIRE(session->getIsScrollbarVisible());

        auto changes =
            QSignalSpy(session.get(), &contour::session::TerminalSession::isScrollbarVisibleChanged);
        session->terminal().writeToScreen("\033[?1049h");
        QCoreApplication::processEvents();
        CHECK_FALSE(session->getIsScrollbarVisible());
        CHECK(changes.count() == 1);

        session->terminal().writeToScreen("\033[?1049l");
        QCoreApplication::processEvents();
        CHECK(session->getIsScrollbarVisible());
        CHECK(changes.count() == 2);
    }
}

TEST_CASE("TerminalSession: the size indicator stays quiet for the first second",
          "[contour][session][sizeindicator]")
{
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    auto requests =
        QSignalSpy(pane.session.get(), &contour::session::TerminalSession::sizeIndicatorRequested);

    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(requests.count() == 0);

    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(requests.count() == 1);
    CHECK(pane->sizeIndicatorText() == pageTextOf(*pane));
}

TEST_CASE("TerminalSession: a resize asks for the size indicator outside the lock, with the new main page",
          "[contour][session][sizeindicator]")
{
    // resizeToFit() is applyResize()'s terminal half, which the render thread reaches too: the request is
    // posted, and runs on the session's thread after the resize is done. That nothing arrives before the
    // event loop runs is what shows it is posted; the probe confirms the lock is free by then, which it would
    // be either way. The text is the MAIN page, which the indicator status line is no part of.
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    REQUIRE(pane->terminal().statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

    auto probe = vtbackend::test::LockProbe {};
    auto texts = std::vector<QString> {};
    QObject::connect(pane.session.get(), &contour::session::TerminalSession::sizeIndicatorRequested, [&] {
        probe.probe(pane->terminal());
        texts.push_back(pane->sizeIndicatorText());
    });

    auto const total = totalPageOf(100, 31);
    contour::session::resizeToFit(*pane, total, vtbackend::ImageSize {});
    // Posted, so nothing yet.
    CHECK(texts.empty());
    QCoreApplication::processEvents();

    REQUIRE(texts.size() == 1);
    CHECK(probe.lockWasFree());
    CHECK(pane->pageColumnsCount() == unbox(total.columns));
    CHECK(pane->pageLineCount() < unbox(total.lines));
    CHECK(texts.front() == pageTextOf(*pane));

    // The same page again resizes nothing, and asks for nothing.
    contour::session::resizeToFit(*pane, total, vtbackend::ImageSize {});
    QCoreApplication::processEvents();
    CHECK(texts.size() == 1);
}

TEST_CASE("TerminalSession: a resize on another thread announces the changed page size on the session's",
          "[contour][session][sizeindicator][threading]")
{
    // applyResize() reaches this from the render thread: the signals wake QML, so they are posted and emitted
    // on the session's thread, after the loop runs; each only where its dimension changed.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    auto const* const sessionThread = QThread::currentThread();
    auto lines = std::vector<int> {};
    auto columns = std::vector<int> {};
    auto offThread = 0;
    auto const onSessionThread = [&] {
        if (QThread::currentThread() != sessionThread)
            ++offThread;
    };
    QObject::connect(
        pane.session.get(), &contour::session::TerminalSession::lineCountChanged, [&](int value) {
            onSessionThread();
            lines.push_back(value);
        });
    QObject::connect(
        pane.session.get(), &contour::session::TerminalSession::columnsCountChanged, [&](int value) {
            onSessionThread();
            columns.push_back(value);
        });

    auto const page = [](int columnCount, int lineCount) {
        return vtbackend::PageSize { .lines = vtbackend::LineCount(lineCount),
                                     .columns = vtbackend::ColumnCount(columnCount) };
    };
    std::thread { [&] { contour::session::announcePageSize(*pane, page(80, 25), page(100, 30)); } }.join();
    CHECK(lines.empty());
    CHECK(columns.empty());
    QCoreApplication::processEvents();
    CHECK(lines == std::vector { 30 });
    CHECK(columns == std::vector { 100 });

    // Only the columns changed: only they are announced. The same page: nothing.
    std::thread { [&] { contour::session::announcePageSize(*pane, page(100, 30), page(120, 30)); } }.join();
    std::thread { [&] { contour::session::announcePageSize(*pane, page(120, 30), page(120, 30)); } }.join();
    QCoreApplication::processEvents();
    CHECK(lines == std::vector { 30 });
    CHECK(columns == std::vector { 100, 120 });
    CHECK(offThread == 0);
}

TEST_CASE("TerminalSession: a font zoom reads in points and keeps its text through its resize",
          "[contour][session][sizeindicator]")
{
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    auto requests =
        QSignalSpy(pane.session.get(), &contour::session::TerminalSession::sizeIndicatorRequested);

    auto const before = pane->profile().fonts.value().size.pt;
    REQUIRE((*pane)(contour::actions::IncreaseFontSize {}));
    auto const points = pane->profile().fonts.value().size.pt;
    REQUIRE(points > before);
    auto const pointsText = QString::fromStdString(std::format("{} pt", points));
    // The action asks once, through setFontSize(). The recording surface resizes no page, so no PageSize
    // request follows here as one does under a real display.
    CHECK(requests.count() == 1);
    CHECK(pane->sizeIndicatorText() == pointsText);

    // The page resize the zoom causes keeps saying the size in points.
    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(pane->sizeIndicatorText() == pointsText);

    // Once the pill has settled, a resize reads as the page again.
    pane->sizeIndicatorSettled(true);
    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(pane->sizeIndicatorText() == pageTextOf(*pane));
}

TEST_CASE("TerminalSession: a pane without a display opens no burst", "[contour][session][sizeindicator]")
{
    // A background pane zooms with no display to draw the pill, and so none to settle it. Its next display's
    // first resize must read as the page, not as the zoom.
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    auto requests = QSignalSpy(session.get(), &contour::session::TerminalSession::sizeIndicatorRequested);

    REQUIRE((*session)(contour::actions::IncreaseFontSize {}));
    CHECK(requests.count() == 0);

    auto surface = std::make_unique<contour::test::FakeDisplaySurface>();
    surface->attachedSession = session.get();
    session->attachDisplay(*surface);
    auto const pane = SessionWithSurface { std::move(session), std::move(surface) };

    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(requests.count() == 1);
    CHECK(pane->sizeIndicatorText() == pageTextOf(*pane));
}

TEST_CASE("TerminalSession: only the active pane is announced, once per burst",
          "[contour][session][sizeindicator]")
{
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto active = makeSessionWithSurface(testApp.app());
    auto inactive = makeSessionWithSurface(testApp.app());
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    auto activeAnnouncer = std::make_unique<RecordingAnnouncer>();
    auto inactiveAnnouncer = std::make_unique<RecordingAnnouncer>();
    auto* const activeSaid = activeAnnouncer.get();
    auto* const inactiveSaid = inactiveAnnouncer.get();
    active->setAnnouncer(std::move(activeAnnouncer));
    inactive->setAnnouncer(std::move(inactiveAnnouncer));

    for ([[maybe_unused]] auto const step: std::views::iota(0, 3))
    {
        active->requestSizeIndicator(SizeIndicatorKind::PageSize);
        inactive->requestSizeIndicator(SizeIndicatorKind::PageSize);
    }
    CHECK(activeSaid->said.empty());

    active->sizeIndicatorSettled(true);
    inactive->sizeIndicatorSettled(false);
    REQUIRE(activeSaid->said.size() == 1);
    CHECK(inactiveSaid->said.empty());
    CHECK(activeSaid->said.front().message
          == QString::fromStdString(
              std::format("{} columns, {} rows", active->pageColumnsCount(), active->pageLineCount())));
    CHECK(activeSaid->said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);

    // A burst is announced once: settling again says nothing more.
    active->sizeIndicatorSettled(true);
    CHECK(activeSaid->said.size() == 1);

    SECTION("a zoom says its size in points")
    {
        REQUIRE((*active)(contour::actions::IncreaseFontSize {}));
        active->sizeIndicatorSettled(true);
        REQUIRE(activeSaid->said.size() == 2);
        CHECK(activeSaid->said.back().message
              == QString::fromStdString(std::format("{} points", active->profile().fonts.value().size.pt)));
    }
}

TEST_CASE("TerminalSession: size_indicator_on_resize: false raises nothing",
          "[contour][session][sizeindicator]")
{
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(), registerProfile(testApp.app(), "quiet", [](contour::config::TerminalProfile& p) {
            p.sizeIndicatorOnResize = false;
        }));
    // Past the startup quiet, so only the setting can be what keeps it quiet.
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    auto announcer = std::make_unique<RecordingAnnouncer>();
    auto* const said = announcer.get();
    pane->setAnnouncer(std::move(announcer));
    auto requests =
        QSignalSpy(pane.session.get(), &contour::session::TerminalSession::sizeIndicatorRequested);

    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    REQUIRE((*pane)(contour::actions::IncreaseFontSize {}));
    pane->sizeIndicatorSettled(true);
    CHECK(requests.count() == 0);
    CHECK(said->said.empty());
}

TEST_CASE("TerminalSession: the target pill shows while the follow modifier is held over a link",
          "[contour][session][hyperlink]")
{
    using contour::session::TerminalSession;
    TestApp testApp;
    testApp.app().config().hyperlinkHoverTooltip = false; // the Native2026 reveal
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);

    session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetText() == pageBreadcrumb());
    CHECK_FALSE(session->linkTargetShown());

    auto changes = QSignalSpy(session.session.get(), &TerminalSession::linkTargetChanged);
    sendControl(*session, vtbackend::KeyboardEventType::Press);
    CHECK(session->linkTargetShown());
    CHECK(session->linkTargetHint().toStdString()
          == contour::command::linkOpenHint(vtbackend::Modifiers { vtbackend::Modifier::Control },
                                            vtbackend::Modifiers {},
                                            contour::command::platformShortcutStyle()));
    CHECK(changes.count() == 1);

    sendControl(*session, vtbackend::KeyboardEventType::Release);
    CHECK_FALSE(session->linkTargetShown());
    CHECK(changes.count() == 2);
}

TEST_CASE("TerminalSession: Classic shows the target on hover", "[contour][session][hyperlink]")
{
    TestApp testApp;
    REQUIRE(testApp.app().config().hyperlinkHoverTooltip.value());
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());
    CHECK(session->linkTargetText() == pageBreadcrumb());
    CHECK_FALSE(session->linkTargetHint().isEmpty());
}

TEST_CASE("TerminalSession: a move that brings the follow modifier along shows the pill",
          "[contour][session][hyperlink]")
{
    // Ctrl went down while another window had the focus: no key event comes, the next move carries it.
    TestApp testApp;
    testApp.app().config().hyperlinkHoverTooltip = false;
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
    REQUIRE_FALSE(session->linkTargetShown());

    session->sendMouseMoveEvent(
        vtbackend::Modifiers { vtbackend::Modifier::Control }, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK_FALSE(session->linkTargetShown());
}

TEST_CASE("TerminalSession: while an application takes the mouse the pill names the bypass chord",
          "[contour][session][hyperlink]")
{
    // A click goes to the application unless the bypass modifier is held too: no pill invites one that would
    // not follow, and the one shown names the whole chord.
    using vtbackend::Modifier;
    TestApp testApp;
    testApp.app().config().hyperlinkHoverTooltip = false;
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    session->terminal().writeToScreen("\033[?1000h");

    session->sendMouseMoveEvent(
        vtbackend::Modifiers { Modifier::Control }, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK_FALSE(session->linkTargetShown());

    session->sendMouseMoveEvent(vtbackend::Modifiers { Modifier::Control, Modifier::Shift },
                                OnFirstLink,
                                vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());
    CHECK(session->linkTargetHint().toStdString()
          == contour::command::linkOpenHint(vtbackend::Modifiers { Modifier::Control },
                                            vtbackend::Modifiers { Modifier::Shift },
                                            contour::command::platformShortcutStyle()));
}

TEST_CASE("TerminalSession: a link at the prompt places the pill off the prompt row",
          "[contour][session][hyperlink]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    constexpr auto Prompt = std::string_view { "$ https://x.y" };
    session->terminal().writeToScreen(Prompt);

    auto const clearance = session->overlayClearance();
    auto const columns = clearance.value(QStringLiteral("columns")).toInt();
    CHECK(columns == session->pageColumnsCount());
    auto const avoid = intsAt(clearance, QStringLiteral("avoidRows"));
    CHECK(std::ranges::contains(avoid, 0));
    auto const blank = intsAt(clearance, QStringLiteral("trailingBlank"));
    REQUIRE_FALSE(blank.empty());
    CHECK(blank.front() == columns - static_cast<int>(Prompt.size()));

    // Without a display there is nowhere to place anything.
    CHECK(makeDisplaylessSession(testApp.app())->overlayClearance().isEmpty());
}

TEST_CASE("TerminalSession: the target is still followed when the pill has nowhere to go",
          "[contour][session][hyperlink]")
{
    // A full-screen program paints every cell: no place is blank, the pill shows nowhere -- but it is still
    // shown as far as the session knows, and the link still opens.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033[44m\033[2J\033[1;1H");
    writePageLink(*session);
    session->sendMouseMoveEvent(
        vtbackend::Modifiers { vtbackend::Modifier::Control }, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());

    auto const blank = intsAt(session->overlayClearance(), QStringLiteral("trailingBlank"));
    REQUIRE_FALSE(blank.empty());
    CHECK(std::ranges::all_of(blank, [](int cells) { return cells == 0; }));

    CHECK((*session)(contour::actions::FollowHyperlink {}));
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back() == QUrl(QStringLiteral("https://example.com/page")));
}

TEST_CASE("TerminalSession: output under a shown pill asks QML to place it again, on the GUI thread",
          "[contour][session][hyperlink][threading]")
{
    using contour::session::TerminalSession;
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    auto changes = QSignalSpy(session.session.get(), &TerminalSession::overlayClearanceChanged);

    SECTION("while it shows: posted from the parser thread, published once a frame on the GUI thread")
    {
        session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
        REQUIRE(session->linkTargetShown());
        auto const before = session->overlayClearanceRevision();
        // Nothing is published on the parser thread, nor before the GUI thread's loop runs.
        std::thread { [&] { session->terminal().writeToScreen("\r\nmore"); } }.join();
        CHECK(changes.count() == 0);
        // With the loop run between them, the first write's post has arrived and started the frame; the
        // second lands inside it unless the frame has already passed (a slow machine), so one publication or
        // two -- never one per write and row, and each bumps the revision once.
        QCoreApplication::processEvents();
        std::thread { [&] { session->terminal().writeToScreen("\r\nand more"); } }.join();
        REQUIRE(changes.wait(1000));
        CHECK_FALSE(changes.wait(100));
        CHECK(changes.count() >= 1);
        CHECK(changes.count() <= 2);
        CHECK(session->overlayClearanceRevision() == before + changes.count());

        // More output under the pill asks again.
        auto const settled = session->overlayClearanceRevision();
        std::thread { [&] { session->terminal().writeToScreen("\r\nyet more"); } }.join();
        REQUIRE(changes.wait(1000));
        CHECK(session->overlayClearanceRevision() == settled + 1);
    }

    SECTION("an application that starts taking the mouse under a still pointer hides it within a frame")
    {
        // htop starting while the pointer rests on a link: no move comes, yet a click would now go to the
        // application, so the pill inviting one is re-decided with the clearance.
        session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
        REQUIRE(session->linkTargetShown());
        auto targets = QSignalSpy(session.session.get(), &TerminalSession::linkTargetChanged);
        std::thread { [&] { session->terminal().writeToScreen("\033[?1000h"); } }.join();
        REQUIRE(changes.wait(1000));
        CHECK_FALSE(session->linkTargetShown());
        CHECK(targets.count() == 1);
    }

    SECTION("while it does not: nothing is asked")
    {
        REQUIRE_FALSE(session->linkTargetShown());
        std::thread { [&] { session->terminal().writeToScreen("\r\nmore"); } }.join();
        CHECK_FALSE(changes.wait(100));
    }
}

// }}}

TEST_CASE("TerminalSession: a move onto the next cell of a link with the follow modifier shows the pill",
          "[contour][session][hyperlink]")
{
    // Ctrl went down while another window had the focus; the first move that carries it lands on the link's
    // next cell, which changes no target.
    TestApp testApp;
    testApp.app().config().hyperlinkHoverTooltip = false;
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, OnFirstLink, vtbackend::PixelCoordinate {});
    REQUIRE_FALSE(session->linkTargetShown());

    auto const nextCell =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(2) };
    session->sendMouseMoveEvent(
        vtbackend::Modifiers { vtbackend::Modifier::Control }, nextCell, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());
}

TEST_CASE("TerminalSession: the pill names the bypass chord only where a plain click goes to the application",
          "[contour][session][hyperlink]")
{
    // In vi normal mode a click is the terminal's even while an application tracks the mouse: the hint is the
    // plain gesture there.
    using vtbackend::Modifier;
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writePageLink(*session);
    session->terminal().writeToScreen("\033[?1000h");
    session->terminal().inputHandler().setMode(vtbackend::ViMode::Normal);

    session->sendMouseMoveEvent(
        vtbackend::Modifiers { Modifier::Control }, OnFirstLink, vtbackend::PixelCoordinate {});
    CHECK(session->linkTargetShown());
    CHECK(session->linkTargetHint().toStdString()
          == contour::command::linkOpenHint(vtbackend::Modifiers { Modifier::Control },
                                            vtbackend::Modifiers {},
                                            contour::command::platformShortcutStyle()));
}

TEST_CASE("TerminalSession: a size indicator held when the display goes is no burst on the next one",
          "[contour][session][sizeindicator]")
{
    // A tab switch detaches the display within the hold: the pill's timer goes with it, so nothing settles
    // the burst. The next display's first resize reads as the page, not as the zoom.
    using contour::session::SizeIndicatorKind;
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    testApp.steadyClock().advance(PastSizeIndicatorQuiet);
    REQUIRE((*pane)(contour::actions::IncreaseFontSize {}));
    REQUIRE(pane->sizeIndicatorText().endsWith(QStringLiteral(" pt")));

    pane->detachDisplay(*pane.surface);
    pane->attachDisplay(*pane.surface);
    pane->requestSizeIndicator(SizeIndicatorKind::PageSize);
    CHECK(pane->sizeIndicatorText() == pageTextOf(*pane));
}

TEST_CASE("TerminalSession: focusing the last block says how it went", "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto& terminal = test.session->terminal();

    // The newest command is `ls`, a success; one step further back is the failed `make`.
    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {}));
    REQUIRE(test.recorder->said.size() == 1);
    CHECK(test.recorder->said.back().message.startsWith(QStringLiteral("ls, Succeeded")));
    CHECK(test.recorder->said.back().politeness == QAccessible::AnnouncementPoliteness::Polite);

    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {}));
    REQUIRE(test.recorder->said.size() == 2);
    CHECK(test.recorder->said.back().message.startsWith(QStringLiteral("make, Failed, exit 2")));
    CHECK(test.recorder->said.back().politeness == QAccessible::AnnouncementPoliteness::Polite);

    // Its head is in view, and nothing older is left to step to: silence, and the focus stays.
    auto const* const record = terminal.commandBlocks().find(*test.focused());
    REQUIRE(record != nullptr);
    CHECK(terminal.viewport().isLineVisible(
        *vtbackend::headOffsetOf(terminal.primaryScreen().grid(), *record)));
    CHECK_FALSE((*test.session)(contour::actions::FocusPreviousCommand {}));
    CHECK(test.recorder->said.size() == 2);

    // Forward again, to `ls`.
    REQUIRE((*test.session)(contour::actions::FocusNextCommand {}));
    CHECK(test.recorder->said.back().message.startsWith(QStringLiteral("ls, Succeeded")));
}

TEST_CASE("TerminalSession: stepping with Select takes the output along", "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto& terminal = test.session->terminal();

    REQUIRE((*test.session)(contour::actions::SelectPreviousCommandOutput {}));
    CHECK(test.focused().has_value());
    CHECK(core::locked(terminal, [&] { return terminal.selectionAvailable(); }));
    CHECK(core::locked(terminal, [&] { return terminal.extractSelectionText(); }).contains("out 0"));
}

TEST_CASE("TerminalSession: Ctrl+Enter opens the focused block's menu, and reaches the shell without focus",
          "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto menuBlocks = std::vector<vtbackend::CommandBlockId> {};
    QObject::connect(&testApp.app().sessionsManager(),
                     &contour::session::TerminalSessionManager::blockMenuRequested,
                     [&](contour::session::TerminalSession*, vtbackend::CommandBlockId block) {
                         menuBlocks.push_back(block);
                     });
    auto const ctrl = vtbackend::KeyboardModifiers { vtbackend::Modifier::Control };

    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {}));
    auto const block = test.focused();
    REQUIRE(block.has_value());
    mockPtyOf(*test.session).stdinBuffer().clear();

    test.press(vtbackend::Key::Enter, ctrl);
    REQUIRE(menuBlocks.size() == 1);
    CHECK(menuBlocks.front() == *block);
    CHECK(mockPtyOf(*test.session).stdinBuffer().empty());

    // Shift+F10 is the same menu.
    test.press(vtbackend::Key::F10, vtbackend::KeyboardModifiers { vtbackend::Modifier::Shift });
    CHECK(menuBlocks.size() == 2);

    // Escape lets go of the block; the same chord then belongs to the shell again.
    test.press(vtbackend::Key::Escape);
    CHECK_FALSE(test.focused().has_value());
    mockPtyOf(*test.session).stdinBuffer().clear();
    test.press(vtbackend::Key::Enter, ctrl);
    CHECK(menuBlocks.size() == 2);
    CHECK_FALSE(mockPtyOf(*test.session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: Enter folds the focused block instead of reaching the shell",
          "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto& terminal = test.session->terminal();
    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {})); // `ls`
    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {})); // `make`, the older one
    auto const block = test.focused();
    REQUIRE(block.has_value());
    auto const head = core::locked(terminal, [&] {
        return *vtbackend::headOffsetOf(terminal.primaryScreen().grid(),
                                        *terminal.commandBlocks().find(*block));
    });
    mockPtyOf(*test.session).stdinBuffer().clear();

    test.press(vtbackend::Key::Enter);
    CHECK(core::locked(terminal, [&] { return terminal.foldContaining(head).has_value(); }));
    CHECK(core::locked(terminal, [&] {
        return terminal.foldState().isCollapsed(terminal.foldContaining(head)->headStableId);
    }));
    CHECK(mockPtyOf(*test.session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: typing clears the block focus", "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto const now = std::chrono::steady_clock::now();
    REQUIRE((*test.session)(contour::actions::FocusPreviousCommand {}));
    REQUIRE(test.focused().has_value());

    SECTION("a bare modifier is no typing: Ctrl goes down before the Ctrl+Enter that needs the focus")
    {
        test.press(vtbackend::Key::LeftControl,
                   vtbackend::KeyboardModifiers { vtbackend::Modifier::Control });
        CHECK(test.focused().has_value());
    }

    SECTION("a character that reaches the PTY")
    {
        test.session->sendCharEvent(U'x',
                                    vtbackend::KeyIdentity { .unshiftedKey = U'x' },
                                    Modifiers {},
                                    KeyboardEventType::Press,
                                    now);
        CHECK_FALSE(test.focused().has_value());
        CHECK(mockPtyOf(*test.session).stdinBuffer().ends_with("x"));
    }

    SECTION("a key that reaches the PTY")
    {
        test.press(vtbackend::Key::UpArrow);
        CHECK_FALSE(test.focused().has_value());
    }
}

TEST_CASE("TerminalSession: a nested shell without integration is focused as one running block",
          "[contour][session][blockfocus]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen(vtbackend::test::commandStarted("ssh host"));
    for (auto const row: std::views::iota(0, 25))
        terminal.writeToScreen(std::format("remote$ ls {}\r\nfile {}\r\n", row, row));

    REQUIRE((*session)(contour::actions::FocusPreviousCommand {}));
    auto const running = core::locked(terminal, [&] { return terminal.focusedCommandBlock(); });
    REQUIRE(running.has_value());

    // Nothing after it: the step finds no block, and the focus stays on the running one.
    CHECK_FALSE((*session)(contour::actions::FocusNextCommand {}));
    CHECK(core::locked(terminal, [&] { return terminal.focusedCommandBlock(); }) == running);

    // A typed key clears it.
    session->sendKeyEvent(vtbackend::Key::UpArrow,
                          vtbackend::KeyboardModifiers {},
                          KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    CHECK_FALSE(core::locked(terminal, [&] { return terminal.focusedCommandBlock(); }).has_value());
}

// {{{ Step 3f, Tasks 11 and 12: the sticky header as chrome, a running block when the session ends

TEST_CASE("TerminalSession: the sticky header in the pane header takes no cells",
          "[contour][session][stickyheader]")
{
    using contour::session::ConsumedByStickyHeader;
    using contour::session::TerminalSession;
    using contour::window::StickyHeaderHost;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& session = scene.session();
    auto& terminal = scene.terminal();
    scene.runCommand(numberedOutput(60));
    auto const spy = QSignalSpy(&session, &TerminalSession::stickyHeaderChanged);

    // Following live output under the default mode there is no header to bind, in the chrome or not.
    session.setStickyHeaderHost(StickyHeaderHost::PaneHeader);
    CHECK(terminal.settings().stickyHeader.presentation == vtbackend::StickyHeaderPresentation::Chrome);
    CHECK(session.stickyHeader().isEmpty());
    CHECK(spy.count() == 0);

    // Scrolled into the output: the frame carries it for the chrome, and the session binds it once.
    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    auto const header = vtbackend::test::drawFrame(terminal).stickyHeader;
    REQUIRE(header.has_value());
    CHECK(header->presentation == vtbackend::StickyHeaderPresentation::Chrome);
    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    auto const bound = session.stickyHeader();
    CHECK(bound.value(QStringLiteral("command")).toString() == QStringLiteral("make"));
    CHECK(bound.value(QStringLiteral("glyph")).toString() == QStringLiteral("✓"));
    CHECK(bound.value(QStringLiteral("outcome")).toInt()
          == static_cast<int>(vtbackend::CommandBlockOutcome::Success));
    CHECK(bound.value(QStringLiteral("statusRole")).toInt()
          == static_cast<int>(contour::config::StatusColorRole::Success));
    CHECK_FALSE(bound.value(QStringLiteral("jumpLabel")).toString().isEmpty());
    // Where it lives, so a host that shows only its own (the title band) can tell.
    CHECK(bound.value(QStringLiteral("host")).toInt() == static_cast<int>(StickyHeaderHost::PaneHeader));

    // Another frame showing the same says nothing new.
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);

    // Row 0 is the grid's: a press there is no press on a header.
    CHECK(session.sendStickyHeaderPressEvent(
              vtbackend::LineOffset(0), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
          == ConsumedByStickyHeader::No);

    // A profile switch -- the path a configuration reload takes too -- keeps where the header lives: it is
    // the card's, not the profile's.
    auto const other =
        registerProfile(testApp.app(), "sticky-chrome", [](contour::config::TerminalProfile&) {});
    REQUIRE(session(contour::actions::ChangeProfile { .name = other }));
    CHECK(terminal.settings().stickyHeader.presentation == vtbackend::StickyHeaderPresentation::Chrome);

    // The jump link brings the command to the top.
    session.jumpToStickyHeader();
    CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));

    // Back in the grid, the chrome shows nothing and row 0 jumps as before.
    session.setStickyHeaderHost(StickyHeaderHost::Grid);
    CHECK(session.stickyHeader().isEmpty());
    REQUIRE(terminal.viewport().scrollToBottom());
    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    REQUIRE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());
    CHECK(session.sendStickyHeaderPressEvent(
              vtbackend::LineOffset(0), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
          == ConsumedByStickyHeader::Yes);
}

TEST_CASE("TerminalSession: a host change with the header shown reaches the chrome without a scroll",
          "[contour][session][stickyheader]")
{
    using contour::session::TerminalSession;
    using contour::window::StickyHeaderHost;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& session = scene.session();
    auto& terminal = scene.terminal();
    scene.runCommand(numberedOutput(60));
    session.setStickyHeaderHost(StickyHeaderHost::PaneHeader);
    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    REQUIRE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());
    QCoreApplication::processEvents();
    REQUIRE(session.stickyHeader().value(QStringLiteral("host")).toInt()
            == static_cast<int>(StickyHeaderHost::PaneHeader));

    // The other pane closed: the card now announces the title band. The text is unchanged, and nothing
    // scrolls, yet the host that shows it moved.
    auto const spy = QSignalSpy(&session, &TerminalSession::stickyHeaderChanged);
    session.setStickyHeaderHost(StickyHeaderHost::TitleBand);
    CHECK(spy.count() == 1);
    CHECK(session.stickyHeader().value(QStringLiteral("host")).toInt()
          == static_cast<int>(StickyHeaderHost::TitleBand));
    CHECK(session.stickyHeader().value(QStringLiteral("command")).toString() == QStringLiteral("make"));
}

TEST_CASE("TerminalSession: the title band follows the active session's sticky header",
          "[contour][session][stickyheader][manager]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    auto const spy = QSignalSpy(&*window, &contour::window::WindowController::activeStickyHeaderChanged);

    session->setStickyHeaderHost(contour::window::StickyHeaderHost::TitleBand);
    auto& terminal = session->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(60)));
    REQUIRE(core::locked(terminal, [&] { return terminal.viewport().scrollUp(vtbackend::LineCount(5)); }));
    REQUIRE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());
    QCoreApplication::processEvents();
    CHECK(spy.count() >= 1);
    CHECK(window->activeStickyHeader().value(QStringLiteral("command")).toString() == QStringLiteral("make"));
    CHECK(window->activeStickyHeader().value(QStringLiteral("host")).toInt()
          == static_cast<int>(contour::window::StickyHeaderHost::TitleBand));

    // The band's jump link brings the command to the top of the pane.
    window->jumpToActiveStickyHeader();
    CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));

    // Another tab in front: its session has no header to show.
    window->createNewTab();
    REQUIRE(window->activeSession() != session);
    CHECK(window->activeStickyHeader().isEmpty());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: QML hears the sticky header host as an int, and nothing outside the enum",
          "[contour][session][stickyheader]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->setStickyHeaderHost(static_cast<int>(contour::window::StickyHeaderHost::TitleBand));
    CHECK(session->stickyHeaderHost() == contour::window::StickyHeaderHost::TitleBand);
    session->setStickyHeaderHost(7);
    CHECK(session->stickyHeaderHost() == contour::window::StickyHeaderHost::TitleBand);
}

TEST_CASE("TerminalSession: QML sees which block the keyboard focuses", "[contour][session][blockfocus]")
{
    using contour::session::TerminalSession;

    TestApp testApp;
    auto test = BlockFocusSession { testApp.app() };
    auto& session = *test.session;
    auto const spy = QSignalSpy(&session, &TerminalSession::focusedCommandBlockChanged);
    CHECK(session.focusedCommandBlock() == 0);

    REQUIRE(session(contour::actions::FocusPreviousCommand {}));
    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    REQUIRE(test.focused().has_value());
    CHECK(session.focusedCommandBlock() == test.focused()->value);

    REQUIRE(session(contour::actions::FocusPreviousCommand {}));
    QCoreApplication::processEvents();
    CHECK(spy.count() == 2);
    CHECK(session.focusedCommandBlock() == test.focused()->value);

    REQUIRE(session(contour::actions::ClearCommandBlockFocus {}));
    QCoreApplication::processEvents();
    CHECK(spy.count() == 3);
    CHECK(session.focusedCommandBlock() == 0);
}

TEST_CASE("TerminalSession: a block running when the shell exits stops running",
          "[contour][session][paneend]")
{
    // Review Focus 3, and N5: no ;D will come from a shell that is gone. The ended pane shows no running
    // mark, no hollow tick and no ticking time, and nothing wakes it once a second.
    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    terminal.writeToScreen(vtbackend::test::commandStarted("sleep 100"));
    writeFiller(terminal, 60);
    pane->setScrollbarMarkTrackPixels(200);
    // Any drawn row of the block reads as running: its rail, and the mark on its head.
    auto const runningRail = [&] {
        return std::ranges::any_of(vtbackend::test::drawFrame(terminal).gutter, [](auto const& cell) {
            return cell.outcome == vtbackend::CommandBlockOutcome::Running
                   || cell.mark == vtbackend::RailMark::Running;
        });
    };
    auto const hollowTicks = [&] {
        pane->refreshScrollbarMarks();
        return std::ranges::count_if(
            std::views::iota(qsizetype { 0 }, pane->scrollbarMarks().size()), [&](qsizetype index) {
                return markStyle(*pane, index) == static_cast<int>(vtbackend::ScrollbarTickStyle::Hollow);
            });
    };
    // Scrolled to the command's own row, where its chip ticks.
    REQUIRE(terminal.viewport().scrollToTop());
    REQUIRE(runningRail());
    QCoreApplication::processEvents();
    REQUIRE(terminal.hasVisibleRunningStatus());
    REQUIRE(pane->refreshesRunningStatus());
    REQUIRE(hollowTicks() == 1);

    terminal.device().close();
    pane->onClosed();
    REQUIRE(pane->getSessionEnd() != static_cast<int>(contour::session::EndReason::None));

    CHECK_FALSE(pane->refreshesRunningStatus());
    auto const* const record = terminal.commandBlocks().lastCommand();
    REQUIRE(record != nullptr);
    CHECK(record->state == vtbackend::CommandBlockState::Finished);
    CHECK(record->end == vtbackend::CommandBlockEnd::Abandoned);
    CHECK_FALSE(runningRail());
    QCoreApplication::processEvents();
    CHECK_FALSE(terminal.hasVisibleRunningStatus());
    CHECK_FALSE(pane->refreshesRunningStatus());
    CHECK(hollowTicks() == 0);

    // Nor does the exposure coming back start the tick again.
    pane->displayExposureChanged();
    CHECK_FALSE(pane->refreshesRunningStatus());
}

TEST_CASE("TerminalSession: an abandoned block raises no finish notice",
          "[contour][session][paneend][notification]")
{
    // Under `always`, with no floor: a command that ended would be notified. One whose shell went away
    // ended unseen -- nothing is raised, and nothing withdrawn.
    FinishNoticeHarness h { { .when = NotifyWhen::Always, .minDuration = 0s } };
    h.write(vtbackend::test::commandStarted("sleep 100"));

    SECTION("the shell exits")
    {
        h.session->terminal().device().close();
        h.session->onClosed();
    }
    SECTION("the pane is closed")
    {
        h.session->terminate();
        h.session->onClosed();
    }
    h.deliver();

    CHECK(h.recorder->raised.empty());
    CHECK(h.recorder->closed.empty());
    auto const* const record = h.session->terminal().commandBlocks().lastCommand();
    REQUIRE(record != nullptr);
    CHECK(record->end == vtbackend::CommandBlockEnd::Abandoned);
}

TEST_CASE("TerminalSession: a dropped path arrives as a paste", "[contour][session][drop]")
{
    // Review Focus 1: the text is a paste, never keystrokes a Return could be among.
    TestApp testApp;
    auto const file = QList<QUrl> { QUrl(QStringLiteral("file:///Users/me/My%20Report.pdf")) };

    SECTION("bracketed when the application asked for it")
    {
        auto session = makeDropSession(testApp, "zsh", true);
        session->insertDroppedUrls(file);
        CHECK(mockPtyOf(*session).stdinBuffer() == "\033[200~/Users/me/My\\ Report.pdf \033[201~");
    }

    SECTION("plain otherwise, and without a Return")
    {
        auto session = makeDropSession(testApp, "zsh", false);
        session->insertDroppedUrls(file);
        CHECK(mockPtyOf(*session).stdinBuffer() == "/Users/me/My\\ Report.pdf ");
    }

    SECTION("several files are one paste")
    {
        auto session = makeDropSession(testApp, "zsh", true);
        session->insertDroppedUrls(
            { QUrl(QStringLiteral("file:///tmp/a")), QUrl(QStringLiteral("file:///tmp/b c")) });
        CHECK(mockPtyOf(*session).stdinBuffer() == "\033[200~/tmp/a /tmp/b\\ c \033[201~");
    }
}

TEST_CASE("TerminalSession: dropped paths are quoted for the shell in front", "[contour][session][drop]")
{
    TestApp testApp;
    auto const file = QList<QUrl> { QUrl(QStringLiteral("file:///tmp/it's%20here.txt")) };

    SECTION("PowerShell takes single quotes")
    {
        auto session = makeDropSession(testApp, "pwsh", false);
        session->insertDroppedUrls(file);
        CHECK(mockPtyOf(*session).stdinBuffer() == "'/tmp/it''s here.txt' ");
    }

    SECTION("a known POSIX shell gets POSIX escapes")
    {
        auto session = makeDropSession(testApp, "zsh", false);
        session->insertDroppedUrls(file);
        CHECK(mockPtyOf(*session).stdinBuffer() == "/tmp/it\\'s\\ here.txt ");
    }

    SECTION("a program that is no known POSIX shell refuses a name with a metacharacter")
    {
        for (auto const* program: { "nu", "elvish", "tmux", "mosh-client", "vim" })
        {
            auto session = makeDropSession(testApp, program, false);
            session->insertDroppedUrls(file);
            CHECK(mockPtyOf(*session).stdinBuffer().empty());
        }
    }

    SECTION("a pane with no foreground process to ask (a daemon's) guesses no shell from the profile")
    {
        auto const profile = registerProfile(
            testApp.app(), "pwsh-profile", [](auto& p) { p.shell.value().program = "pwsh -NoLogo"; });
        auto session = std::make_unique<contour::session::TerminalSession>(
            &testApp.app().sessionsManager(),
            std::make_unique<vtpty::MockPty>(TestPageSize),
            testApp.app(),
            profile,
            std::nullopt,
            std::nullopt,
            contour::test::makeTestNotifier(),
            contour::session::SessionRole::Shell,
            contour::session::SessionHosting::Remote);
        mockPtyOf(*session).setForegroundProcessName(std::string { "zsh" }); // not asked: it is not ours

        // A name any shell reads as syntax (here a quote, live in PowerShell, bash and cmd alike) is refused
        // whatever the profile says.
        session->insertDroppedUrls(file);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());

        // A name without any is typed, double-quoted.
        session->insertDroppedUrls({ QUrl(QStringLiteral("file:///tmp/my%20file.txt")) });
        CHECK(mockPtyOf(*session).stdinBuffer() == "\"/tmp/my file.txt\" ");
    }
}

TEST_CASE("TerminalSession: a file name that could run something refuses the whole drop",
          "[contour][session][drop]")
{
    TestApp testApp;
    auto session = makeDropSession(testApp, "zsh", true);

    // %0A is a newline: typed, it would press Return after whatever came before it.
    session->insertDroppedUrls(
        { QUrl(QStringLiteral("file:///tmp/fine")), QUrl(QStringLiteral("file:///tmp/x%0Arm%20-rf%20~")) });
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // not even the name that was fine

    session->insertDroppedUrls({ QUrl(QStringLiteral("file:///tmp/esc%1B%5B31m")) });
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // The same names under cmd refuse for what cmd cannot quote.
    mockPtyOf(*session).setForegroundProcessName(std::string { "cmd.exe" });
    session->insertDroppedUrls({ QUrl(QStringLiteral("file:///tmp/%25PATH%25")) });
    CHECK(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: a dropped word that starts with a dash is never typed as an option",
          "[contour][session][drop]")
{
    // The drag source chooses the uri-list, so `-rf` (a relative URL) and `file:-rf` (a local file whose path
    // is `-rf`) are possible; neither may reach a program as an option.
    TestApp testApp;
    auto const link = QList<QUrl> { QUrl(QStringLiteral("-rf")) };
    auto const file = QList<QUrl> { QUrl(QStringLiteral("file:-rf")) };

    for (auto const* program: { "pwsh", "cmd", "nu", "zsh" })
    {
        CAPTURE(program);
        auto session = makeDropSession(testApp, program, false);
        session->insertDroppedUrls(file);
        auto const typed = mockPtyOf(*session).stdinBuffer();
        CHECK(typed.contains("./-rf"));
        CHECK_FALSE(typed.contains("\"-rf")); // the quote does not open on the dash
        CHECK_FALSE(typed.contains("'-rf"));
    }

    // A relative link has no path to put `./` in front of: cmd, PowerShell and an unknown shell refuse it.
    for (auto const* program: { "pwsh", "cmd", "nu" })
    {
        CAPTURE(program);
        auto session = makeDropSession(testApp, program, false);
        session->insertDroppedUrls(link);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }
}

TEST_CASE("TerminalSession: a dropped URL list never types a shell expansion or a line break",
          "[contour][session][drop]")
{
    // A web page chooses the text/uri-list it drags: `$(...)` in a URL would run when the shell reads
    // the line, with no Return pressed, so each URL is typed as one quoted word.
    TestApp testApp;
    auto const urls = QList<QUrl> { QUrl(QStringLiteral("https://a/$(touch${IFS}x)")),
                                    QUrl(QStringLiteral("https://b/`id`;rm")),
                                    QUrl(QStringLiteral("file:///tmp/it's")) };

    for (auto const bracketed: { true, false })
    {
        CAPTURE(bracketed);
        auto session = makeDropSession(testApp, "sh", bracketed);
        session->insertDroppedUrls(urls);
        auto const typed = mockPtyOf(*session).stdinBuffer();
        CHECK_FALSE(typed.contains('\n'));
        CHECK_FALSE(typed.contains('\r'));
        CHECK_FALSE(typed.contains("$(")); // only as \$\(
        CHECK_FALSE(typed.contains('`'));
        CHECK(typed.contains("\\$\\(touch\\$"));
        CHECK(typed.contains("id%60\\;rm")); // the encoded form, and the `;` escaped
    }
}

TEST_CASE("TerminalSession: dropped web links and text are pasted as text", "[contour][session][drop]")
{
    TestApp testApp;

    SECTION("a web link is quoted as one word, as a file name is")
    {
        auto session = makeDropSession(testApp, "zsh", true);
        session->insertDroppedUrls({ QUrl(QStringLiteral("https://example.com/a?b=1&c=2")) });
        CHECK(mockPtyOf(*session).stdinBuffer()
              == "\033[200~https://example.com/a\\?b\\=1\\&c\\=2 \033[201~");
    }

    SECTION("text")
    {
        auto session = makeDropSession(testApp, "zsh", true);
        session->insertDroppedText(QStringLiteral("echo hi"));
        CHECK(mockPtyOf(*session).stdinBuffer() == "\033[200~echo hi\033[201~");
    }

    SECTION("text with a line break is typed only where bracketed paste brackets it")
    {
        auto bracketed = makeDropSession(testApp, "zsh", true);
        bracketed->insertDroppedText(QStringLiteral("curl x|sh\nid"));
        CHECK(mockPtyOf(*bracketed).stdinBuffer() == "\033[200~curl x|sh\nid\033[201~");

        // Without it every line would press Return as it arrives: refused, nothing typed.
        for (auto const* text: { "curl x|sh\n", "a\nb", "a\rb", "a\r\nb" })
        {
            CAPTURE(text);
            auto plain = makeDropSession(testApp, "sh", false);
            plain->insertDroppedText(QString::fromLatin1(text));
            CHECK(mockPtyOf(*plain).stdinBuffer().empty());
        }

        // One line is as harmless as a paste.
        auto plain = makeDropSession(testApp, "sh", false);
        plain->insertDroppedText(QStringLiteral("echo hi"));
        CHECK(mockPtyOf(*plain).stdinBuffer() == "echo hi");
    }

    SECTION("empty text is nothing")
    {
        auto session = makeDropSession(testApp, "zsh", true);
        session->insertDroppedText({});
        session->insertDroppedUrls({});
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }
}

TEST_CASE("TerminalSession: a dropped text over the soft limit asks first", "[contour][session][drop]")
{
    TestApp testApp;
    auto session = makeDropSession(testApp, "zsh", true);
    auto permissionRequests = 0;
    contour::test::countAsks(*session,
                             contour::session::PermissionAsk { contour::session::GuardedRole::BigPaste },
                             permissionRequests);

    session->insertDroppedText(QString((1024 * 512) + 1, QChar('y')));
    CHECK(permissionRequests == 1);
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // held for the verdict

    session->applyPendingPaste(PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("yyy"));

    // And above the hard limit it is refused outright.
    mockPtyOf(*session).stdinBuffer().clear();
    session->insertDroppedText(QString((1024 * 1024) + 1, QChar('z')));
    CHECK(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: an ended pane takes no drop", "[contour][session][drop]")
{
    // Typed into an ended pane, a path would be swallowed or kept for whatever Restart starts, and a Return
    // after it would run it.
    TestApp testApp;
    auto session =
        makeSessionWith(testApp.app(),
                        std::make_unique<contour::test::ConfigurableStartPty>(
                            TestPageSize, contour::test::ConfigurableStartPty::StartBehavior::Fail));
    session->start();
    REQUIRE(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::StartFailed));

    // What a live pane does with a drop this big is ask first; an ended one does not even get that far.
    auto permissionRequests = 0;
    contour::test::countAsks(*session,
                             contour::session::PermissionAsk { contour::session::GuardedRole::BigPaste },
                             permissionRequests);
    session->insertDroppedText(QString((1024 * 512) + 1, QChar('y')));
    session->insertDroppedUrls(
        { QUrl(QStringLiteral("https://example.com/") + QString((1024 * 512) + 1, QChar('y'))) });
    CHECK(permissionRequests == 0);
}
