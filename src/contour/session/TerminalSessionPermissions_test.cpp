// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession tests, continued: display-guarded actions, search, the guarded roles and their
// permission memory, buffer capture, font changes and opening hyperlinks.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/HyperlinkTooltip.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionTestSupport.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Utils.hpp>
#include <core/log/LogSink.hpp>

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
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
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

using namespace contour::test::sessionSupport;

namespace
{

using contour::test::shownQuestionId;

/// Counts how often @p needle appears on @p session's visible page. Used to assert that a notice
/// the session writes in its own voice is written exactly once.
[[nodiscard]] size_t countOnScreen(contour::session::TerminalSession& session, std::string_view needle)
{
    auto const screen = session.terminal().primaryScreen().renderMainPageText();
    auto count = size_t { 0 };
    for (auto pos = screen.find(needle); pos != std::string::npos; pos = screen.find(needle, pos + 1))
        ++count;
    return count;
}

/// A finished block printed at the bottom of rows 1-12, so the rows above it scroll up within those margins:
/// marked rows and blocks' heads among them.
constexpr auto MarginScrollingBlock = std::string_view {
    "\033[1;12r\033[12H\r\n\033]133;A\033\\$ x\r\n\033]133;C\033\\o\r\n\033]133;D;0\033\\"
};

/// Runs the count's walk to its end, a step at a time, as the event loop would; returns how many steps.
int drainSearchTally(contour::session::TerminalSession& session)
{
    auto steps = 0;
    while (session.searchTallyRunning())
    {
        session.runSearchTallyStep();
        ++steps;
    }
    return steps;
}

/// Writes @p count rows, row @p i reading @p rowText(i).
template <typename RowText>
void writeSearchRows(vtbackend::Terminal& terminal, int count, RowText rowText)
{
    auto text = std::string {};
    for (auto const row: std::views::iota(0, count))
        text += rowText(row) + "\r\n";
    terminal.writeToScreen(text);
}

/// Gives @p session room for @p rows of history.
void allowHistory(contour::session::TerminalSession& session, int rows)
{
    auto const _ = std::scoped_lock { session.terminal() };
    session.terminal().setHistoryLimits(
        vtbackend::HistoryLimits::plain(vtbackend::MaxHistoryLineCount(vtbackend::LineCount(rows))));
}

constexpr auto CaptureReplyTerminator = std::string_view { "\033^314;\033\\" };

/// How many (non-overlapping) times @p needle occurs in @p haystack.
[[nodiscard]] size_t countOccurrences(std::string_view haystack, std::string_view needle)
{
    auto found = size_t { 0 };
    for (auto at = haystack.find(needle); at != std::string_view::npos;
         at = haystack.find(needle, at + needle.size()))
        ++found;
    return found;
}

/// XTCAPTURE for the whole page: `CSI > Ps ; Ps , t`, physical lines, count defaulted to the page.
/// The ',' intermediate is load-bearing — the bare `CSI > Ps ; Ps t` is xterm's XTSMTITLE.
constexpr auto CaptureWholePageSequence = std::string_view { "\033[>0,t" };

/// Counts the buffer-capture permission dialogs @p session raises into @p asks. The connection lives
/// with the session, which every caller outlives.
void countCaptureAsks(contour::session::TerminalSession& session, int& asks)
{
    contour::test::countAsks(
        session, contour::session::PermissionAsk { contour::session::GuardedRole::CaptureBuffer }, asks);
}

/// A font request a program might make: a family and a size.
[[nodiscard]] vtbackend::FontDef requestedFont()
{
    auto font = vtbackend::FontDef {};
    font.size = 17.0;
    font.regular = "Iosevka";
    return font;
}

/// A copy of @p app's configuration in which profile @p name configures capture_buffer as @p permission: what
/// a reload reads from a file saved with that one setting changed, built in memory.
[[nodiscard]] contour::config::Config withCapturePermission(contour::ContourGuiApp& app,
                                                            std::string const& name,
                                                            contour::config::Permission permission)
{
    auto config = app.config();
    config.profiles.value().at(name).permissions.value().captureBuffer = permission;
    return config;
}

} // namespace

TEST_CASE("TerminalSession: Vi-mode toggling does not race concurrent screen writes",
          "[contour][session][actions][threading]")
{
    // The production shape of #1495: the parser thread writes cells while the GUI thread toggles Vi
    // mode, whose status-line push reflows the grid. This is the case that must be run under
    // ThreadSanitizer (`ctest --preset=clang-tsan`) -- without the lock in ViNormalMode, TSan reports
    // a write-write race on the grid between Screen::writeText and Grid::resize. Under ASan it stands
    // as a plain no-deadlock / no-corruption smoke test.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    {
        // Stands in for Terminal::processInputOnce(): writeToScreen() takes the terminal lock around
        // the parse exactly as the real parser thread does.
        auto const writer = vtbackend::test::writeOnOwnThread(
            [&] { session->terminal().writeToScreen("hello \033[1mworld\033[m\r\n"); });

        auto constexpr ToggleCount = 100;
        for ([[maybe_unused]] auto const i: std::views::iota(0, ToggleCount))
            (*session)(actions::ViNormalMode {});
    }

    // An even number of toggles lands back in Insert mode, and the terminal is still consistent.
    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Insert);
    CHECK(session->terminal().totalPageSize().lines > vtbackend::LineCount(0));
}

TEST_CASE("TerminalSession: Vi marks and folds do not race output scrolling a margin region",
          "[contour][session][vi][threading]")
{
    // `mm` writes a row's flags and the user-mark index, `za` the fold set. The parser thread writes both
    // under the terminal lock when a scroll inside DECSTBM margins carries a marked row or a block's head
    // (Screen::followRowMove), so the key path has to hold that lock as well. This is the case to run under
    // ThreadSanitizer, which reports the key path's writes against the parser's without it. Elsewhere it
    // stands as a no-deadlock, no-corruption smoke test.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto const type = [&](std::string_view keys) {
        typeKeys(*session, keys);
    };

    // Three finished blocks and a prompt below them, all inside the margins the writer sets: `za` has folds
    // to toggle, and the scroll carries their heads. The Vi cursor goes onto the last block's output.
    for (auto const i: std::views::iota(0, 3))
        writeFinishedBlock(*session, std::format("cmd{}", i), "out a\r\nout b\r\n");
    (*session)(contour::actions::ViNormalMode {});
    type("k");

    // Both keys reach their Vi commands, and change what the parser thread changes too.
    auto const foldRevision = terminal.foldState().revision();
    type("mmza");
    REQUIRE(terminal.userMarks().ids().size() == 1);
    REQUIRE(terminal.foldState().revision() != foldRevision);

    {
        // Stands in for the parser thread, which writes under the terminal lock (writeToScreen() takes it).
        auto const writer =
            vtbackend::test::writeOnOwnThread([&] { terminal.writeToScreen(MarginScrollingBlock); });
        auto constexpr RoundCount = 100;
        for ([[maybe_unused]] auto const round: std::views::iota(0, RoundCount))
            type("mmza");
    }

    CHECK(terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
}

TEST_CASE("TerminalSession: a vi mark or fold asks for new scrollbar ticks without retaking the lock",
          "[contour][session][vi][scrollbar-marks][threading]")
{
    // `mm` and `za` run under the terminal lock and raise screenUpdated(), whose request for new ticks may
    // only post: the refresh takes that lock, which is not recursive. The writer thread then races output
    // against both keys and the refreshes the GUI thread runs -- the case to run under ThreadSanitizer. Its
    // screenUpdated() must not rebuild the fold projection the keys rebuild under the lock.
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    pane->setScrollbarMarkTrackPixels(200);

    for (auto const i: std::views::iota(0, 3))
        writeFinishedBlock(*pane, std::format("cmd{}", i), "out a\r\nout b\r\n");
    auto settled = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
    REQUIRE(settled.wait(1000));

    (*pane)(contour::actions::ViNormalMode {});
    typeKeys(*pane, "k");
    auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);

    typeKeys(*pane, "mm");
    REQUIRE(spy.wait(1000));
    CHECK(std::ranges::contains(markKindsOf(*pane), ScrollbarMarkKind::UserMark));

    // Closed, then opened again.
    for ([[maybe_unused]] auto const toggle: std::views::iota(0, 2))
    {
        auto const foldRevision = terminal.foldState().revision();
        typeKeys(*pane, "za");
        REQUIRE(terminal.foldState().revision() != foldRevision);
        REQUIRE(spy.wait(1000));
    }

    {
        auto const writer =
            vtbackend::test::writeOnOwnThread([&] { terminal.writeToScreen(MarginScrollingBlock); });

        // The GUI thread turns between the keys, so the refreshes they ask for run while the writer writes.
        auto constexpr RoundCount = 50;
        for ([[maybe_unused]] auto const round: std::views::iota(0, RoundCount))
        {
            typeKeys(*pane, "mmza");
            QTest::qWait(1);
        }
    }

    CHECK(terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
}

TEST_CASE("TerminalSession: output evicting collapsed folds races no scrollbar refresh",
          "[contour][session][scrollbar-marks][folding][threading]")
{
    // Output into a full history moves the floor with every batch, so each refresh prunes the collapsed folds
    // whose heads it evicted: a write to the fold state, under the lock. The parser thread's screenUpdated()
    // runs without the lock, so it may read none of that state -- the case to run under ThreadSanitizer.
    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(
            testApp.app(), "shallow", vtbackend::AllScrollbarMarkSources, vtbackend::LineCount(20)));
    auto& terminal = pane->terminal();
    pane->setScrollbarMarkTrackPixels(200);

    for (auto const i: std::views::iota(0, 3))
        terminal.writeToScreen(vtbackend::test::commandRun(std::format("cmd {}", i), numberedOutput(3), 0));
    REQUIRE((*pane)(contour::actions::CollapseAllFolds {}));
    auto const oldest = terminal.foldRanges().back().headStableId;
    REQUIRE(terminal.foldState().isCollapsed(oldest));

    // The GUI thread refreshes as fast as it can rather than once a frame, so the prunes meet the parser
    // thread's screen updates as often as possible, and collapses whatever arrived before each, so every
    // eviction has folds to take. It does so for as long as the writer writes its rounds rather than for a
    // count of its own: each refresh takes the lock, and where that lock is not fair -- on macOS -- five
    // hundred of them ran before the writer had written enough to evict anything.
    auto const block = vtbackend::test::commandRun("x", numberedOutput(2), 0);
    std::ignore = vtbackend::test::runWhileWriting([&] { terminal.writeToScreen(block); },
                                                   [&] {
                                                       std::ignore =
                                                           (*pane)(contour::actions::CollapseAllFolds {});
                                                       pane->refreshScrollbarMarks();
                                                       QCoreApplication::processEvents();
                                                   });

    pane->refreshScrollbarMarks();
    REQUIRE(terminal.primaryScreen().grid().stableRangeFloor() > oldest);
    CHECK_FALSE(terminal.foldState().isCollapsed(oldest));
}

TEST_CASE("TerminalSession: ScreenshotVT writes the screen capture to a file", "[contour][session][actions]")
{
    // ScreenshotVT serializes the active screen to screenshot.vt in the cwd — a display-independent
    // action (no GPU/window). Run it in a temp cwd so the artifact does not pollute the tree.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    session->terminal().writeToScreen("hello screenshot\r\n");

    auto const tmp = std::filesystem::temp_directory_path()
                     / std::format("contour-vt-shot-{}", QCoreApplication::applicationPid());
    std::filesystem::create_directories(tmp);
    auto const prev = std::filesystem::current_path();
    std::filesystem::current_path(tmp);
    auto const restore = core::Finally { [&] {
        std::filesystem::current_path(prev);
        std::filesystem::remove_all(tmp);
    } };

    CHECK((*session)(actions::ScreenshotVT {}));
    CHECK(std::filesystem::exists(tmp / "screenshot.vt"));
    CHECK(std::filesystem::file_size(tmp / "screenshot.vt") > 0);
}

TEST_CASE("TerminalSession: key and char events encode modifiers into the PTY", "[contour][session][input]")
{
    using vtbackend::Key;
    using vtbackend::KeyboardEventType;
    using vtbackend::Modifier;
    using vtbackend::Modifiers;

    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    // A control character: Ctrl+C encodes as 0x03.
    pty.stdinBuffer().clear();
    session->sendCharEvent(U'c',
                           vtbackend::KeyIdentity { .unshiftedKey = U'c' },
                           Modifiers { Modifier::Control },
                           KeyboardEventType::Press,
                           std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer().contains('\x03'));

    // A function/navigation key produces its escape sequence (CSI-prefixed).
    pty.stdinBuffer().clear();
    session->sendKeyEvent(
        Key::UpArrow, Modifiers {}, KeyboardEventType::Press, std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer().contains('\033'));

    // A key RELEASE encodes nothing (the legacy protocol is press-only).
    pty.stdinBuffer().clear();
    session->sendKeyEvent(
        Key::UpArrow, Modifiers {}, KeyboardEventType::Release, std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer().empty());
}

TEST_CASE("TerminalSession: mouse events encode under an active mouse protocol",
          "[contour][session][input][mouse]")
{
    using vtbackend::Modifier;
    using vtbackend::Modifiers;
    using vtbackend::MouseButton;
    using vtbackend::PixelCoordinate;

    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    // Enable normal (1000) + SGR (1006) mouse reporting.
    session->terminal().writeToScreen("\033[?1000h\033[?1006h");

    pty.stdinBuffer().clear();
    auto const at = PixelCoordinate { PixelCoordinate::X { 20 }, PixelCoordinate::Y { 20 } };
    session->sendMousePressEvent(Modifiers {}, MouseButton::Left, vtbackend::CellLocation {}, at);
    session->sendMouseReleaseEvent(Modifiers {}, MouseButton::Left, at);
    // SGR mouse reports are CSI < ... M/m; at minimum an escape must have been emitted.
    CHECK(pty.stdinBuffer().contains('\033'));
}

TEST_CASE("TerminalSession: host-writable status line permission executes headlessly",
          "[contour][session][permission]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // Display-less request is a guarded no-op (nothing to pop a dialog on).
    session->requestShowHostWritableStatusLine();

    // Deny + remember: caches the denial and leaves the status display untouched.
    session->executeShowHostWritableStatusLine(PermissionDecision::Deny, PermissionMemory::Session);
    CHECK(session->terminal().statusDisplayType() != vtbackend::StatusDisplayType::HostWritable);

    // Allow + remember: switches the status display to host-writable and unsyncs the title.
    session->executeShowHostWritableStatusLine(PermissionDecision::Allow, PermissionMemory::Session);
    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
}

TEST_CASE("TerminalSession: each guarded role remembers its decision for the session and nothing else",
          "[contour][session][permission]")
{
    using contour::session::GuardedRole;
    using contour::session::TerminalSession;
    using Executor = void (TerminalSession::*)(PermissionDecision, PermissionMemory);

    struct ExecutorRow
    {
        Executor execute;
        GuardedRole role;
    };
    auto const executors = std::array {
        ExecutorRow { .execute = &TerminalSession::applyPendingFontChange, .role = GuardedRole::ChangeFont },
        ExecutorRow { .execute = &TerminalSession::executePendingBufferCapture,
                      .role = GuardedRole::CaptureBuffer },
        ExecutorRow { .execute = &TerminalSession::executeShowHostWritableStatusLine,
                      .role = GuardedRole::ShowHostWritableStatusLine },
        ExecutorRow { .execute = &TerminalSession::applyPendingPaste, .role = GuardedRole::BigPaste },
        ExecutorRow { .execute = &TerminalSession::applyPendingRerun, .role = GuardedRole::RerunCommand },
    };

    struct Answer
    {
        PermissionDecision decision;
        PermissionMemory memory;
    };
    auto const answers = std::array {
        Answer { .decision = PermissionDecision::Allow, .memory = PermissionMemory::Once },
        Answer { .decision = PermissionDecision::Deny, .memory = PermissionMemory::Once },
        Answer { .decision = PermissionDecision::Allow, .memory = PermissionMemory::Session },
        Answer { .decision = PermissionDecision::Deny, .memory = PermissionMemory::Session },
        // A role has nowhere persistent to keep an answer: it stands for the session.
        Answer { .decision = PermissionDecision::Allow, .memory = PermissionMemory::Persistent },
    };

    TestApp testApp;
    for (auto const& row: executors)
        for (auto const& answer: answers)
        {
            INFO(std::format("{} answered {} {}", row.role, answer.decision, answer.memory));
            auto session = makeDisplaylessSession(testApp.app());
            ((*session).*row.execute)(answer.decision, answer.memory);
            if (answer.memory == PermissionMemory::Once)
                CHECK_FALSE(session->rememberedDecision(row.role).has_value());
            else
                CHECK(session->rememberedDecision(row.role) == answer.decision);
            // Only the role answered remembers anything.
            for (auto const& other: executors)
                if (other.role != row.role)
                    CHECK_FALSE(session->rememberedDecision(other.role).has_value());
        }
}

TEST_CASE("TerminalSession: font queries and pending font changes are display-safe",
          "[contour][session][font]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // Regression: getFontDef() dereferenced a null display. Display-less it answers from the
    // profile's configured fonts (the VT DECRQSS-style font query must not crash a background pane).
    auto const def = session->getFontDef();
    CHECK(def.size > 0.0);
    CHECK_FALSE(def.regular.empty());

    // setFontDef without a display cannot ask for permission: it stays pending-free, and a
    // subsequent apply (deny path, remember) is a safe no-op that still caches the answer.
    auto spec = vtbackend::FontDef {};
    spec.size = 13.0;
    session->setFontDef(spec);
    session->applyPendingFontChange(PermissionDecision::Deny, PermissionMemory::Session);
    session->applyPendingFontChange(PermissionDecision::Allow,
                                    PermissionMemory::Once); // no pending change left: early-out
    SUCCEED("font permission paths are display-safe");
}

TEST_CASE("TerminalSession: clipboard, selection and notification paths are display-safe",
          "[contour][session][guards]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    session->copyToClipboard("hello clipboard",
                             vtbackend::ClipboardWriter::Program); // display-less: guarded no-op
    session->onSelectionCompleted();                               // display-less: guarded no-op
    session->focusTerminalWindow();                                // display-less: guarded no-op
    session->discardDesktopNotification("no-such-id");

    // Multi-count paste replicates the clipboard text N times into the PTY.
    auto& pty = mockPtyOf(*session);
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        clipboard->setText(QStringLiteral("ab"));
        pty.stdinBuffer().clear();
        session->pasteFromClipboard(3, vtbackend::PasteStrip::Keep);
        CHECK(pty.stdinBuffer().contains("ababab"));
    }
}

TEST_CASE("TerminalSession: an OSC 22 pointer shape is remembered without a display attached",
          "[contour][session][pointershape]")
{
    // A pane in a background tab, or a session mid display hand-off, has no display to post the
    // cursor change to. The shape still has to be REMEMBERED, because attachDisplay() applies
    // whatever is recorded here once a display arrives -- returning early on the null display drops
    // it in precisely the case the remembering exists for, and the pane never gets the shape for the
    // rest of the session.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE(session->display() == nullptr);

    session->setPointerShape("pointer");
    CHECK(session->applicationPointerShape() == contour::input::MouseCursorShape::PointingHand);

    // Each supported CSS name maps to its own shape, display or no display.
    session->setPointerShape("text");
    CHECK(session->applicationPointerShape() == contour::input::MouseCursorShape::IBeam);
    session->setPointerShape("none");
    CHECK(session->applicationPointerShape() == contour::input::MouseCursorShape::Hidden);
    session->setPointerShape("default");
    CHECK(session->applicationPointerShape() == contour::input::MouseCursorShape::Arrow);

    // The empty name is the documented reset, and must clear the memory rather than pin the last
    // shape -- otherwise the alternate screen would never get its own default back.
    session->setPointerShape("");
    CHECK(session->applicationPointerShape() == std::nullopt);

    // A name this terminal does not implement leaves the remembered shape untouched: it is not a
    // reset, and inventing one would let an unsupported request clear a supported one.
    session->setPointerShape("pointer");
    session->setPointerShape("zoom-in");
    CHECK(session->applicationPointerShape() == contour::input::MouseCursorShape::PointingHand);
}

TEST_CASE("TerminalSession: desktop notification wiring is display-safe and reports back over the PTY",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const raised = notifier.get();
    auto session = makeDisplaylessSession(testApp.app(), std::move(notifier));

    // A notification goes through the session's notifier on every platform, and never also to the
    // chrome on screen: shown twice is a bug the user sees.
    auto shown = std::vector<std::pair<QString, QString>> {};
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::showNotification,
                     [&](QString const& title, QString const& body) { shown.emplace_back(title, body); });

    // A plain notification is passed on as it was parsed, once the GUI thread has seen where the pane is.
    auto plain = vtbackend::DesktopNotification {};
    plain.identifier = "n1";
    plain.title = "Build finished";
    plain.body = "OK";
    session->showDesktopNotification(plain);
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

    REQUIRE(raised->raised.size() == 1);
    CHECK(raised->raised[0].identifier == "n1");
    CHECK(raised->raised[0].title == "Build finished");
    CHECK(raised->raised[0].body == "OK");
    CHECK(shown.empty());

    // A notification requesting close/activation/focus reporting wires the notifier's signals; the
    // handlers reply over the PTY. Playing the notifier's events back drives those single-shot
    // handlers, which is the only way these OSC 99 replies are reachable without a desktop.
    auto reporting = vtbackend::DesktopNotification {};
    reporting.identifier = "n2";
    reporting.title = "Job";
    reporting.body = "done";
    reporting.closeEventRequested = true;
    reporting.reportOnActivation = true;
    reporting.focusOnActivation = true;
    session->showDesktopNotification(reporting);
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

    SECTION("discarding a notification asks the notifier to withdraw it")
    {
        // Withdrawing goes through the notifier everywhere; one that cannot withdraw does nothing.
        session->discardDesktopNotification("n2");
        QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

        REQUIRE(raised->closed.size() == 1);
        CHECK(raised->closed[0] == "n2");
    }

    // The OSC 99 close and activation replies are wired on every platform, and are sent wherever the
    // notifier reports back.
    REQUIRE(raised->raised.size() == 2);

    auto& pty = contour::test::mockPtyOf(*session);

    // Terminal::reply() queues into the input generator; flushInput() is what puts the bytes on the
    // PTY, so the assertion is that they reach the shell and not merely that a flag flipped.
    auto const writtenAfter = [&](auto&& fire) {
        pty.stdinBuffer().clear();
        fire();
        session->terminal().flushInput();
        return pty.stdinBuffer();
    };

    SECTION("a close event is reported back as p=close")
    {
        auto const written = writtenAfter([&] { raised->fireClosed("n2", /*reason*/ 2); });

        CHECK(written.contains("\033]99;i=n2:p=close;\033\\"));
    }

    SECTION("a close the backend only assumed is reported back as untracked")
    {
        // What a sandboxed Contour produces: org.freedesktop.portal.Notification has no close
        // signal at all, so the close is a timer expiring rather than something observed. Saying
        // so is the difference between an honest report and claiming to have watched it happen.
        // @see issue #2074.
        auto const written =
            writtenAfter([&] { raised->fireClosed("n2", /*reason*/ 1, vtbackend::CloseReport::Untracked); });

        CHECK(written.contains("\033]99;i=n2:p=close;untracked\033\\"));
    }

    SECTION("an activation is reported back as p=activated")
    {
        auto const written = writtenAfter([&] { raised->fireActivated("n2"); });

        CHECK(written.contains("\033]99;i=n2:p=activated;"));
    }

    SECTION("someone else's notification is not reported back")
    {
        auto const written = writtenAfter([&] {
            raised->fireClosed("not-ours", 2);
            raised->fireActivated("not-ours");
        });

        CHECK_FALSE(written.contains(":p=close;"));
        CHECK_FALSE(written.contains(":p=activated;"));
    }

    SECTION("another notification's events do not retire this one's reporting")
    {
        // Several notifications are live at once routinely -- OSC 99 identifies them precisely so
        // an application can have more than one. A handler wired with Qt::SingleShotConnection
        // would be broken by the FIRST emission whatever it was about, so the events below would
        // silently swallow n2's, and an application waiting on `c=1` would wait forever.
        (void) writtenAfter([&] {
            raised->fireClosed("n1", 2);
            raised->fireActivated("n1");
        });

        auto const written = writtenAfter([&] {
            raised->fireClosed("n2", 2);
            raised->fireActivated("n2");
        });

        CHECK(written.contains("\033]99;i=n2:p=close;\033\\"));
        CHECK(written.contains("\033]99;i=n2:p=activated;"));
    }

    SECTION("a notification is reported on once, not once per event")
    {
        auto const written = writtenAfter([&] {
            raised->fireClosed("n2", 2);
            raised->fireClosed("n2", 2);
            raised->fireActivated("n2");
            raised->fireActivated("n2");
        });

        CHECK(written.find("p=close;") == written.rfind("p=close;"));
        CHECK(written.find("p=activated;") == written.rfind("p=activated;"));
    }
}

TEST_CASE("TerminalSession: openDocument resolves URLs and local paths without crashing",
          "[contour][session][document]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // A scheme-qualified URL is opened as-is; an existing local path is resolved to a file:// URL;
    // a non-existent bare string stays a relative URL. QDesktopServices::openUrl may fail headlessly
    // (no handler) — the point is that every branch runs and the failure is only logged, not fatal.
    CHECK_NOTHROW(session->openDocument("https://contour-terminal.org"));
    CHECK_NOTHROW(session->openDocument("/")); // an existing local path (root dir)
    CHECK_NOTHROW(session->openDocument("this-file-does-not-exist.xyz"));
}

TEST_CASE("TerminalSession: inputModeChanged configures the cursor for each Vi mode; scroll offset "
          "change is relayed",
          "[contour][session][vimode]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    using vtbackend::ViMode;
    for (auto const mode: { ViMode::Insert,
                            ViMode::Normal,
                            ViMode::Visual,
                            ViMode::VisualLine,
                            ViMode::VisualBlock,
                            ViMode::Hint })
        CHECK_NOTHROW(session->inputModeChanged(mode));

    // onScrollOffsetChanged emits scrollOffsetChanged with the unboxed value.
    int seen = -1;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::scrollOffsetChanged,
                     [&seen](int value) { seen = value; });
    session->onScrollOffsetChanged(vtbackend::ScrollOffset(7));
    CHECK(seen == 7);
}

TEST_CASE("TerminalSession: a spontaneous early exit ends the session; typing does not close it",
          "[contour][session][close]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    auto closed = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&closed](contour::session::TerminalSession&) { ++closed; });

    // The shell "dies" moments after startup: onClosed() must keep the pane (deliberately NOT emitting
    // sessionClosed) because the default early-exit threshold is 5s -- and say so in the footer, not by
    // writing into the grid the shell's own last words are on.
    pty.close();
    session->onClosed();
    CHECK(closed == 0);
    CHECK(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
    CHECK(session->getEndMessage() == QStringLiteral("Process exited."));
    CHECK(session->endAction() == contour::session::FooterAction::Restart);
    CHECK(countOnScreen(*session, "Shell terminated too quickly.") == 0);

    // A typed character is neither an acknowledgement nor input for a shell that is gone.
    auto const written = pty.stdinBuffer();
    session->sendCharEvent(U'x',
                           vtbackend::KeyIdentity { .unshiftedKey = U'x' },
                           vtbackend::Modifiers {},
                           vtbackend::KeyboardEventType::Press,
                           std::chrono::steady_clock::now());
    CHECK(closed == 0);
    CHECK(pty.stdinBuffer() == written);

    // Nor is a key other than Return.
    session->sendKeyEvent(vtbackend::Key::Escape,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    CHECK(closed == 0);
    CHECK(pty.stdinBuffer() == written);

    // The footer's Close is.
    session->dismiss();
    CHECK(closed == 1);
}

TEST_CASE("TerminalSession: a second onClosed() reports the end once", "[contour][session][close]")
{
    // Regression (#2102, "the message about terminating too quickly appears twice"): ONE exit
    // reaches onClosed() twice -- from the session's own exit watcher, and from
    // TerminalDisplay::onBeforeSynchronize(), which fires it when the device is already closed by
    // the time the first frame is about to be drawn. That is the norm, not a corner case, for a
    // command that exits immediately (`contour terminal echo hello`). The de-duplication further
    // down onClosed() is below the early-exit branch's `return`, so only the recorded end itself can
    // say that this exit has already been reported.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);
    auto ends = QSignalSpy(session.get(), &contour::session::TerminalSession::sessionEndChanged);

    pty.close();
    session->onClosed();
    session->onClosed();

    CHECK(ends.count() == 1);
    CHECK(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::Exited));
}

TEST_CASE("TerminalSession: a lost daemon connection ends as Disconnected, with no Restart",
          "[contour][session][close]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    auto closed = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&closed](contour::session::TerminalSession&) { ++closed; });

    session->noteTransportLost();
    pty.close();
    session->onClosed();

    CHECK(closed == 0);
    CHECK(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::Disconnected));
    CHECK(session->endAction() == contour::session::FooterAction::None);
    CHECK(session->getEndMessage().startsWith(QStringLiteral("Disconnected from the Contour daemon.")));
    CHECK(countOnScreen(*session, "Disconnected from the Contour daemon.") == 0);

    // The shell runs on in the daemon, so there is nothing here to restart: Return does nothing.
    auto const written = pty.stdinBuffer();
    session->sendKeyEvent(vtbackend::Key::Enter,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    session->restart();
    CHECK(closed == 0);
    CHECK(pty.stdinBuffer() == written);
    CHECK(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::Disconnected));

    // And closing it the way a user closes any pane still works.
    session->terminate();
    CHECK(closed == 1);
}

// ============================================================================================
// Profile-driven behaviour through the profile-injection seam (a session may be constructed under a
// named profile registered in the app's config, so config-driven paths are reachable headlessly).
// ============================================================================================

TEST_CASE("TerminalSession: constructs under an injected profile name", "[contour][session][profile]")
{
    TestApp testApp;
    auto const name = registerProfile(
        testApp.app(), "alt", [](contour::config::TerminalProfile& p) { p.wmClass = "AltClass"; });
    auto session = makeSessionWithProfile(testApp.app(), name);

    // The session runs under the injected profile, not the app default.
    CHECK(session->profile().wmClass.value() == "AltClass");
}

TEST_CASE("TerminalSession: an unknown injected profile name falls back to the app default",
          "[contour][session][profile]")
{
    TestApp testApp;
    // Naming a profile that was never registered must not abort (Config::profile() asserts, but the
    // seam routes through the fallible resolveProfileName()); it silently uses the app default.
    auto session = makeSessionWithProfile(testApp.app(), "no-such-profile");
    CHECK(session->profile().wmClass.value()
          == testApp.app().config().profile(testApp.app().profileName())->wmClass.value());
}

TEST_CASE("TerminalSession: ChangeProfile switches the active profile and is a no-op for the current one",
          "[contour][session][profile][actions]")
{
    TestApp testApp;
    registerProfile(testApp.app(), "night", [](contour::config::TerminalProfile& p) {
        p.wmClass = "Night";
        p.terminalSize = vtbackend::PageSize { vtbackend::LineCount(30), vtbackend::ColumnCount(100) };
    });
    auto session = makeDisplaylessSession(testApp.app());

    // Switching to a registered profile activates it (activateProfile -> _profile replaced).
    CHECK((*session)(contour::actions::ChangeProfile { "night" }));
    CHECK(session->profile().wmClass.value() == "Night");

    // Switching to the already-active profile is a short-circuited no-op (still returns true).
    CHECK((*session)(contour::actions::ChangeProfile { "night" }));
    CHECK(session->profile().wmClass.value() == "Night");

    // Switching to an unknown profile is tolerated (activateProfile's findProfile miss) — no abort,
    // and the previously active profile stays in effect.
    CHECK((*session)(contour::actions::ChangeProfile { "ghost" }));
    CHECK(session->profile().wmClass.value() == "Night");
}

TEST_CASE("TerminalSession: HintMode merges user-configured hint patterns with the builtins",
          "[contour][session][profile][actions]")
{
    TestApp testApp;
    registerProfile(testApp.app(), "hinted", [](contour::config::TerminalProfile& p) {
        p.hintPatterns = std::vector<contour::config::HintPatternConfig> {
            { .name = "ticket", .regex = "[A-Z]+-[0-9]+" },
        };
    });
    auto session = makeSessionWithProfile(testApp.app(), "hinted");

    // HintMode compiles the user pattern (a valid regex) alongside the builtins; no display needed
    // for the pattern-merge path. It returns true once the handler is armed.
    CHECK((*session)(contour::actions::HintMode { .patterns = "ticket" }));
}

TEST_CASE("TerminalSession: HintMode tolerates an invalid user regex", "[contour][session][profile][actions]")
{
    TestApp testApp;
    registerProfile(testApp.app(), "badhint", [](contour::config::TerminalProfile& p) {
        p.hintPatterns = std::vector<contour::config::HintPatternConfig> {
            { .name = "broken", .regex = "([unclosed" },
        };
    });
    auto session = makeSessionWithProfile(testApp.app(), "badhint");

    // A malformed regex is caught per-pattern (std::regex throws) and skipped; the action still arms
    // with the builtins rather than crashing.
    CHECK_NOTHROW((*session)(contour::actions::HintMode { .patterns = "broken" }));
}

TEST_CASE("TerminalSession: FollowHyperlink reports no target when nothing is hovered",
          "[contour][session][actions]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // With no hovered hyperlink and no local path under the (absent) mouse, the action reports that
    // it did nothing.
    CHECK_FALSE((*session)(contour::actions::FollowHyperlink {}));
}

TEST_CASE("TerminalSession: desktop notification wiring runs for every report/close/focus request",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const raised = notifier.get();
    auto session = makeDisplaylessSession(testApp.app(), std::move(notifier));

    // Drive every combination of the optional report branches so all three are covered.
    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = "note-1";
    notification.title = "Title";
    notification.body = "Body";
    notification.closeEventRequested = true;
    notification.reportOnActivation = true;
    notification.focusOnActivation = true;
    session->showDesktopNotification(notification);

    // A second notification with none of the optional reports takes the plain path.
    auto plain = vtbackend::DesktopNotification {};
    plain.identifier = "note-2";
    plain.title = "Plain";
    plain.closeEventRequested = false;
    plain.reportOnActivation = false;
    plain.focusOnActivation = false;
    session->showDesktopNotification(plain);
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

    REQUIRE(raised->raised.size() == 2);
    CHECK(raised->raised[0].identifier == "note-1");
    CHECK(raised->raised[1].identifier == "note-2");

    // note-2 asked for nothing to be reported, so its events are dropped rather than replied to.
    auto& pty = contour::test::mockPtyOf(*session);
    pty.stdinBuffer().clear();
    raised->fireClosed("note-2", 2);
    raised->fireActivated("note-2");
    session->terminal().flushInput();
    CHECK_FALSE(pty.stdinBuffer().contains(":p="));

    // Withdrawing runs through the notifier on every platform.
    session->discardDesktopNotification("note-1");
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);
    REQUIRE(raised->closed.size() == 1);
    CHECK(raised->closed[0] == "note-1");
}

TEST_CASE("TerminalSession: notify() routes an OSC-9 title/body to the desktop notifier",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto notifier = std::make_unique<RecordingNotifier>();
    auto* const raised = notifier.get();
    auto session = makeDisplaylessSession(testApp.app(), std::move(notifier));

    auto shown = std::vector<std::pair<QString, QString>> {};
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::showNotification,
                     [&](QString const& title, QString const& body) { shown.emplace_back(title, body); });

    // notify() is the OSC 9 / OSC 777 entry point: it builds a DesktopNotification out of a bare
    // title and body and passes that on to the session's notifier, on every platform.
    session->notify("Build finished", "All targets are up to date");
    QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall);

    REQUIRE(raised->raised.size() == 1);
    CHECK(raised->raised[0].title == "Build finished");
    CHECK(raised->raised[0].body == "All targets are up to date");
    // Never also to the chrome on screen -- a notification shown twice is a bug the user sees.
    CHECK(shown.empty());
}

TEST_CASE("TerminalSession: onSelectionCompleted honours the configured on-mouse-selection action",
          "[contour][session][selection]")
{
    // The action is read from the app's config at session construction; drive each enum arm.
    for (auto const action: { contour::config::SelectionAction::CopyToClipboard,
                              contour::config::SelectionAction::CopyToSelectionClipboard,
                              contour::config::SelectionAction::Nothing })
    {
        TestApp testApp;
        testApp.app().config().onMouseSelection = action;
        auto session = makeDisplaylessSession(testApp.app());
        // No selection is active; the handler must run its configured branch without crashing.
        CHECK_NOTHROW(session->onSelectionCompleted());
    }
}

TEST_CASE("TerminalSession: QAbstractItemModel surface exposes the session id read-only",
          "[contour][session][model]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // The model is a trivial 1x1 table whose single cell carries the session id.
    CHECK(session->rowCount() == 1);
    CHECK(session->columnCount() == 1);

    auto const idx = session->index(0, 0);
    CHECK(idx.isValid());
    CHECK_FALSE(session->parent(idx).isValid());
    CHECK(session->data(idx).toInt() == session->id());

    // The id column is read-only: setData is rejected.
    CHECK_FALSE(session->setData(idx, QVariant(123), Qt::EditRole));
}

TEST_CASE("TerminalSession: display-coupled event overrides are safe no-ops without a display",
          "[contour][session][display]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // setTabName posts a tab-strip/status-line refresh to the session (a GUI-thread QObject) even
    // with no display attached — a background tab must still refresh its title (see the dedicated
    // real-time-title test in MultiWindow_test). setTerminalProfile early-returns when no display is
    // attached. Neither must crash.
    CHECK_NOTHROW(session->setTabName("my-tab"));
    CHECK_NOTHROW(session->setTerminalProfile("main"));
}

TEST_CASE("TerminalSession: status-line {Tabs} indicator with Indexing=title selects Title naming mode",
          "[contour][session][profile][statusline]")
{
    TestApp testApp;
    // A status-line indicator segment that references {Tabs} with Indexing=title drives the session's
    // TabsNamingMode to Title (exercising the createSettingsFromConfig indicator-parse branch).
    registerProfile(testApp.app(), "titled", [](contour::config::TerminalProfile& p) {
        // Clear the default left segment (which itself references {Tabs}) so the middle segment is the
        // one whose Indexing directive is parsed.
        p.statusLine.value().indicator.left = "";
        p.statusLine.value().indicator.middle = "{Tabs:Indexing=title}";
    });
    // Constructing under this profile runs createSettingsFromConfig, which parses the {Tabs}
    // indexing directive out of the indicator segment (the branch under test). The session's tab
    // naming mode is Title as a result.
    auto session = makeSessionWithProfile(testApp.app(), "titled");
    session->terminal().setWindowTitle("shell");
    CHECK(session->terminal().getTabsNamingMode() == vtbackend::TabsNamingMode::Title);
}

TEST_CASE("TerminalSession: HintMode merges user patterns, skips invalid regex, and filters by name",
          "[contour][session][actions]")
{
    // The HintMode handler starts from the builtin patterns, then merges the profile's user patterns:
    // a valid regex with a new name is appended, a valid regex reusing a builtin name overrides it, and
    // an invalid regex is caught and skipped (the std::regex_error branch). A named-pattern filter then
    // restricts activation to the requested set. None of this needs a display.
    contour::test::TestApp testApp;
    auto const name = registerProfile(testApp.app(), "hints", [](contour::config::TerminalProfile& p) {
        p.hintPatterns.value() = {
            contour::config::HintPatternConfig { .name = "ticket", .regex = R"([A-Z]+-\d+)" },
            contour::config::HintPatternConfig { .name = "url", .regex = R"(https?://\S+)" },
            // Deliberately broken regex: an unbalanced group triggers regex_error, exercising the
            // catch-and-skip path without aborting the merge.
            contour::config::HintPatternConfig { .name = "broken", .regex = "(unterminated" },
        };
    });
    auto session = makeSessionWithProfile(testApp.app(), name);

    session->terminal().writeToScreen("see ABC-123 and https://example.org here\r\n");

    namespace actions = contour::actions;
    // "all" keeps every (compilable) pattern; a specific name filters to it; an unknown name falls
    // back to all. Each dispatch consumes the action (returns true) and must not throw.
    CHECK((*session)(actions::HintMode { .patterns = "all" }));
    CHECK((*session)(actions::HintMode { .patterns = "ticket" }));
    CHECK((*session)(actions::HintMode { .patterns = "ticket|url" }));
    CHECK((*session)(actions::HintMode { .patterns = "no-such-pattern" }));
    CHECK((*session)(actions::HintMode { .patterns = "" }));

    // The scrollback scope reads the profile's hint_scrollback_lines and scans history as well.
    CHECK((*session)(actions::HintMode { .patterns = "all", .scope = vtbackend::HintScope::Scrollback }));
}

TEST_CASE("TerminalSession: search-match focus actions move the vi cursor onto a found match",
          "[contour][session][actions]")
{
    // FocusNextSearchMatch/FocusPreviousSearchMatch claim their chord either way: with a search they step
    // (wrapping), and with none they ask for the find bar (R-h).
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    for (int i = 0; i < 20; ++i)
        session->terminal().writeToScreen(std::format("needle row {} needle\r\n", i));

    auto barRequests = QSignalSpy { session.get(), &contour::session::TerminalSession::searchBarRequested };
    CHECK((*session)(actions::FocusNextSearchMatch {}));
    QCoreApplication::processEvents();
    CHECK(barRequests.count() == 1);

    // Set a search pattern through the terminal, then the match-focus actions have somewhere to go.
    {
        auto const l = std::scoped_lock { session->terminal() };
        std::ignore = session->terminal().setNewSearchTerm(U"needle", vtbackend::SearchOrigin::Typed);
    }
    auto const cursor = [&] {
        auto const l = std::scoped_lock { session->terminal() };
        return session->terminal().normalModeCursorPosition();
    };
    auto const before = cursor();
    CHECK((*session)(actions::FocusNextSearchMatch {}));
    CHECK(cursor() != before);
    CHECK((*session)(actions::FocusPreviousSearchMatch {}));
    QCoreApplication::processEvents();
    CHECK(barRequests.count() == 1);
}

TEST_CASE("TerminalSession: Find Next with no search opens the find bar", "[contour][session][actions]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;
    session->terminal().writeToScreen("some text\r\n");
    auto const cursor = [&] {
        auto const l = std::scoped_lock { session->terminal() };
        return session->terminal().normalModeCursorPosition();
    };
    auto const before = cursor();
    auto barRequests = QSignalSpy { session.get(), &contour::session::TerminalSession::searchBarRequested };

    CHECK((*session)(actions::FocusNextSearchMatch {}));
    QCoreApplication::processEvents();
    CHECK(barRequests.count() == 1);

    CHECK((*session)(actions::FocusPreviousSearchMatch {}));
    QCoreApplication::processEvents();
    CHECK(barRequests.count() == 2);

    // Nothing reached the application, and the cursor stayed.
    CHECK(mockPtyOf(*session).stdinBuffer().empty());
    CHECK(cursor() == before);
}

TEST_CASE("TerminalSession: Find Next wraps", "[contour][session][actions]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen("needle one\r\nhay\r\nneedle two\r\n");
    session->setSearchPattern(QStringLiteral("needle"));
    auto const first = vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = {} };
    auto const second = vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = {} };
    {
        auto const _ = std::scoped_lock { terminal };
        terminal.moveNormalModeCursorTo(second);
    }
    auto barRequests = QSignalSpy { session.get(), &contour::session::TerminalSession::searchBarRequested };

    CHECK((*session)(contour::actions::FocusNextSearchMatch {}));
    QCoreApplication::processEvents();
    {
        auto const _ = std::scoped_lock { terminal };
        CHECK(terminal.normalModeCursorPosition() == first);
    }
    // With the bar closed, stepping does not open it.
    CHECK(barRequests.count() == 0);
}

TEST_CASE("TerminalSession: Use Selection for Find takes the first line, escaped under regex",
          "[contour][session][actions]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen("foo.bar\r\nsecond line");
    session->toggleSearchOption(static_cast<int>(contour::session::SearchToggle::Regex));
    auto barRequests = QSignalSpy { session.get(), &contour::session::TerminalSession::searchBarRequested };

    SECTION("without a selection nothing changes")
    {
        CHECK_FALSE((*session)(contour::actions::SearchSelection {}));
        QCoreApplication::processEvents();
        CHECK(barRequests.count() == 0);
        auto const _ = std::scoped_lock { terminal };
        CHECK(terminal.search().pattern.empty());
    }

    SECTION("the selection's first line, escaped")
    {
        REQUIRE((*session)(contour::actions::SelectAll {}));
        CHECK((*session)(contour::actions::SearchSelection {}));
        QCoreApplication::processEvents();
        CHECK(barRequests.count() == 1);
        auto const _ = std::scoped_lock { terminal };
        CHECK(terminal.search().pattern == U"foo\\.bar");
    }
}

TEST_CASE("TerminalSession: the find bar's state follows the pattern it is given",
          "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    for (int i = 0; i < 5; ++i)
        session->terminal().writeToScreen(std::format("needle row {}\r\n", i));

    // The bar has to be open for a tally to be computed at all: walking the scrollback for a label
    // nobody is displaying is exactly the cost that guard exists to avoid. In production the bar
    // itself says so from its onOpened handler.
    session->searchBarOpened();

    SECTION("an empty pattern reports nothing at all")
    {
        session->setSearchPattern(QString {});
        CHECK(session->searchPattern().isEmpty());
        CHECK(session->searchSummary().isEmpty());
        CHECK_FALSE(session->searchNavigable());
    }

    SECTION("a matching pattern reports a match immediately, and its count once tallied")
    {
        session->setSearchPattern(QStringLiteral("needle"));
        CHECK(session->searchPattern() == QStringLiteral("needle"));

        // Known without walking the grid, so it is there the moment the key is pressed: the field's
        // tint and the step buttons do not wait for the count.
        CHECK(session->searchHasMatches());
        CHECK(session->searchNavigable());

        // The count itself is debounced, because counting walks the whole scrollback. Forcing it here
        // is what the timer does 250 ms after the user stops typing.
        session->refreshSearchStatus();
        CHECK(session->searchSummary().contains(QStringLiteral("5")));
    }

    SECTION("a pattern that matches nothing says so, and offers nowhere to step")
    {
        session->setSearchPattern(QStringLiteral("haystack"));
        CHECK_FALSE(session->searchHasMatches());
        CHECK(session->searchSummary() == QStringLiteral("No results"));
        CHECK_FALSE(session->searchNavigable());
    }

    SECTION("cycling the case policy re-runs the search, and says which way it is pinned")
    {
        session->terminal().writeToScreen("NEEDLE upper\r\n");
        session->setSearchPattern(QStringLiteral("needle"));
        auto const caseToggle = [&] {
            return session->searchToggles().at(0).toMap();
        };
        auto constexpr Case = static_cast<int>(contour::session::SearchToggle::Case);

        // Smart, and unpinned: the glyph is the capitalised one and the button is not lit.
        CHECK(caseToggle()[QStringLiteral("glyph")] == QStringLiteral("Aa"));
        CHECK_FALSE(caseToggle()[QStringLiteral("checked")].toBool());
        auto const smart = session->searchSummary();

        // Smart -> Sensitive. Still "Aa", now lit, and the uppercase line stops counting.
        session->toggleSearchOption(Case);
        CHECK(caseToggle()[QStringLiteral("glyph")] == QStringLiteral("Aa"));
        CHECK(caseToggle()[QStringLiteral("checked")].toBool());
        CHECK(session->searchSummary() != smart);

        // Sensitive -> Insensitive. THIS is the one that shows the lowercase glyph. An earlier
        // version had it the other way round, and a test asserting enumerator integers could not
        // tell -- which is why this asserts what the user actually sees.
        session->toggleSearchOption(Case);
        CHECK(caseToggle()[QStringLiteral("glyph")] == QStringLiteral("aa"));
        CHECK(caseToggle()[QStringLiteral("checked")].toBool());
        CHECK(session->searchHasMatches());

        // Insensitive -> Smart, closing the cycle.
        session->toggleSearchOption(Case);
        CHECK(caseToggle()[QStringLiteral("glyph")] == QStringLiteral("Aa"));
        CHECK_FALSE(caseToggle()[QStringLiteral("checked")].toBool());
    }

    SECTION("closing the bar stops the tallying but keeps the pattern lit for F3")
    {
        session->setSearchPattern(QStringLiteral("needle"));
        REQUIRE(session->searchHasMatches());

        session->searchBarClosed();

        // The pattern deliberately outlives the bar -- that is what F3 keeps stepping through.
        CHECK(session->searchPattern() == QStringLiteral("needle"));
        // And re-opening picks the count back up rather than showing a stale or empty one.
        session->searchBarOpened();
        CHECK(session->searchHasMatches());
        CHECK_FALSE(session->searchSummary().isEmpty());
    }

    SECTION("emptying the field drops the pattern and the summary together")
    {
        session->setSearchPattern(QStringLiteral("needle"));
        REQUIRE(session->searchHasMatches());

        // What deleting the term in the field does -- there is no separate "clear" entry point.
        session->setSearchPattern(QString {});
        CHECK(session->searchPattern().isEmpty());
        CHECK(session->searchSummary().isEmpty());
        CHECK_FALSE(session->searchNavigable());
    }

    SECTION("the tallied label carries an ordinal, because the cursor follows the match")
    {
        // The whole point of the label: "3 of 5", not "5 matches". It only works if the incremental
        // search moves the normal-mode cursor onto what it found -- the tally's ordinal, the renderer's
        // focused-match highlight and Enter's starting point all read that cursor.
        session->setSearchPattern(QStringLiteral("needle"));
        session->refreshSearchStatus();
        CHECK(session->searchSummary().contains(QStringLiteral(" of ")));
    }

    SECTION("stepping wraps at both ends")
    {
        session->setSearchPattern(QStringLiteral("needle"));
        session->refreshSearchStatus();
        REQUIRE(session->searchNavigable());

        // Walk past the last match; the bar wraps even though Terminal::searchNextMatch does not,
        // so the navigable promise holds at the ends rather than offering an inert button.
        // Stepping tallies immediately -- the user pressed a key expecting the ordinal to move.
        for (auto i = 0; i < 10; ++i)
            session->searchNext();
        CHECK(session->searchHasMatches());
        CHECK(session->searchSummary().contains(QStringLiteral(" of ")));

        for (auto i = 0; i < 10; ++i)
            session->searchPrevious();
        CHECK(session->searchSummary().contains(QStringLiteral(" of ")));
    }
}

TEST_CASE("TerminalSession: the rows a search matches are ticked, and clearing the search removes them",
          "[contour][session][scrollbar-marks][search]")
{
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    terminal.writeToScreen("needle one\r\nhay\r\nneedle two\r\nneedle and needle\r\n");
    writeFiller(terminal, 60);
    pane->setScrollbarMarkTrackPixels(2000);
    pane->refreshScrollbarMarks();
    REQUIRE(pane->scrollbarMarks().isEmpty());

    pane->searchBarOpened();
    pane->setSearchPattern(QStringLiteral("needle"));
    pane->refreshSearchStatus(); // the debounce firing: the first step runs inline
    drainSearchTally(*pane);
    pane->refreshScrollbarMarks();

    // Three rows, the last matching twice.
    CHECK(markKindsOf(*pane) == std::vector(3, ScrollbarMarkKind::SearchMatch));
    CHECK(markStyle(*pane, 0) == static_cast<int>(vtbackend::ScrollbarTickStyle::Solid));
    CHECK(markColor(*pane, ScrollbarMarkKind::SearchMatch) != markColor(*pane, ScrollbarMarkKind::Failure));

    pane->setSearchPattern(QString {});
    pane->refreshSearchStatus();
    pane->refreshScrollbarMarks();
    CHECK(pane->scrollbarMarks().isEmpty());
}

TEST_CASE("TerminalSession: the count is walked in steps and the lock is free between them",
          "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    allowHistory(*session, 3000);
    writeSearchRows(
        terminal, 2000, [](int row) { return row % 5 == 0 ? std::string("needle") : std::string("hay"); });

    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("needle"));
    session->refreshSearchStatus(); // the debounce firing: the first step runs inline
    REQUIRE(session->searchTallyRunning());
    CHECK(session->searchSummary().endsWith(QStringLiteral("…")));

    // Between two steps nothing holds the lock: output goes in, as it would from the parser thread.
    session->runSearchTallyStep();
    terminal.writeToScreen("hay\r\n");
    REQUIRE(session->searchTallyRunning());

    drainSearchTally(*session);
    auto const expected = [&] {
        auto const _ = std::scoped_lock { terminal };
        return terminal.tallySearchMatches(terminal.normalModeCursorPosition());
    }();
    REQUIRE(expected.total == 400);
    CHECK(session->searchSummary()
          == (expected.ordinal != 0 ? QStringLiteral("%1 of %2").arg(expected.ordinal).arg(expected.total)
                                    : QStringLiteral("%1 matches").arg(expected.total)));
}

TEST_CASE("TerminalSession: a status refresh mid-walk after output continues the walk",
          "[contour][session][search]")
{
    // M3 (Q4): output arriving while a count is walked re-runs the debounce, which refreshes the status; that
    // refresh must leave the walk of the same query from the same place going, or a count longer than the
    // debounce would never finish while output streams. Run twice, with and without the refresh: a walk that
    // continued needs the same steps to finish, a restarted one more.
    auto const remainingSteps = [](bool refreshAfterOutput) {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& terminal = session->terminal();
        allowHistory(*session, 3000);
        writeSearchRows(terminal, 2000, [](int row) {
            return row % 5 == 0 ? std::string("needle") : std::string("hay");
        });
        session->searchBarOpened();
        session->setSearchPattern(QStringLiteral("needle"));
        session->refreshSearchStatus();
        session->runSearchTallyStep();
        terminal.writeToScreen("hay\r\n");
        if (refreshAfterOutput)
            session->refreshSearchStatus();
        REQUIRE(session->searchTallyRunning());
        auto const steps = drainSearchTally(*session);
        CHECK(session->searchSummary().contains(QStringLiteral("400")));
        return steps;
    };
    auto const undisturbed = remainingSteps(false);
    REQUIRE(undisturbed > 0);
    CHECK(remainingSteps(true) == undisturbed);
}

TEST_CASE("TerminalSession: an invalid regex says so in the count slot", "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("a(b\r\n");
    session->searchBarOpened();

    session->toggleSearchOption(static_cast<int>(contour::session::SearchToggle::Regex));
    session->setSearchPattern(QStringLiteral("a("));
    session->refreshSearchStatus();
    CHECK(session->searchSummary() == QStringLiteral("Invalid pattern"));
    CHECK_FALSE(session->searchHasMatches());
    CHECK_FALSE(session->searchNavigable());
    CHECK_FALSE(session->searchTallyRunning());

    session->setSearchPattern(QStringLiteral("a"));
    session->refreshSearchStatus();
    CHECK(session->searchHasMatches());
    CHECK(session->searchSummary().contains(QStringLiteral(" of ")));
}

TEST_CASE("TerminalSession: a catastrophic pattern ends in a word, not a hang", "[contour][session][search]")
{
    auto constexpr Rows = 10000;
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    allowHistory(*session, Rows + 100);
    writeSearchRows(session->terminal(), Rows, [](int) { return std::string(60, 'a') + "b"; });
    session->searchBarOpened();
    session->toggleSearchOption(static_cast<int>(contour::session::SearchToggle::Regex));

    session->setSearchPattern(QStringLiteral("(a+)+$"));
    session->refreshSearchStatus();
    auto const steps = drainSearchTally(*session);
    CHECK(steps <= (Rows + 499) / 500);
#ifdef __GLIBCXX__
    CHECK(session->searchSummary() == QStringLiteral("No results"));
#else
    CHECK(session->searchSummary() == QStringLiteral("Pattern too complex"));
#endif
    CHECK_FALSE(session->searchHasMatches());
}

TEST_CASE("TerminalSession: a match far above the cursor is reached after the bounded probe",
          "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    allowHistory(*session, 3000);
    terminal.writeToScreen("needle\r\n");
    writeSearchRows(terminal, 2000, [](int) { return std::string("hay"); });
    session->searchBarOpened();
    auto const cursorBefore = [&] {
        auto const _ = std::scoped_lock { terminal };
        return terminal.normalModeCursorPosition();
    }();

    session->setSearchPattern(QStringLiteral("needle"));
    // Beyond the probe's reach: not known yet, which is neither red nor navigable.
    CHECK(session->searchHasMatches());
    CHECK_FALSE(session->searchNavigable());
    {
        auto const _ = std::scoped_lock { terminal };
        CHECK(terminal.normalModeCursorPosition() == cursorBefore);
    }

    session->refreshSearchStatus();
    drainSearchTally(*session);
    auto const _ = std::scoped_lock { terminal };
    auto const top = -boxed_cast<vtbackend::LineOffset>(terminal.primaryScreen().historyLineCount());
    CHECK(terminal.normalModeCursorPosition() == vtbackend::CellLocation { .line = top, .column = {} });
    CHECK(session->searchSummary() == QStringLiteral("1 of 1"));
}

TEST_CASE("TerminalSession: toggles cycle and re-run the search", "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("foo foobar\r\n");
    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("foo"));
    session->refreshSearchStatus();

    auto const toggles = session->searchToggles();
    REQUIRE(std::cmp_equal(toggles.size(), contour::session::SearchToggleRows.size()));
    for (auto const& row: contour::session::SearchToggleRows)
    {
        auto const map = toggles.at(static_cast<int>(row.toggle)).toMap();
        CHECK(map[QStringLiteral("objectName")]
              == QString::fromUtf8(row.objectName.data(), static_cast<qsizetype>(row.objectName.size())));
        CHECK(map[QStringLiteral("toggle")].toInt() == static_cast<int>(row.toggle));
        CHECK_FALSE(map[QStringLiteral("accessibleName")].toString().isEmpty());
        CHECK_FALSE(map[QStringLiteral("tooltip")].toString().isEmpty());
    }
    CHECK(session->searchSummary().contains(QStringLiteral("of 2")));

    auto constexpr WholeWord = static_cast<int>(contour::session::SearchToggle::WholeWord);
    session->toggleSearchOption(WholeWord);
    {
        auto const _ = std::scoped_lock { session->terminal() };
        CHECK(session->terminal().search().options.wordBoundary == vtbackend::SearchWordBoundary::WholeWord);
    }
    CHECK(session->searchToggles().at(WholeWord).toMap()[QStringLiteral("checked")].toBool());
    CHECK(session->searchSummary().contains(QStringLiteral("of 1")));

    auto const before = session->searchToggles();
    session->toggleSearchOption(99);
    session->toggleSearchOption(-1);
    CHECK(session->searchToggles() == before);
}

TEST_CASE("TerminalSession: searchFocusedRows names the current match's rows", "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto const columns = unbox<int>(terminal.pageSize().columns);
    // Rows 0-2 of hay, then "needle" wrapped over rows 3 and 4.
    terminal.writeToScreen("hay\r\nhay\r\nhay\r\n" + std::string(static_cast<size_t>(columns - 3), 'x')
                           + "needle");
    session->searchBarOpened();
    auto rowsChanged =
        QSignalSpy { session.get(), &contour::session::TerminalSession::searchFocusedRowsChanged };

    session->setSearchPattern(QStringLiteral("needle"));
    auto const rows = session->searchFocusedRows();
    CHECK(rows[QStringLiteral("first")].toInt() == 3);
    CHECK(rows[QStringLiteral("last")].toInt() == 4);
    CHECK(rowsChanged.count() == 1);

    // Scrolled off screen: no rows, and said so.
    allowHistory(*session, 1000);
    writeSearchRows(terminal, 100, [](int) { return std::string("hay"); });
    session->refreshSearchFocusedRows();
    CHECK(session->searchFocusedRows().isEmpty());
    CHECK(rowsChanged.count() == 2);
}

TEST_CASE("TerminalSession: searchFocusedRows counts the rows a collapsed fold above the match hides",
          "[contour][session][search][folding]")
{
    // The rows are the ones the user sees: a folded command's output above the match takes no rows, so the
    // match shows that many rows higher than its grid line says.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("ls", numberedOutput(5)));
    terminal.writeToScreen("needle\r\n");
    REQUIRE(terminal.foldRanges().size() == 1);
    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("needle"));
    auto const unfolded = session->searchFocusedRows()[QStringLiteral("first")].toInt();

    terminal.foldState().collapse(terminal.foldRanges().front().headStableId);
    REQUIRE(terminal.hiddenLineCount() > vtbackend::LineCount(0));
    session->refreshSearchFocusedRows();

    auto const rows = session->searchFocusedRows();
    REQUIRE_FALSE(rows.isEmpty());
    CHECK(rows[QStringLiteral("first")].toInt() == unfolded - unbox<int>(terminal.hiddenLineCount()));
    CHECK(rows[QStringLiteral("last")].toInt() == rows[QStringLiteral("first")].toInt());
}

TEST_CASE("TerminalSession: output scrolling a match from another thread refreshes its rows",
          "[contour][session][search][threading]")
{
    // The scroll callback runs on the scrolling thread with the state mutex held; the rows are re-read on
    // the GUI thread, from a posted call, never under that callback.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    allowHistory(*session, 1000);
    writeSearchRows(
        terminal, 50, [](int row) { return row == 45 ? std::string("needle") : std::string("hay"); });
    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("needle"));
    auto const before = session->searchFocusedRows();
    REQUIRE_FALSE(before.isEmpty());
    auto rowsChanged =
        QSignalSpy { session.get(), &contour::session::TerminalSession::searchFocusedRowsChanged };

    auto scroller = std::thread { [&terminal] {
        auto const _ = std::scoped_lock { terminal };
        std::ignore = terminal.viewport().scrollUp(vtbackend::LineCount(3));
    } };
    scroller.join();
    QCoreApplication::processEvents();

    CHECK(rowsChanged.count() == 1);
    auto const rows = session->searchFocusedRows();
    CHECK(rows[QStringLiteral("first")].toInt() == before[QStringLiteral("first")].toInt() + 3);
    auto const expected = [&] {
        auto const _ = std::scoped_lock { terminal };
        return unbox<int>(terminal.focusedSearchMatch()->first.line)
               + unbox<int>(terminal.viewport().scrollOffset());
    }();
    CHECK(rows[QStringLiteral("first")].toInt() == expected);
}

TEST_CASE("TerminalSession: closing the find bar drops the jump its walk was to make",
          "[contour][session][search]")
{
    // B-M7: the probe found nothing in its reach, so the walk was to move the cursor once it ended. Closing
    // the bar ends the walk, and the jump with it: reopening must not move a cursor nobody asked to move.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    allowHistory(*session, 3000);
    terminal.writeToScreen("needle\r\n");
    writeSearchRows(terminal, 2000, [](int) { return std::string("hay"); });
    auto const cursor = [&] {
        auto const _ = std::scoped_lock { terminal };
        return terminal.normalModeCursorPosition();
    };

    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("needle"));
    auto const before = cursor();
    session->searchBarClosed();

    session->searchBarOpened();
    drainSearchTally(*session);
    CHECK(cursor() == before);
    CHECK_FALSE(session->searchSummary().contains(QStringLiteral(" of ")));
}

TEST_CASE("TerminalSession: a pattern too complex to draw says so in the count slot",
          "[contour][session][search]")
{
    // B-I3: the query ends where a frame meets it, not in the probe or the walk -- the page on display
    // holds the catastrophic rows below the cursor, the probe reads only up from it, and no count runs. The
    // terminal's searchQueryEnded event is what tells the bar.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    allowHistory(*session, 3000);
    writeSearchRows(terminal, 2000, [](int) { return std::string("hay"); });
    writeSearchRows(terminal, 10, [](int) { return std::string(60, 'a') + "b"; });
    // Opening the bar from Insert mode puts the vi cursor on the terminal's; it is moved after.
    session->searchBarOpened();
    session->toggleSearchOption(static_cast<int>(contour::session::SearchToggle::Regex));
    {
        // The page's top row is hay; the probe reads up from it, the frame reads the rows below it too.
        auto const _ = std::scoped_lock { terminal };
        REQUIRE(terminal.primaryScreen().grid().lineText(vtbackend::LineOffset(0)).starts_with("hay"));
        terminal.moveNormalModeCursorTo(vtbackend::CellLocation {});
    }
    session->setSearchPattern(QStringLiteral("(a+)+$"));
    REQUIRE(session->searchSummary() != QStringLiteral("Pattern too complex"));
    REQUIRE_FALSE(session->searchTallyRunning());

    terminal.refreshRenderBuffer(false);
#ifdef __GLIBCXX__
    // libstdc++'s breadth-first executor matches it, if slowly: nothing ends.
    CHECK_FALSE(terminal.search().problem.has_value());
#else
    REQUIRE(terminal.search().problem == vtbackend::SearchPatternError::TooComplex);
    QCoreApplication::processEvents();
    CHECK(session->searchSummary() == QStringLiteral("Pattern too complex"));
    CHECK_FALSE(session->searchHasMatches());
#endif
}

TEST_CASE("TerminalSession: the find bar wraps to the top of the page on display",
          "[contour][session][search]")
{
    // DECPCCM reset: page 1 stays on display, its history holding the only two matches, while the cursor
    // writes to page 3, which holds none. Stepping on from the last match wraps to the first, at the top of
    // page 1's history.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen("needle one\r\n");
    for (auto const i: std::views::iota(0, 10))
        terminal.writeToScreen(std::format("hay {}\r\n", i));
    terminal.writeToScreen("needle two\r\n");
    for (auto const i: std::views::iota(0, 40))
        terminal.writeToScreen(std::format("more hay {}\r\n", i));
    terminal.writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, then the cursor to page 3
    REQUIRE(terminal.displayedPageIndex() == vtbackend::PageIndex(0));
    auto const top = -boxed_cast<vtbackend::LineOffset>(terminal.primaryScreen().historyLineCount());
    auto const first = vtbackend::CellLocation { .line = top, .column = vtbackend::ColumnOffset(0) };
    auto const last = vtbackend::CellLocation { .line = top + vtbackend::LineOffset(11),
                                                .column = vtbackend::ColumnOffset(0) };

    session->searchBarOpened();
    session->setSearchPattern(QStringLiteral("needle"));
    {
        auto const _ = std::scoped_lock { terminal };
        terminal.moveNormalModeCursorTo(last);
    }
    session->searchNext();

    auto const _ = std::scoped_lock { terminal };
    CHECK(terminal.normalModeCursorPosition() == first);
}

TEST_CASE("TerminalSession: SearchReverse asks for the find bar without switching Vi mode",
          "[contour][session][search]")
{
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    auto requests = 0;
    QObject::connect(
        session.get(), &contour::session::TerminalSession::searchBarRequested, [&]() { ++requests; });

    auto const modeBefore = session->terminal().inputHandler().mode();
    CHECK((*session)(actions::SearchReverse {}));
    QCoreApplication::processEvents();

    CHECK(requests == 1);
    // The old prompt forced Normal mode purely to reveal the status line carrying it. Nothing is
    // revealed any more, so nothing may be switched.
    CHECK(session->terminal().inputHandler().mode() == modeBefore);
}

TEST_CASE("TerminalSession: open and paste-shell actions run headlessly without a display",
          "[contour][session][actions]")
{
    // OpenConfiguration{in_editor}/OpenFileManager/OpenSelection route through QDesktopServices; under
    // the offscreen platform these no-op (no handler) but must still run and return true. PasteSelection
    // with evaluate_in_shell sends the (empty) selection as raw input. None touch the display.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    CHECK((*session)(actions::OpenConfiguration { .target = actions::ConfigurationTarget::Editor }));
    CHECK((*session)(actions::OpenFileManager {}));
    CHECK((*session)(actions::OpenSelection {}));
    CHECK((*session)(actions::PasteSelection { .evaluateInShell = true }));
}

TEST_CASE("TerminalSession: updateColorPreference swaps palettes for a dual-color profile",
          "[contour][session]")
{
    // A dual (dark/light) color config resolves preferredColorPalette by preference: switching the
    // preference away from the current one re-applies the matching palette and emits the change. A
    // no-op call with the same preference returns early.
    contour::test::TestApp testApp;
    auto const name = registerProfile(testApp.app(), "dual", [](contour::config::TerminalProfile& p) {
        auto dual = contour::config::DualColorConfig {};
        dual.darkMode.defaultBackground = vtbackend::RGBColor { 0x101010 };
        dual.lightMode.defaultBackground = vtbackend::RGBColor { 0xF0F0F0 };
        p.colors.value() = dual;
    });
    auto session = makeSessionWithProfile(testApp.app(), name);

    // Flip to the opposite of the current preference so the swap branch runs, then a redundant call
    // to exercise the early-return.
    session->updateColorPreference(vtbackend::ColorPreference::Light);
    session->updateColorPreference(vtbackend::ColorPreference::Light); // early-return (unchanged)
    session->updateColorPreference(vtbackend::ColorPreference::Dark);
    SUCCEED("dual-palette preference switches applied without crashing");
}

TEST_CASE("TerminalSession: accumulated angle scroll consumes into line/column steps", "[contour][session]")
{
    // The angle-scroll accumulation path is display-independent (the pixel path needs cell metrics
    // from a display). Feeding an angle-only delta clears any pixel accumulation, and consumeScroll
    // converts a large-enough angle accumulation into a non-zero line step.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // angle-only delta with no pixel delta takes the "reset pixel accumulation" branch.
    session->addToAccumulatedScroll(crispy::Point { .x = 0, .y = 0 },
                                    crispy::Point { .x = 0, .y = 8 * 5 * 3 },
                                    vtbackend::ScrollPhase::NoPhase,
                                    false);
    auto const [lines, columns] = session->consumeScroll();
    CHECK(lines.value != 0);
    CHECK(columns.value == 0);
}

TEST_CASE("TerminalSession: sideways drift of a vertical scroll never becomes a column step",
          "[contour][session][wheel]")
{
    // End-to-end for the drift guard: the horizontal component is dropped at accumulation, so it can
    // never reach the WheelLeft/WheelRight fallback binding no matter how long the scroll runs.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    auto constexpr Step = 8 * 5; // one angle notch, per consumeScroll()

    session->addToAccumulatedScroll(
        crispy::Point {}, crispy::Point { .x = 0, .y = -Step }, vtbackend::ScrollPhase::Begin, false);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
        session->addToAccumulatedScroll(
            crispy::Point {}, crispy::Point { .x = Step, .y = -1 }, vtbackend::ScrollPhase::Update, false);

    auto const [lines, columns] = session->consumeScroll();
    CHECK(columns.value == 0);
    CHECK(lines.value != 0);
}

TEST_CASE("TerminalSession: a deliberate sideways swipe does produce column steps",
          "[contour][session][wheel]")
{
    // The counterpart to the drift guard: a gesture that is horizontal from the outset must still get
    // through, or the feature would never fire at all.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    auto constexpr Step = 8 * 5;

    session->addToAccumulatedScroll(
        crispy::Point {}, crispy::Point { .x = Step, .y = 0 }, vtbackend::ScrollPhase::Begin, false);
    session->addToAccumulatedScroll(
        crispy::Point {}, crispy::Point { .x = Step, .y = 0 }, vtbackend::ScrollPhase::Update, false);

    auto const [lines, columns] = session->consumeScroll();
    CHECK(columns.value != 0);
    CHECK(lines.value == 0);
}

TEST_CASE("TerminalSession: openDocument routes through the injected external launcher",
          "[contour][session][launcher]")
{
    // This machine's filesystem: the document is a real file, in a temporary directory.
    contour::test::TestApp testApp { contour::test::TestApp::SessionFiles::ThisMachine };
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    // A URL with an explicit scheme is opened verbatim (no filesystem probe).
    session->openDocument("https://contour-terminal.org/");
    REQUIRE(launcher.openedUrls.size() == 1);
    CHECK(launcher.openedUrls.back().toString().toStdString() == "https://contour-terminal.org/");

    // A scheme-less path that EXISTS is resolved to an absolute file:// URL.
    QTemporaryDir const dir;
    auto const filePath = std::filesystem::path(dir.path().toStdU16String()) / "doc.txt";
    {
        std::ofstream(filePath) << "x";
    }
    session->openDocument(filePath.string());
    REQUIRE(launcher.openedUrls.size() == 2);
    CHECK(launcher.openedUrls.back().isLocalFile());
    // QUrl::toLocalFile() always yields '/'-separated paths, so compare against the generic form
    // (native std::filesystem separators are '\' on Windows).
    CHECK(launcher.openedUrls.back().toLocalFile().toStdString() == filePath.generic_string());

    // A scheme-less path that does NOT exist is passed through as-is (a relative QUrl).
    session->openDocument("no-such-file.xyz");
    REQUIRE(launcher.openedUrls.size() == 3);
    CHECK_FALSE(launcher.openedUrls.back().isLocalFile());
}

TEST_CASE("TerminalSession: a pre-allowed change_font applies without asking the user",
          "[contour][session][permission]")
{
    // A profile that pre-allows ChangeFont drives requestPermission's Allow branch straight to
    // executeRole — no permission dialog signal is emitted.
    contour::test::TestApp testApp;
    auto const profileName = registerProfile(testApp.app(), "perm", [](contour::config::TerminalProfile& p) {
        p.permissions.value().changeFont = contour::config::Permission::Allow;
    });
    auto session = makeSessionWithProfile(testApp.app(), profileName);
    namespace actions = contour::actions;

    CHECK_NOTHROW((*session)(actions::ResetFontSize {}));
}

TEST_CASE("TerminalSession: the configured capture_buffer permission decides the request",
          "[contour][session][permission]")
{
    // The regression behind #2089: requestCaptureBuffer() emitted the dialog signal directly instead of
    // going through requestPermission(), so neither the configured permission nor the remembered answer
    // was ever consulted. These drive requestCaptureBuffer() — the function that skipped the gate —
    // rather than executePendingBufferCapture(), which is the executor *after* it.
    contour::test::TestApp testApp;
    auto asks = 0;

    SECTION("deny refuses the request without asking, but still terminates the reply")
    {
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Deny);
        countCaptureAsks(*held, asks);

        captureWholePage(*held);

        CHECK(asks == 0);
        // Refused, so the screen must not be on the wire — but the empty terminating chunk must be, or a
        // client reading until it blocks forever. @see docs/vt-extensions/buffer-capture.md
        CHECK_FALSE(mockPtyOf(*held).stdinBuffer().contains(SeededLine));
        CHECK(mockPtyOf(*held).stdinBuffer() == CaptureReplyTerminator);
    }

    SECTION("allow captures without asking")
    {
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Allow);
        countCaptureAsks(*held, asks);

        captureWholePage(*held);

        CHECK(asks == 0);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("ask consults the answer the user asked to be remembered")
    {
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
        countCaptureAsks(*held, asks);

        captureWholePage(*held);
        REQUIRE(asks == 1);
        CHECK(mockPtyOf(*held).stdinBuffer().empty()); // nothing happens until the user answers

        // "Yes to all": the answer is stored, and the capture runs.
        held->executePendingBufferCapture(PermissionDecision::Allow, PermissionMemory::Session);
        CHECK(capturedSeededScreen(*held));

        // The second request must resolve from that memory instead of asking again.
        mockPtyOf(*held).stdinBuffer().clear();
        captureWholePage(*held);
        CHECK(asks == 1);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("ask remembers a refusal too, and keeps answering the client")
    {
        // The other half of what the changelog promises: "No to all" has to stick for the session the
        // same way "Yes to all" does, and a remembered refusal still owes each request its terminator.
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
        countCaptureAsks(*held, asks);

        captureWholePage(*held);
        REQUIRE(asks == 1);

        held->executePendingBufferCapture(PermissionDecision::Deny, PermissionMemory::Session);
        CHECK(mockPtyOf(*held).stdinBuffer() == CaptureReplyTerminator);

        mockPtyOf(*held).stdinBuffer().clear();
        captureWholePage(*held);
        CHECK(asks == 1); // remembered: no second dialog
        CHECK_FALSE(mockPtyOf(*held).stdinBuffer().contains(SeededLine));
        CHECK(mockPtyOf(*held).stdinBuffer() == CaptureReplyTerminator);
    }

    SECTION("the sequence itself resolves, arriving under the terminal's lock")
    {
        // The cases above call the Events hook directly, on the test thread. Production reaches it from
        // parseFragmentChunked(), with writeToScreen() holding the terminal's NON-RECURSIVE state mutex
        // — so anything on this path that takes that mutex inline deadlocks the parser thread. Driving
        // the real sequence is what pins that; posts are held back because the queue a display would
        // hand this to is exactly what makes the locked work safe.
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Allow);
        held.surface->runPostsImmediately = false;
        countCaptureAsks(*held, asks);

        held->terminal().writeToScreen(CaptureWholePageSequence);

        REQUIRE(held.surface->pendingPosts.size() == 1);
        held.surface->drainPosts();
        CHECK(asks == 0);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("every request queued before the user answers gets its own reply")
    {
        // A single pending slot dropped whichever request arrived while another was outstanding, and a
        // dropped request is a client blocked forever — the protocol cannot say "no reply is coming".
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
        countCaptureAsks(*held, asks);

        captureWholePage(*held);
        captureWholePage(*held);

        // One verdict answers both, and both replies are on the wire.
        held->executePendingBufferCapture(PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(countOccurrences(mockPtyOf(*held).stdinBuffer(), CaptureReplyTerminator) == 2);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("the request is handed to the display's queue, not run where it arrives")
    {
        // requestCaptureBuffer() is a Terminal::Events hook, so it arrives on the terminal thread, while
        // the Allow branch reaches captureBuffer() and flushInput() — GUI-thread work. Holding the posts
        // back shows the hop is load-bearing rather than incidental.
        auto held = sessionCapturing(testApp.app(), contour::config::Permission::Allow);
        countCaptureAsks(*held, asks);
        held.surface->runPostsImmediately = false;

        captureWholePage(*held);

        CHECK(held.surface->pendingPosts.size() == 1);
        CHECK(mockPtyOf(*held).stdinBuffer().empty());

        held.surface->drainPosts();
        CHECK(asks == 0);
        CHECK(capturedSeededScreen(*held));
    }
}

TEST_CASE("TerminalSession: two questions asked at once are shown one after the other",
          "[contour][session][permission][queue]")
{
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;

    TestApp testApp;
    auto const name = registerProfile(testApp.app(), "ask-both", [](contour::config::TerminalProfile& p) {
        p.permissions.value().changeFont = contour::config::Permission::Ask;
        p.permissions.value().captureBuffer = contour::config::Permission::Ask;
    });
    auto held = makeSessionWithSurface(testApp.app(), name);
    held->terminal().writeToScreen(std::format("{}\r\n", SeededLine));
    mockPtyOf(*held).stdinBuffer().clear();
    auto changes = 0;
    QObject::connect(held.session.get(),
                     &contour::session::TerminalSession::pendingPermissionChanged,
                     [&changes] { ++changes; });

    // A program changes the font and asks for a capture in the same chunk; the capture asks twice.
    held->setFontDef(requestedFont());
    captureWholePage(*held);
    captureWholePage(*held);
    QCoreApplication::processEvents();

    CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::ChangeFont });
    CHECK(changes == 1);
    CHECK(
        held->pendingPermission().value(QStringLiteral("title")).toString().endsWith("to change the font?"));

    // The answer to the font is the font's alone: the capture is asked next, once this dialog has closed.
    answerShown(*held, PermissionDecision::Deny, PermissionMemory::Once);
    CHECK(held.surface->fontsSet.empty());
    CHECK(mockPtyOf(*held).stdinBuffer().empty());
    QCoreApplication::processEvents();
    CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    CHECK(
        held->pendingPermission().value(QStringLiteral("title")).toString().endsWith("to read the screen?"));

    // One answer serves both captures, each owed its reply.
    answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(capturedSeededScreen(*held));
    CHECK(countOccurrences(mockPtyOf(*held).stdinBuffer(), CaptureReplyTerminator) == 2);
    CHECK(held.surface->fontsSet.empty());
    QCoreApplication::processEvents();
    CHECK_FALSE(held->pendingPermissionAsk().has_value());
    CHECK(held->pendingPermission().isEmpty());
}

TEST_CASE("TerminalSession: an answer counts only for the question it answers",
          "[contour][session][permission][queue]")
{
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;

    TestApp testApp;
    auto const name = registerProfile(testApp.app(), "ask-twice", [](contour::config::TerminalProfile& p) {
        p.permissions.value().changeFont = contour::config::Permission::Ask;
        p.permissions.value().captureBuffer = contour::config::Permission::Ask;
    });
    auto held = makeSessionWithSurface(testApp.app(), name);
    mockPtyOf(*held).stdinBuffer().clear();
    held->setFontDef(requestedFont());
    captureWholePage(*held);
    QCoreApplication::processEvents();
    REQUIRE(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::ChangeFont });
    auto const fontQuestion = shownQuestionId(*held);

    SECTION("the same answer given twice (No, then the dialog's rejected) answers one question")
    {
        held->answerPermission(fontQuestion, PermissionDecision::Deny, PermissionMemory::Once);
        held->answerPermission(fontQuestion, PermissionDecision::Deny, PermissionMemory::Once);
        QCoreApplication::processEvents();
        CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
        CHECK(mockPtyOf(*held).stdinBuffer().empty()); // the capture was not refused unseen
        CHECK(shownQuestionId(*held) != fontQuestion);
    }

    SECTION("an id that is not the shown question's is ignored")
    {
        held->answerPermission(fontQuestion + 1, PermissionDecision::Allow, PermissionMemory::Session);
        CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::ChangeFont });
        CHECK(held.surface->fontsSet.empty());
        CHECK_FALSE(held->rememberedDecision(GuardedRole::ChangeFont).has_value());
    }

    SECTION("a question settled by something else takes its id with it")
    {
        // The font change is answered without the dialog -- a reload, a remembered answer: the dialog's
        // late click on it must not answer the capture now waiting.
        held->applyPendingFontChange(PermissionDecision::Deny, PermissionMemory::Once);
        held->answerPermission(fontQuestion, PermissionDecision::Allow, PermissionMemory::Once);
        QCoreApplication::processEvents();
        CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
        CHECK(mockPtyOf(*held).stdinBuffer().empty());
    }
}

TEST_CASE("TerminalSession: a question asked again with more to it is another question",
          "[contour][session][permission][queue]")
{
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;

    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    REQUIRE(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    auto const shown = shownQuestionId(*held);

    // The program asks for more before the user clicks: the click on what was shown grants nothing.
    held->requestCaptureBuffer(held->terminal().pageSize().lines, vtbackend::CaptureLineMode::Physical);
    CHECK(shownQuestionId(*held) != shown);
    held->answerPermission(shown, PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    CHECK(mockPtyOf(*held).stdinBuffer().empty());

    // The same question asked again, unchanged, keeps its id.
    auto const grown = shownQuestionId(*held);
    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    CHECK(shownQuestionId(*held) == grown);

    // Answered with what is shown now -- once Allow is given back after the re-presentation -- it is.
    testApp.steadyClock().advance(contour::session::RepresentedAllowHold);
    REQUIRE(QTest::qWaitFor(
        [&] {
            return !held->pendingPermission()
                        .value(QString::fromUtf8(contour::session::AllowHeldKey))
                        .toBool();
        },
        2000));
    answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
    CHECK(capturedSeededScreen(*held));
}

TEST_CASE("TerminalSession: Allow waits a moment after a question is re-presented",
          "[contour][session][permission][queue]")
{
    // A program can grow its question just before the user's click lands on Allow. For a moment after a
    // re-presentation Allow is withheld -- in what the dialog shows, and in what the session takes -- while
    // Deny and Escape stay live. The first presentation is not held.
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;
    using contour::session::RepresentedAllowHold;
    auto const allowHeld = [](contour::session::TerminalSession const& session) {
        return session.pendingPermission().value(QString::fromUtf8(contour::session::AllowHeldKey)).toBool();
    };

    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    REQUIRE(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    CHECK_FALSE(allowHeld(*held));

    held->requestCaptureBuffer(held->terminal().pageSize().lines, vtbackend::CaptureLineMode::Physical);
    CHECK(allowHeld(*held));
    auto const regrown = shownQuestionId(*held);

    SECTION("Allow is not taken while held, and is once the moment has passed")
    {
        answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
        CHECK(mockPtyOf(*held).stdinBuffer().empty());

        testApp.steadyClock().advance(RepresentedAllowHold);
        REQUIRE(QTest::qWaitFor([&] { return !allowHeld(*held); }, 2000));
        CHECK(shownQuestionId(*held) == regrown); // the same question, Allow given back
        answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("the hold ends by the clock, not by a timer that fires first")
    {
        // The clock stands still: however long the timer has waited, the hold has not run out.
        QTest::qWait(static_cast<int>(RepresentedAllowHold.count()) * 3 / 2);
        CHECK(allowHeld(*held));
        answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
        CHECK(mockPtyOf(*held).stdinBuffer().empty());

        testApp.steadyClock().advance(RepresentedAllowHold);
        REQUIRE(QTest::qWaitFor([&] { return !allowHeld(*held); }, 2000));
        answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(capturedSeededScreen(*held));
    }

    SECTION("Deny is taken at once")
    {
        answerShown(*held, PermissionDecision::Deny, PermissionMemory::Once);
        CHECK_FALSE(held->pendingPermissionAsk().has_value());
    }
}

TEST_CASE("TerminalSession: a click before a larger capture's request is in serves only what was shown",
          "[contour][session][permission][queue]")
{
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;

    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    REQUIRE(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });

    // The program asks for the whole page; its request is still on its way to the GUI thread when the user
    // clicks Allow on the one-line question shown.
    held.surface->runPostsImmediately = false;
    held->requestCaptureBuffer(held->terminal().pageSize().lines, vtbackend::CaptureLineMode::Physical);
    REQUIRE(held.surface->pendingPosts.size() == 1);
    answerShown(*held, PermissionDecision::Allow, PermissionMemory::Once);

    // One line, the one asked about: not the page, whose top line is the seeded one.
    CHECK(countOccurrences(mockPtyOf(*held).stdinBuffer(), CaptureReplyTerminator) == 1);
    CHECK_FALSE(mockPtyOf(*held).stdinBuffer().contains(SeededLine));

    // The larger request arrives, and is asked about on its own.
    held.surface->drainPosts();
    QCoreApplication::processEvents();
    CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    CHECK(held->pendingPermission()
              .value(QStringLiteral("lines"))
              .toList()
              .front()
              .toMap()
              .value(QStringLiteral("text"))
              .toString()
              .contains(QString::number(unbox(held->terminal().pageSize().lines))));
}

TEST_CASE("TerminalSession: a question re-asked with more to it is said anew",
          "[contour][session][permission]")
{
    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), contour::config::Permission::Ask);
    auto announcer = std::make_unique<contour::test::RecordingAnnouncer>();
    auto* recorder = announcer.get();
    held->setAnnouncer(std::move(announcer));

    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    REQUIRE(recorder->said.size() == 1);
    held->requestCaptureBuffer(held->terminal().pageSize().lines, vtbackend::CaptureLineMode::Physical);
    CHECK(recorder->said.size() == 2);
    // Asked again unchanged, it is not said again.
    held->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    CHECK(recorder->said.size() == 2);
}

TEST_CASE("TerminalSession: a remembered answer outlives a reload that leaves its setting alone, and not "
          "one that changes it",
          "[contour][session][permission]")
{
    using contour::config::Permission;
    using contour::session::GuardedRole;
    using contour::session::PermissionAsk;

    TestApp testApp;
    auto held = sessionCapturing(testApp.app(), Permission::Ask);
    auto const profile = held->profileName();
    auto asks = 0;
    contour::test::countAsks(*held, contour::session::PermissionAsk { GuardedRole::CaptureBuffer }, asks);

    captureWholePage(*held);
    REQUIRE(asks == 1);
    answerShown(*held, PermissionDecision::Allow, PermissionMemory::Session);
    REQUIRE(capturedSeededScreen(*held));

    // The same file again: the answer stands.
    held->applyReloadedConfig(withCapturePermission(testApp.app(), profile, Permission::Ask), profile);
    mockPtyOf(*held).stdinBuffer().clear();
    captureWholePage(*held);
    CHECK(asks == 1);
    CHECK(capturedSeededScreen(*held));

    // Configured deny wins, without asking: the terminator alone.
    held->applyReloadedConfig(withCapturePermission(testApp.app(), profile, Permission::Deny), profile);
    mockPtyOf(*held).stdinBuffer().clear();
    captureWholePage(*held);
    CHECK(asks == 1);
    CHECK(mockPtyOf(*held).stdinBuffer() == CaptureReplyTerminator);

    // Back to ask: the answer given for the old setting is gone, so it asks again.
    held->applyReloadedConfig(withCapturePermission(testApp.app(), profile, Permission::Ask), profile);
    mockPtyOf(*held).stdinBuffer().clear();
    captureWholePage(*held);
    CHECK(asks == 2);
    CHECK(held->pendingPermissionAsk() == PermissionAsk { GuardedRole::CaptureBuffer });
    CHECK(mockPtyOf(*held).stdinBuffer().empty());

    // And a reload that answers the question on screen answers it, rather than leaving the dialog up.
    held->applyReloadedConfig(withCapturePermission(testApp.app(), profile, Permission::Deny), profile);
    CHECK_FALSE(held->pendingPermissionAsk().has_value());
    CHECK(countOccurrences(mockPtyOf(*held).stdinBuffer(), CaptureReplyTerminator) == 1);
    CHECK_FALSE(mockPtyOf(*held).stdinBuffer().contains(SeededLine));
}

TEST_CASE("TerminalSession: the paste prompt's preview is bounded however large the paste",
          "[contour][session][clipboard][permission]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    // 1 500 lines of 400 bytes: over the soft limit, under the hard one, each line far over a shown line's
    // bound.
    constexpr auto Lines = 1'500;
    constexpr auto LineBytes = 399;
    auto text = QString {};
    for ([[maybe_unused]] auto const i: std::views::iota(0, Lines))
        text += QString(LineBytes, QChar('w')) + QChar('\n');
    clipboard->setText(text);
    session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);

    auto const prompt = session->pendingPermission();
    auto const lines = prompt.value(QStringLiteral("lines")).toList();
    auto const limits = contour::session::PastePreviewLimits {};
    auto const ellipsis = QStringLiteral("…");
    REQUIRE(std::cmp_equal(lines.size(), limits.lines + 1));
    for (auto const& line: lines | std::views::take(static_cast<qsizetype>(limits.lines)))
    {
        auto const map = line.toMap();
        CHECK(map.value(QStringLiteral("style")).toInt()
              == std::to_underlying(contour::session::PromptLineStyle::Verbatim));
        auto const shown = map.value(QStringLiteral("text")).toString();
        // Cut to its bound, and saying so.
        CHECK(shown.endsWith(ellipsis));
        CHECK(std::cmp_equal(shown.toUtf8().size(), limits.bytesPerLine + ellipsis.toUtf8().size()));
    }
    CHECK(lines.back().toMap().value(QStringLiteral("text")).toString()
          == QStringLiteral("… %1 more lines").arg(Lines - static_cast<int>(limits.lines)));
    CHECK(
        prompt.value(QStringLiteral("title")).toString().startsWith(QStringLiteral("Paste 600.0 kB into ")));

    session->applyPendingPaste(PermissionDecision::Deny, PermissionMemory::Once);
    clipboard->clear();
}

TEST_CASE("TerminalSession: a capture asked of a background pane is refused, not dropped",
          "[contour][session][permission]")
{
    // A display-less session (a background split or tab) has no GUI queue to run the gate on, but the
    // client is still owed the terminating chunk — dropping the request silently blocks it forever.
    //
    // This drives the real sequence rather than the hook, deliberately: writeToScreen() holds the
    // terminal's non-recursive state mutex across the parse, so a reply written inline here must NOT
    // re-take it. A regression would hang this test rather than fail it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(std::format("{}\r\n", SeededLine));
    mockPtyOf(*session).stdinBuffer().clear();

    session->terminal().writeToScreen(CaptureWholePageSequence);

    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().contains(SeededLine));
    CHECK(mockPtyOf(*session).stdinBuffer() == CaptureReplyTerminator);
}

TEST_CASE("TerminalSession: Ctrl+click on a link follows it and starts no block selection",
          "[contour][session][hyperlink][input]")
{
    // The default config binds Ctrl+Left to FollowHyperlink AND makes Ctrl the block-selection modifier. A
    // press on a link must be the binding's alone: opening the link hands focus to the browser, so its
    // release never arrives, and a selection started by the press would follow every later move of the
    // pointer.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();
    auto& terminal = session->terminal();
    auto const ctrl = Modifiers { vtbackend::Modifier::Control };
    auto const origin =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(0) };
    auto const onPath =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) };
    auto const elsewhere =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(2), .column = vtbackend::ColumnOffset(6) };
    auto const pixelsElsewhere = vtbackend::PixelCoordinate { .x = { 60 }, .y = { 40 } };

    // What the claim decides from: the cell as it is now, an OSC 8 link or a target known at once.
    auto const opensAt = [&](vtbackend::CellLocation at) {
        auto const lookup = core::locked(terminal, [&] { return terminal.lookUpLinkAt(at); });
        return lookup.hyperlink != nullptr || lookup.target.has_value();
    };
    auto const restPointerAt = [&](vtbackend::CellLocation at) {
        session->sendMouseMoveEvent(Modifiers {}, at, vtbackend::PixelCoordinate {});
    };
    auto const ctrlPressAt = [&](vtbackend::CellLocation at) {
        session->sendMousePressEvent(ctrl, vtbackend::MouseButton::Left, at, vtbackend::PixelCoordinate {});
    };
    auto const checkFollowedWithoutSelecting = [&](std::string_view url) {
        // No release: the browser took focus.
        session->sendMouseMoveEvent(Modifiers {}, elsewhere, pixelsElsewhere);
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().toString().toStdString() == url);
        CHECK_FALSE(terminal.selectionAvailable());
        CHECK_FALSE(terminal.leftMouseButtonPressed());
    };
    auto const checkBlockSelecting = [&]() {
        session->sendMouseMoveEvent(ctrl, elsewhere, pixelsElsewhere);
        CHECK(launcher.openedUrls.empty());
        REQUIRE(terminal.selectionAvailable());
        CHECK(dynamic_cast<vtbackend::RectangularSelection const*>(terminal.selector()) != nullptr);
        CHECK(terminal.leftMouseButtonPressed());
    };

    SECTION("on a link: followed, and no release leaves the pointer selecting")
    {
        seedHoveredHyperlink(*session, "https://contour-terminal.org/");
        REQUIRE(opensAt(origin));

        ctrlPressAt(origin);
        checkFollowedWithoutSelecting("https://contour-terminal.org/");
    }

    SECTION("off a link: the block selection is unchanged")
    {
        terminal.writeToScreen("plain text, no link here\r\nmore text on line two\r\nand three");
        REQUIRE_FALSE(opensAt(origin));

        ctrlPressAt(origin);
        checkBlockSelecting();
    }

    SECTION("a link printed under the resting pointer: claimed, though no pointer event saw it")
    {
        terminal.writeToScreen("plain text");
        restPointerAt(origin);
        terminal.writeToScreen("\033[H\033]8;;https://late.example/\033\\LINK\033]8;;\033\\");
        REQUIRE_FALSE(terminal.isPointerOverLink()); // the hover state is stale
        REQUIRE(opensAt(origin));

        ctrlPressAt(origin);
        checkFollowedWithoutSelecting("https://late.example/");
    }

    SECTION("plain text moved under a pointer resting on a link: not claimed, the drag block-selects")
    {
        seedHoveredHyperlink(*session, "https://contour-terminal.org/");
        terminal.writeToScreen("\033[H\033[Lplain text"); // the link moves down a line
        REQUIRE(terminal.isPointerOverLink());            // the hover state is stale
        REQUIRE_FALSE(opensAt(origin));

        ctrlPressAt(origin);
        checkBlockSelecting();
    }

    SECTION("a URL in plain text, its row not scanned yet: claimed")
    {
        terminal.writeToScreen("https://example.com/page");
        restPointerAt(onPath);
        REQUIRE_FALSE(terminal.isPointerOverLink()); // detection has not run
        REQUIRE(opensAt(onPath));

        ctrlPressAt(onPath);
        checkFollowedWithoutSelecting("https://example.com/page");
    }

    SECTION("a path still to be probed: not claimed, the drag block-selects")
    {
        // It opens only once the probe finds it, if it exists at all; this one does not.
        terminal.writeToScreen(inWorkDirectory("gone.txt"));
        restPointerAt(onPath);
        REQUIRE_FALSE(opensAt(onPath));

        ctrlPressAt(onPath);
        checkBlockSelecting();
        testApp.drainLinkProbes();
        QCoreApplication::processEvents();
        CHECK(launcher.openedUrls.empty());
    }

    SECTION("key mappings toggled off: not claimed, the drag block-selects")
    {
        seedHoveredHyperlink(*session, "https://contour-terminal.org/");
        REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
        REQUIRE(opensAt(origin));

        ctrlPressAt(origin);
        checkBlockSelecting();
    }
}

TEST_CASE("TerminalSession: a file:// link in a pane replicating another machine opens nothing here",
          "[contour][session][hyperlink]")
{
    // Every path the application of another machine's daemon writes names a file on that machine, whatever
    // authority it spells: handed to the desktop, file:///tmp/x.log would open this machine's file of that
    // name. The pane's terminal says so (vtbackend::Terminal::fileUrlLocalHost), as for its working
    // directory.
    namespace actions = contour::actions;
    auto const uri =
        GENERATE_COPY(std::string { "file:///tmp/x.log" },
                      std::string { "file://localhost/tmp/x.log" },
                      "file://" + std::string { contour::test::TestApp::HostName } + "/tmp/x.log");
    INFO("URI: " << uri);
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    SECTION("on another machine: neither opened nor run, hovered or pinned")
    {
        session->terminal().bindReplica(vtbackend::ReplicaRole::OtherMachine, vtbackend::HostIdentity {});
        seedHoveredHyperlink(*session, uri);
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        CHECK((*session)(actions::FollowHyperlink { .uri = uri }));
        CHECK(launcher.openedUrls.empty());
        CHECK(launcher.detached.empty());

        // A link of another scheme is no path, and still opens.
        CHECK((*session)(actions::FollowHyperlink { .uri = "https://example.com/page" }));
        CHECK(launcher.openedUrls.size() == 1);
    }

    SECTION("on the daemon's own machine: a local file, as in a pane of its own")
    {
        session->terminal().bindReplica(vtbackend::ReplicaRole::SameMachine, vtbackend::HostIdentity {});
        seedHoveredHyperlink(*session, uri);
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().isLocalFile());
        CHECK(launcher.openedUrls.back().toLocalFile() == QStringLiteral("/tmp/x.log"));
    }
}

TEST_CASE(
    "TerminalSession: a path under the pointer in a pane replicating another machine opens nothing here",
    "[contour][session][hyperlink]")
{
    // A path the application of another machine's daemon prints names a file on that machine: one that
    // exists here too is another file. Ctrl+click on it opens nothing, as for a file:// link
    // (vtbackend::Terminal::lookUpLinkAt).
    namespace actions = contour::actions;
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    // A file that exists on this machine, named relative to the working directory the application reports.
    testApp.fileSystem().addFile(inWorkDirectory("doc.txt"), "x");
    auto const pointAtPath = [&] {
        session->terminal().setCurrentWorkingDirectory("file://" + std::string { WorkDirectory });
        session->terminal().writeToScreen("open doc.txt now");
        session->terminal().sendMouseMoveEvent(
            vtbackend::Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(0), vtbackend::ColumnOffset(6) },
            vtbackend::PixelCoordinate {},
            /*uiHandledHint=*/false);
        REQUIRE(vtbackend::test::printedPathUnderPointer(session->terminal()) == "doc.txt");
    };
    // Ctrl+click, and the probe's answer, which opens the file back on this thread.
    auto const follow = [&] {
        auto const followed = (*session)(actions::FollowHyperlink {});
        testApp.drainLinkProbes();
        QCoreApplication::processEvents();
        return followed;
    };

    SECTION("on another machine: not opened, and said")
    {
        auto capture = core::log::ScopedCapture { "error" };
        session->terminal().bindReplica(vtbackend::ReplicaRole::OtherMachine, vtbackend::HostIdentity {});
        pointAtPath();
        CHECK(vtbackend::test::lookUpLinkUnderPointer(session->terminal()).refusal
              == vtbackend::PathRefusal::ReplicaOfAnotherMachine);
        CHECK_FALSE(follow());
        CHECK(launcher.openedUrls.empty());
        CHECK(capture.contains(
            "Not opening \"doc.txt\": it names a file on the machine the session runs on, not this one."));
    }

    SECTION("on the daemon's own machine: opened, as in a pane of its own")
    {
        session->terminal().bindReplica(vtbackend::ReplicaRole::SameMachine, vtbackend::HostIdentity {});
        pointAtPath();
        CHECK(follow());
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().isLocalFile());
    }

    SECTION("in a pane of its own: opened")
    {
        pointAtPath();
        CHECK(follow());
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().isLocalFile());
    }
}

TEST_CASE("TerminalSession: a path a command printed on another machine opens nothing here, and says why",
          "[contour][session][hyperlink][blockpaths]")
{
    // The command ran in a directory on another host (over ssh), so the name it printed is a file there --
    // even though one of that name exists in the directory the shell reports now.
    namespace actions = contour::actions;
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    testApp.fileSystem().addFile(inWorkDirectory("doc.txt"), "x");
    auto const generic = std::string { WorkDirectory };
    auto const remote =
        "file://otherhost.invalid" + std::string(generic.starts_with('/') ? "" : "/") + generic;

    session->terminal().writeToScreen("\033]7;" + remote + "\033\\"
                                      + "\033]133;A\033\\$ ls\r\n\033]133;C\033\\"
                                      + "open doc.txt now\r\n\033]133;D;0\033\\");
    session->terminal().setCurrentWorkingDirectory("file://" + generic);
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(1), .column = vtbackend::ColumnOffset(6) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    REQUIRE(vtbackend::test::printedPathUnderPointer(session->terminal()) == "doc.txt");

    auto capture = core::log::ScopedCapture { "error" };
    CHECK_FALSE((*session)(actions::FollowHyperlink {}));
    CHECK(launcher.openedUrls.empty());
    CHECK(capture.contains("Not opening \"doc.txt\": the command that printed it ran on another machine."));
}

TEST_CASE("TerminalSession: a path under the pointer opens as the app's filesystem names it",
          "[contour][session][hyperlink]")
{
    // The terminal finds the path through the app's filesystem, and opening it asks the same one. Asked of
    // the disk instead, a path only the app's filesystem holds reached the desktop as a relative URL.
    namespace actions = contour::actions;
    contour::test::TestApp testApp { contour::test::TestApp::SessionFiles::Fixture };
    // Absolute on every platform, as the temporary directory is; a name in the in-memory filesystem only.
    auto const work = std::filesystem::temp_directory_path() / "contour-in-memory-work";
    testApp.fileSystem().addDirectory(work);
    testApp.fileSystem().addFile(work / "doc.txt", "x");
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto& launcher = testApp.launcher();

    terminal.setCurrentWorkingDirectory(localFileUrl(work).toString().toStdString());
    terminal.writeToScreen("open doc.txt now");
    terminal.sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(6) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    REQUIRE(vtbackend::test::printedPathUnderPointer(terminal) == "doc.txt");

    CHECK((*session)(actions::FollowHyperlink {}));
    // Opened once the probe has answered, back on this thread.
    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    REQUIRE(launcher.openedUrls.size() == 1);
    CHECK(launcher.openedUrls.back() == localFileUrl(work / "doc.txt"));
}

TEST_CASE("TerminalSession: a file:// hyperlink is followed as the app's filesystem names it",
          "[contour][session][hyperlink]")
{
    // Asked of the app's filesystem, where the terminal finds the paths it shows: a program only it holds is
    // run, through `contour config`. Asked of the disk, it was no file, and went to the desktop.
    namespace actions = contour::actions;
    contour::test::TestApp testApp { contour::test::TestApp::SessionFiles::Fixture };
    // A program by either platform's rule: an execute bit, and a name ending in ".exe". Absolute on every
    // platform, as the temporary directory is; a name in the in-memory filesystem only.
    auto const tool = std::filesystem::temp_directory_path() / "contour-in-memory-bin" / "tool.exe";
    testApp.fileSystem().addExecutable(tool);
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    seedHoveredHyperlink(*session, localFileUrl(tool).toString().toStdString());
    REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
    CHECK((*session)(actions::FollowHyperlink {}));

    CHECK(launcher.openedUrls.empty());
    REQUIRE(launcher.detached.size() == 1);
    CHECK(launcher.detached.back().arguments.front() == QStringLiteral("config"));
    CHECK(launcher.detached.back().arguments.back() == localFileUrl(tool).toLocalFile());
}

TEST_CASE(
    "TerminalSession: a file:// hyperlink to a text file opens in the editor the app's environment names",
    "[contour][session][hyperlink]")
{
    // $EDITOR is read through the app's environment, as every other variable is: a text file only the app's
    // filesystem holds opens in the editor that environment names, through `contour config`.
    namespace actions = contour::actions;
    contour::test::TestApp testApp { contour::test::TestApp::SessionFiles::Fixture,
                                     { { "EDITOR", "contour-test-editor" } } };
    // Absolute on every platform, as the temporary directory is; a name in the in-memory filesystem only.
    auto const notes = std::filesystem::temp_directory_path() / "contour-in-memory-doc" / "notes.txt";
    testApp.fileSystem().addFile(notes, "x");
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    seedHoveredHyperlink(*session, localFileUrl(notes).toString().toStdString());
    REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
    CHECK((*session)(actions::FollowHyperlink {}));

    CHECK(launcher.openedUrls.empty());
    REQUIRE(launcher.detached.size() == 1);
    auto const& arguments = launcher.detached.back().arguments;
    REQUIRE(arguments.size() == 4);
    CHECK(arguments.at(0) == QStringLiteral("config"));
    CHECK(arguments.at(2) == QStringLiteral("contour-test-editor"));
    CHECK(arguments.at(3) == localFileUrl(notes).toLocalFile());
}

TEST_CASE("TerminalSession: FollowHyperlink routes each URI class through the external launcher",
          "[contour][session][hyperlink]")
{
    // followHyperlink() branches on the hovered link's URI: a remote link and a local non-file both
    // open via openUrl; a local link to an existing executable file starts `contour config ...` via the
    // launcher's runDetached(). Driving them headlessly (hover + FollowHyperlink) exercises all the
    // launcher-routing arms without opening a browser or spawning a process.
    namespace actions = contour::actions;

    SECTION("a remote http link opens via openUrl")
    {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        seedHoveredHyperlink(*session, "https://example.com/page");
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().toString().toStdString() == "https://example.com/page");
        CHECK(launcher.detached.empty());
    }

    SECTION("a local file:// link to a non-existent path opens via openUrl (not run)")
    {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        seedHoveredHyperlink(*session, "file://localhost/no/such/file/here.txt");
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        CHECK(launcher.openedUrls.size() == 1);
        CHECK(launcher.detached.empty());
    }

    SECTION("a local file:// link opens the file its pill names")
    {
        // Read by the resolver the pill uses (vtbackend::localPathOfFileUrl): a fragment is no part of the
        // path. A drive-letter link, which Qt cannot parse, is a local file only under the Windows path
        // convention; anywhere else it names no file here, and both show and open it as written.
        auto const windowsPaths = vtbackend::NativePathConvention == vtbackend::PathConvention::Windows;
        auto const links = std::vector<std::pair<std::string, std::optional<std::string>>> {
            { "file:///no/such/file.txt#L10", "/no/such/file.txt" },
            { "file://C:/no/such/file.txt",
              windowsPaths ? std::optional<std::string> { "C:/no/such/file.txt" } : std::nullopt },
        };
        for (auto const& [uri, path]: links)
        {
            INFO("URI: " << uri);
            contour::test::TestApp testApp;
            auto session = makeDisplaylessSession(testApp.app());
            auto& launcher = testApp.launcher();

            seedHoveredHyperlink(*session, uri);
            REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
            CHECK((*session)(actions::FollowHyperlink {}));
            REQUIRE(launcher.openedUrls.size() == 1);
            CHECK(launcher.openedUrls.back()
                  == (path ? QUrl::fromLocalFile(QString::fromStdString(*path))
                           : QUrl(QString::fromStdString(uri))));
            CHECK(contour::session::linkPillText(uri, std::string { contour::test::TestApp::HostName }, 64)
                  == path.value_or(uri));
        }
    }

    // A file:// URL that survives with its authority intact is read by the desktop's URL handler as a
    // network location ("//host/path"), which does not exist -- issue #2057. Every spelling of THIS
    // machine must therefore reach openUrl() as a plain local file URL.
    SECTION("every spelling of this host resolves to a plain local file URL")
    {
        auto const shortName = std::string { contour::test::TestApp::HostName };

        auto const uri = GENERATE_COPY(
            // The authority-free form. Its host is "", which the old exact-match check never accepted.
            std::string { "file:///tmp/x.log" },
            // The literal loopback name.
            std::string { "file://localhost/tmp/x.log" },
            // The short host name -- the only form that used to be classified as local.
            "file://" + shortName + "/tmp/x.log",
            // The same name in a different case.
            "file://" + QString::fromStdString(shortName).toUpper().toStdString() + "/tmp/x.log",
            // The fully-qualified name, as gethostname(2) reports it on the reporter's machine.
            "file://" + shortName + ".example.invalid/tmp/x.log");

        INFO("URI: " << uri);
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        seedHoveredHyperlink(*session, uri);
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        // The host authority is gone: a local file URL, not a //host network path.
        CHECK(launcher.openedUrls.back().host().isEmpty());
        CHECK(launcher.openedUrls.back().isLocalFile());
        CHECK(launcher.openedUrls.back().toLocalFile() == QStringLiteral("/tmp/x.log"));
    }

    SECTION("a file:// link naming ANOTHER host keeps its authority")
    {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        // Not this machine, so there is no local path to resolve it to: hand it over as written and let
        // the desktop's URL handler decide (it may know how to mount it; we do not).
        seedHoveredHyperlink(*session, "file://some-other-host.invalid/tmp/x.log");
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().host() == QStringLiteral("some-other-host.invalid"));
        CHECK(launcher.detached.empty());
    }

#ifndef _WIN32
    // The "is this an executable file" branch of followHyperlink() keys off POSIX owner-execute
    // permission; Windows determines executability by extension, not filesystem perms, so this Unix
    // semantics section does not apply there.
    SECTION("a local file:// link to an executable file starts `contour config` without waiting for it")
    {
        // The authority-free form reaches the executable branch too: it used to be classified as remote
        // (its host is ""), so a file:///path link to a script fell through to openUrl() instead.
        auto const authority =
            GENERATE(std::string {}, std::string { contour::test::TestApp::HostName } + ".example.invalid");

        INFO("authority: " << authority);
        // This machine's filesystem: the executable is a real file, whose execute bit the app reads.
        contour::test::TestApp testApp { contour::test::TestApp::SessionFiles::ThisMachine };
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        // Create a real executable file and hover a file://<authority>/<path> link to it.
        QTemporaryDir const tmp;
        REQUIRE(tmp.isValid());
        auto const exePath = std::filesystem::path(tmp.path().toStdU16String()) / "script.sh";
        {
            auto out = std::ofstream(exePath);
            out << "#!/bin/sh\n";
        }
        std::filesystem::permissions(
            exePath, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
        auto const uri = "file://" + authority + exePath.string();

        seedHoveredHyperlink(*session, uri);
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        // Detached: the second Contour runs until its window is closed, and waiting for it would hold up
        // the GUI thread -- every window of this Contour -- all that time. The launcher has nothing that
        // waits for a program any more.
        REQUIRE(launcher.detached.size() == 1);
        CHECK(launcher.detached.back().arguments.contains(QStringLiteral("config")));
        // The path reaches `contour config` as the filesystem spells it, not as a URL.
        CHECK(launcher.detached.back().arguments.back() == QString::fromStdString(exePath.generic_string()));
    }
#endif
}
