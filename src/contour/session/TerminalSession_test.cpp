// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for TerminalSession that exercise the *real* class (not a Qt-free surrogate).
//
// Unlike the model-layer tests (TabListModel_test / vtworkspace SessionModel_test), these construct an
// actual TerminalSession around a MockPty and a test-configured ContourGuiApp. That is only possible
// because the contour frontend is built as the `contour_core` object library the test links against,
// and because core::cli::App exposes parseParametersForTesting() to populate parameters() without
// launching the GUI event loop.
//
// The headline case is the regression behind the "close leaks background tabs" finding:
// TerminalSession::terminate() must close the PTY device even when NO display is attached (a
// background tab/split pane whose display was detached on the last tab switch). Before the fix
// terminate() early-returned for a display-less session, so the device stayed open and the session —
// plus its shell process — leaked.
//
// TerminalSession_test.cpp holds the session basics: lifecycle, working directory, actions, input,
// configuration reload and the scrollbar marks. The remaining areas live in
// TerminalSessionPermissions_test.cpp, TerminalSessionView_test.cpp, TerminalSessionBlocks_test.cpp and
// TerminalSessionPane_test.cpp, split so that no single translation unit compiles the whole suite.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/EndReason.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/session/TerminalSessionTestSupport.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/log/LogSink.hpp>
#include <core/platform/FileUri.hpp>
#include <core/platform/PathUtils.hpp>
#include <core/testing/ScopedTempDir.hpp>

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

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
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

/// Puts a vi `mm` mark on the line the cursor stands on, as a user in normal mode does, and leaves
/// normal mode again.
void markCursorLine(vtbackend::Terminal& terminal)
{
    terminal.inputHandler().setMode(vtbackend::ViMode::Normal);
    auto const now = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto const key: std::views::iota(0, 2))
        std::ignore = terminal.sendCharEvent(U'm',
                                             vtbackend::KeyIdentity { .unshiftedKey = U'm' },
                                             Modifiers {},
                                             KeyboardEventType::Press,
                                             now);
    terminal.inputHandler().setMode(vtbackend::ViMode::Insert);
}

/// Where tick @p index of @p session's published marks sits, as a fraction of the track.
[[nodiscard]] double markPosition(contour::session::TerminalSession const& session, qsizetype index)
{
    return session.scrollbarMarks().at(index).toMap().value(QStringLiteral("position")).toDouble();
}

/// The line a click on tick @p index of @p session's published marks brings to the top.
[[nodiscard]] int64_t markTarget(contour::session::TerminalSession const& session, qsizetype index)
{
    return session.scrollbarMarks().at(index).toMap().value(QStringLiteral("targetLine")).toLongLong();
}

/// Has the application ask for @p session's host-writable status line (DECSSDT 2), denies it, then has the
/// application write @p text to it and return to the main display, as a program that keeps its status in
/// DEC's status line does.
void writeToDeniedStatusLine(contour::session::TerminalSession& session, std::string_view text)
{
    session.terminal().writeToScreen("\033[2$~");
    session.executeShowHostWritableStatusLine(PermissionDecision::Deny, PermissionMemory::Once);
    session.terminal().writeToScreen(std::format("\033[1$}}{}\033[0$}}", text));
}

/// Sets every profile's `status_line.sync_to_window_title` in @p app's configuration.
void setSyncToWindowTitle(contour::ContourGuiApp& app, bool sync)
{
    for (auto& [name, profile]: app.config().profiles.value())
        profile.statusLine.value().syncWindowTitleWithHostWritableStatusDisplay = sync;
}

/// Sets every profile's `status_line.display` in @p app's configuration.
void setStatusLineDisplay(contour::ContourGuiApp& app, vtbackend::StatusDisplayType type)
{
    for (auto& [name, profile]: app.config().profiles.value())
        profile.statusLine.value().initialType = type;
}

/// Has the application select @p session's host-writable status line (DECSSDT 2), the user allow it, and
/// the application write to it (DECSASD 1).
void writeToAllowedStatusLine(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033[2$~");
    session.executeShowHostWritableStatusLine(PermissionDecision::Allow, PermissionMemory::Once);
    session.terminal().writeToScreen("\033[1$}");
    REQUIRE(session.terminal().activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);
}

/// Writes @p configFile with two profiles: @p mainProfile, the app's own, and `work`, each with its
/// own terminal ID -- as a user saving the file would.
void writeTwoProfileConfig(std::filesystem::path const& configFile,
                           std::string const& mainProfile,
                           std::string_view mainId,
                           std::string_view workId)
{
    contour::test::writeConfigFile(configFile,
                                   std::format("default_profile: {0}\n"
                                               "profiles:\n"
                                               "    {0}:\n"
                                               "        terminal_id: {1}\n"
                                               "    work:\n"
                                               "        terminal_id: {2}\n",
                                               mainProfile,
                                               mainId,
                                               workId));
}

/// Pumps the Qt event loop until @p predicate holds or about five seconds pass.
/// @return Whether @p predicate held.
[[nodiscard]] bool pumpUntil(std::function<bool()> const& predicate)
{
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 500))
    {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds { 10 });
    }
    return predicate();
}

/// The DA1 reply @p session's terminal gives now.
[[nodiscard]] std::string primaryDeviceAttributesOf(contour::session::TerminalSession& session)
{
    auto& pty = mockPtyOf(session);
    pty.stdinBuffer().clear();
    session.terminal().writeToScreen("\033[c");
    session.terminal().flushInput();
    return pty.stdinBuffer();
}

/// Whether the app watches its configuration file (`live_config`).
enum class LiveConfig : uint8_t
{
    Off,
    On,
};

/// A TestApp whose configuration lives in a file of its own, with two profiles: the app's and `work`.
struct TwoProfileApp
{
    contour::test::ConfiguredTestApp testApp;
    contour::test::ScopedController window;
    std::string mainProfile;

    explicit TwoProfileApp(LiveConfig liveConfig):
        testApp { std::make_unique<contour::test::MockPtySessionFactory>() },
        window { testApp.manager() },
        mainProfile { testApp.app().profileName() }
    {
        auto& config = testApp.app().config();
        config.live.value() = liveConfig == LiveConfig::On;
        config.profiles.value()["work"] = config.profiles.value().at(mainProfile);
        writeTwoProfileConfig(testApp.configFile(), mainProfile, "VT525", "VT525");
    }
};

} // namespace

TEST_CASE("TerminalSession::terminate closes the PTY device when no display is attached",
          "[contour][session][close]")
{
    // Regression guard: a background tab/split-pane session has no display, and terminate() must
    // still close it. Before the fix terminate() did `if (!_display) return;` — a silent no-op that
    // left the device open and leaked the session + its shell.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    REQUIRE(session->display() == nullptr);                 // precondition: no display attached
    REQUIRE_FALSE(session->terminal().device().isClosed()); // precondition: device still open

    session->terminate();

    // The device is now closed. This is the display-independent close trigger: ExitWatcherThread's
    // waitForClosed() returns and posts onClosed() -> sessionClosed -> removeSession.
    CHECK(session->terminal().device().isClosed());
}

TEST_CASE("TerminalSession::terminate is idempotent on an already-closed display-less session",
          "[contour][session][close]")
{
    // Closing the device twice must be safe (onClosed() guards against this too): a second terminate()
    // on a session whose device is already closed is a no-op, not a double-close.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    session->terminate();
    REQUIRE(session->terminal().device().isClosed());

    CHECK_NOTHROW(session->terminate());
    CHECK(session->terminal().device().isClosed());
}

TEST_CASE("TerminalSession::cursorPositionChanged is a safe no-op without a display",
          "[contour][session][ime]")
{
    // Regression guard for the coalescing-flag leak. cursorPositionChanged() fires on the terminal/
    // render thread once per frame AND on every cursor blink — including while a background tab or a
    // collapsing split has NO display attached (the detach->attach gap). Two things must hold with no
    // display: it must not dereference the absent display, and — the subtle half — it must not latch
    // its coalescing flag. Latching without scheduling the post that clears it would strand the flag
    // set forever, so once a display finally arrived every later cursorPositionChanged() would
    // early-return, permanently freezing IME rectangle tracking and the accessibility caret. With no
    // display the whole notification must collapse to nothing, safely, any number of times.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE(session->display() == nullptr); // precondition: no display attached

    // Blink/frame churn: many cursor-move notifications while no display is present.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 8))
        CHECK_NOTHROW(session->cursorPositionChanged());

    // Still no display, still no crash: every notification stayed a pure no-op.
    CHECK(session->display() == nullptr);
}

TEST_CASE("TerminalSession::workingDirectory falls back to \".\" for a non-process device",
          "[contour][session][cwd]")
{
    // workingDirectory() is the single cwd-inheritance accessor shared by every spawn path (new tab,
    // new window, split pane). For a device that is not a local process (here a MockPty; in production
    // an SSH session on non-Windows) it must return the "." fallback rather than crash on the failed
    // dynamic_cast — this is what split panes now use so they inherit a cwd on every platform.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // The fallback is uniform across platforms: for a non-process device the "." sentinel is returned
    // (off Windows the failed dynamic_cast<Process> path yields it; on Windows a fresh MockPty reports
    // an empty OSC-7 cwd, which — being useless to CreateProcess — also falls through to "."). The point
    // is a total accessor that never crashes and always hands a usable directory to a new tab/split.
    CHECK(session->workingDirectory() == ".");
}

TEST_CASE("TerminalSession::workingDirectory rejects a cwd that does not exist on the local machine",
          "[contour][session][cwd]")
{
    // Regression: creating a new tab (or split) from an SSH session crashed on Windows. The remote
    // shell reports its cwd via OSC 7 as a file:// URL carrying a *remote* path (e.g.
    // "file://remotehost/home/user"). workingDirectory() extracted "/home/user" and handed it to the
    // new local shell's CreateProcess() as lpCurrentDirectory; that directory does not exist on the
    // local machine, so CreateProcess() failed and Process::start() threw — the exception then
    // propagated through the QML `session:` binding write inside a Qt event handler and aborted the
    // whole process. The accessor must only return a directory that exists locally, falling back to
    // the "." sentinel otherwise (which CreateProcess treats as "inherit the parent's cwd").
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // A remote/nonexistent cwd advertised over OSC 7. Use a path that cannot exist locally on any
    // platform so the check is uniform (the leading component is a bogus host+root).
    session->terminal().setCurrentWorkingDirectory("file://remotehost/this/path/does/not/exist/anywhere");
    CHECK(session->workingDirectory() == ".");

    // A cwd that *does* exist locally is honoured — the inheritance must still work for a local shell.
    auto const local = std::filesystem::temp_directory_path();
    session->terminal().setCurrentWorkingDirectory(
        std::format("file://localhost/{}", local.generic_string()));
    auto const resolved = session->workingDirectory();
    // On non-Windows the device is a MockPty (not a vtpty::Process), so the accessor returns "."
    // regardless of OSC 7; the local-existence contract is the Windows behaviour under test. Either
    // the local path was resolved, or the platform-uniform "." fallback was taken — never a
    // nonexistent path.
    CHECK((resolved == "." || std::filesystem::exists(resolved)));
}

TEST_CASE("TerminalSession::displayWorkingDirectory shows what a path holds, not obeys it",
          "[contour][session][cwd]")
{
    // OSC 7 is percent-decoded, so %0A becomes a line feed and %E2%80%AE a right-to-left override. Shown raw
    // in the tab's tooltip, the first would add a line of the application's choosing, the second reverse
    // what follows it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setCurrentWorkingDirectory("file:///tmp/x%0AFake%20title%E2%80%AEtxt");
    CHECK(session->displayWorkingDirectory()
          == "/tmp/x\xEF\xBF\xBD"
             "Fake title\xEF\xBF\xBDtxt");
}

#ifdef _WIN32 // POSIX asks the child's /proc entry, not OSC 7, which a MockPty has none of
TEST_CASE("TerminalSession::workingDirectory opens a new tab in an OSC 7 directory beyond ASCII",
          "[contour][session][cwd]")
{
    // OSC 7 reports a directory in UTF-8. Read as a narrow path, Windows took it in the ANSI code page:
    // workingDirectory() found no such directory and answered ".", and a new tab handed the directory
    // could not start in it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const temp = core::testing::ScopedTempDir { "contour-cwd" };
    auto const directory = temp.path() / vtpty::testing::NonAsciiName;
    std::filesystem::create_directory(directory);
    session->terminal().setCurrentWorkingDirectory(
        core::platform::fileUri(core::platform::normalizePath(directory), "localhost"));

    auto const workingDirectory = session->workingDirectory();
    auto const reported = core::pathFromUtf8(workingDirectory);
    INFO("working directory: " << core::escape(workingDirectory));
    REQUIRE(reported.has_value());
    REQUIRE(std::filesystem::exists(*reported));
    CHECK(std::filesystem::equivalent(*reported, directory));

    // The new tab's shell starts there: the spawn gives up a working directory it cannot start in, and
    // says so, rather than not starting at all.
    auto factory =
        contour::session::AppSessionFactory { testApp.app(), std::make_shared<vtpty::NoPlacement>() };
    auto pty = factory.createPty(
        workingDirectory, std::nullopt, vtpty::testing::execInfo("cmd.exe", { "/d", "/c", "exit" }));
    auto const started = pty->start();
    REQUIRE(started.has_value());
    CHECK(started->diagnostic.empty());
}
#endif

TEST_CASE("TerminalSession::start reports a device that fails to start instead of propagating",
          "[contour][session][start]")
{
    // Regression for #1711 ("Crash on Windows"). On Windows there is no fork(), so a CreateProcess()
    // failure — a working directory the shell cannot be started in, a shell that is not on PATH — is
    // discovered in the PARENT, and vtpty's only way to report it was to throw. TerminalSession::start()
    // let that exception through, so it unwound out of TerminalDisplay::setSession() halfway: past the
    // image decoder, past the font/grid re-seed, and past `emit sessionChanged(newSession)`. The session
    // was by then registered with the manager and owned a tab in the vtmux model, but its update thread
    // and exit watcher had never started, so no onClosed() could ever prune it — a half-bound display
    // and a zombie pane.
    //
    // A shell that will not start is an expected, recoverable failure. It must be REPORTED — in the
    // pane, the way the POSIX child already reports its own chdir/exec failures — never propagated.
    TestApp testApp;
    auto session =
        makeSessionWith(testApp.app(),
                        std::make_unique<contour::test::ConfigurableStartPty>(
                            TestPageSize, contour::test::ConfigurableStartPty::StartBehavior::Throw));

    // The headline: whatever the device does, start() must not take the process down with it.
    CHECK_NOTHROW(session->start());

    // Reported where the user is looking -- the pane's footer, not the grid -- carrying the reason rather
    // than a bare "it failed".
    using contour::session::EndReason;
    CHECK(session->getSessionEnd() == static_cast<int>(EndReason::StartFailed));
    CHECK(session->getEndMessage().contains(
        QString::fromUtf8(contour::test::ConfigurableStartPty::FailureText.data(),
                          static_cast<qsizetype>(contour::test::ConfigurableStartPty::FailureText.size()))));
    CHECK_FALSE(session->terminal().primaryScreen().renderMainPageText().contains(
        contour::test::ConfigurableStartPty::FailureText));

    // The device never came up, so it is left closed -- which is what makes the pane dismissible
    // through the paths that already exist (terminate()'s already-closed branch, and dismiss()).
    CHECK(session->terminal().device().isClosed());

    // The pane is not a zombie, but neither does a stray key close it: Return asks for a restart, which a
    // session no manager hosts cannot have, so it is swallowed; the footer's Close is what dismisses it.
    auto closed = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&](contour::session::TerminalSession&) { ++closed; });
    session->sendKeyEvent(vtbackend::Key::Enter,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    CHECK(closed == 0);
    session->dismiss();
    CHECK(closed == 1);
}

TEST_CASE("TerminalSession::start surfaces a reported start failure", "[contour][session][start]")
{
    // The same contract as above, reached the way vtpty now actually reports it: as a StartFailure
    // value rather than an exception. This is the shape Process_win32's CreateProcess() ladder returns
    // when every rung failed.
    TestApp testApp;
    auto session =
        makeSessionWith(testApp.app(),
                        std::make_unique<contour::test::ConfigurableStartPty>(
                            TestPageSize, contour::test::ConfigurableStartPty::StartBehavior::Fail));

    auto ends = QSignalSpy(session.get(), &contour::session::TerminalSession::sessionEndChanged);
    CHECK_NOTHROW(session->start());

    // The platform's own reason reaches the user -- a bare "could not start" would leave them with no
    // idea that it was the directory, which is exactly what they need to fix.
    CHECK(session->getSessionEnd() == static_cast<int>(contour::session::EndReason::StartFailed));
    CHECK(session->getEndMessage().contains(
        QString::fromUtf8(contour::test::ConfigurableStartPty::FailureText.data(),
                          static_cast<qsizetype>(contour::test::ConfigurableStartPty::FailureText.size()))));
    CHECK(ends.count() == 1);
    CHECK(session->terminal().device().isClosed());

    // A local session that failed to start may be tried again.
    CHECK(session->endAction() == contour::session::FooterAction::Restart);

    // And the second report of the same end (onClosed() after the failure) does not repeat it.
    session->onClosed();
    CHECK(ends.count() == 1);
}

TEST_CASE("TerminalSession::start runs a session that started with a diagnostic", "[contour][session][start]")
{
    // A spawn-ladder rung below the first one: the child IS running, but not the way it was asked for
    // (here the inherited working directory had to be dropped). That is a notice, not a failure — the
    // session must come up completely, or the recovery would be worse than the crash it replaced.
    TestApp testApp;
    auto session =
        makeSessionWith(testApp.app(),
                        std::make_unique<contour::test::ConfigurableStartPty>(
                            TestPageSize, contour::test::ConfigurableStartPty::StartBehavior::Diagnose));

    CHECK_NOTHROW(session->start());

    auto const screen = session->terminal().primaryScreen().renderMainPageText();
    CHECK(screen.contains(contour::test::ConfigurableStartPty::DiagnosticText));

    // Unlike the failure cases, the device stays OPEN and the read loop is running.
    CHECK_FALSE(session->terminal().device().isClosed());

    // Not armed for dismissal: a key press is input for the shell, not an acknowledgement.
    auto closed = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&](contour::session::TerminalSession&) { ++closed; });
    session->sendKeyEvent(vtbackend::Key::Enter,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    CHECK(closed == 0);
}

TEST_CASE("TerminalSession: key and char events write their encoding into the PTY",
          "[contour][session][input]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    session->sendCharEvent(
        U'a', vtbackend::KeyIdentity { .unshiftedKey = U'a' }, Modifiers {}, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer() == "a");

    session->sendKeyEvent(vtbackend::Key::Enter, Modifiers {}, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer() == "a\r");

    // A release in the default keyboard protocol encodes nothing.
    session->sendCharEvent(
        U'a', vtbackend::KeyIdentity { .unshiftedKey = U'a' }, Modifiers {}, KeyboardEventType::Release, now);
    CHECK(mockPtyOf(*session).stdinBuffer() == "a\r");
}

TEST_CASE("TerminalSession: a bare Ctrl press does not hide the pointer", "[contour][session]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    // hide_while_typing is on by default (Config.hpp ~596); assert that rather than assume it
    REQUIRE(session->profile().mouse.value().hideWhileTyping);
    auto const now = std::chrono::steady_clock::now();
    session->sendKeyEvent(vtbackend::Key::LeftControl,
                          vtbackend::KeyboardModifiers { vtbackend::Modifier::Control },
                          vtbackend::KeyboardEventType::Press,
                          now);
    CHECK((session.surface->cursorShapes.empty()
           || session.surface->cursorShapes.back() != contour::input::MouseCursorShape::Hidden));
}

TEST_CASE("TerminalSession: a non-modifier key still hides the pointer while typing", "[contour][session]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    REQUIRE(session->profile().mouse.value().hideWhileTyping);
    session.surface->cursorShapes.clear();
    auto const now = std::chrono::steady_clock::now();
    session->sendKeyEvent(
        vtbackend::Key::F5, vtbackend::KeyboardModifiers {}, vtbackend::KeyboardEventType::Press, now);
    REQUIRE_FALSE(session.surface->cursorShapes.empty());
    CHECK(session.surface->cursorShapes.back() == contour::input::MouseCursorShape::Hidden);
}

TEST_CASE("TerminalSession: mouse events without a mouse protocol write nothing and do not crash",
          "[contour][session][input]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();
    auto const pos = vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(1) };
    auto const pixels = vtbackend::PixelCoordinate {};
    (void) now;

    session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Left, pos, pixels);
    session->sendMouseMoveEvent(Modifiers {}, pos, pixels);
    session->sendMouseReleaseEvent(Modifiers {}, vtbackend::MouseButton::Left, pixels);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: right-click opens the context menu exactly when a selection drag would work",
          "[contour][session][input][contextmenu]")
{
    // The activation rule, pinned where it is actually decided. There is no new conditional behind it: the
    // right button is a BUILT-IN FALLBACK mouse mapping, and TerminalSession::sendMousePressEvent consults
    // that table only after vtbackend has declined the press. So the menu fires exactly when the terminal
    // would have let the user drag a selection instead — the same gate, reused rather than restated.
    //
    // Both halves are observed, and they have to be. An empty PTY says only that the APPLICATION did not
    // get the click; it says nothing about where the click went instead, and it is just as empty when the
    // fallback lookup is deleted outright. So the menu request is counted at its own seam
    // (TerminalSessionManager::contextMenuRequested, emitted before the routing that needs a window), and
    // the PTY is checked alongside it: together they distinguish "the menu took the click" from "the
    // application took it" from "nothing took it at all".
    //
    // The middle-click section pastes the primary selection, which belongs to the display server and
    // outlives every case before this one, so start from an empty one rather than whatever they left.
    QGuiApplication::clipboard()->clear(QClipboard::Selection);

    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const pixels = vtbackend::PixelCoordinate {};
    auto const pos = vtbackend::CellLocation {};

    auto menuRequests = 0;
    QObject::connect(&testApp.app().sessionsManager(),
                     &contour::session::TerminalSessionManager::contextMenuRequested,
                     [&](contour::session::TerminalSession*) { ++menuRequests; });

    SECTION("no mouse protocol: the application hears nothing, and the menu takes the click")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pos, pixels);
        CHECK(menuRequests == 1);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("mouse protocol on: the application gets its right-click and the menu stays shut")
    {
        // DECSET 1000 -- what vim and tmux turn on.
        session->terminal().writeToScreen("\033[?1000h");
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pos, pixels);
        CHECK(menuRequests == 0);
        CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("mouse protocol on, plus Shift: the bypass modifier hands the click back to the menu")
    {
        session->terminal().writeToScreen("\033[?1000h");
        session->sendMousePressEvent(
            Modifiers { vtbackend::Modifier::Shift }, vtbackend::MouseButton::Right, pos, pixels);
        // Shift is the bypass modifier, so the application is skipped and the bare `Right` fallback
        // matches (sendMousePressEvent strips the bypass modifier before looking the mapping up).
        CHECK(menuRequests == 1);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("a left-drag in flight keeps the menu shut: the popup would steal the button-release")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Left, pos, pixels);
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pos, pixels);
        CHECK(menuRequests == 0);
    }

    SECTION("a middle-click still pastes: the fallback claims the right button and nothing else")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Middle, pos, pixels);
        CHECK(menuRequests == 0);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }
}

TEST_CASE("TerminalSession: SendChars and WriteScreen actions route to PTY and screen respectively",
          "[contour][session][actions]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    CHECK((*session)(contour::actions::SendChars { .chars = "ls\r" }));
    CHECK(mockPtyOf(*session).stdinBuffer() == "ls\r");

    CHECK((*session)(contour::actions::WriteScreen { .chars = "hello" }));
    // The parser thread is not running; process the write synchronously.
    session->terminal().writeToScreen(""); // flush point (writeToScreen appends+processes when unstarted)
}

TEST_CASE("TerminalSession: an action that changes hosted state runs where the session is hosted",
          "[contour][session][actions][operations]")
{
    using contour::session::HostedOperation;
    using contour::session::HostedOperationRequest;
    using contour::session::HostedOperationRoute;
    static_assert(contour::session::hostedOperationOf<contour::actions::SoftReset>()
                  == HostedOperation::SoftReset);
    static_assert(!contour::session::hostedOperationOf<contour::actions::CopySelection>().has_value());

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };
    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    for (auto const row: std::views::iota(0, 40))
        session->terminal().writeToScreen(std::format("row {}\r\n", row));
    session->terminal().writeToScreen("\033]133;A\033\\$ ");
    auto const historyOf = [&] {
        return session->terminal().primaryScreen().historyLineCount();
    };
    auto const history = historyOf();
    REQUIRE(history > vtbackend::LineCount(0));

    auto notices = std::vector<std::string> {};
    QObject::connect(
        session,
        &contour::session::TerminalSession::showNotification,
        [&](QString const&, QString const& content) { notices.push_back(content.toStdString()); });

    SECTION("a mirrored session's host performs each of them, and the mirror is left to follow")
    {
        factory->hostedOperationRoute = HostedOperationRoute::RunOnHost;

        CHECK((*session)(contour::actions::ClearToPrompt {}));
        CHECK((*session)(contour::actions::ClearHistoryAndReset {}));
        CHECK((*session)(contour::actions::SoftReset {}));
        CHECK((*session)(contour::actions::WriteScreen { .chars = "\033[2J" }));

        CHECK(factory->requestedHostedOperations
              == std::vector {
                  HostedOperationRequest { .operation = HostedOperation::ClearToPrompt },
                  HostedOperationRequest { .operation = HostedOperation::ClearHistoryAndReset },
                  HostedOperationRequest { .operation = HostedOperation::SoftReset },
                  HostedOperationRequest { .operation = HostedOperation::WriteScreen, .text = "\033[2J" },
              });
        CHECK(historyOf() == history);
        CHECK(notices.empty());
    }

    SECTION("a local session runs them here")
    {
        CHECK((*session)(contour::actions::ClearToPrompt {}));
        CHECK(factory->requestedHostedOperations.size() == 1);
        CHECK(historyOf() == vtbackend::LineCount(0));
    }

    SECTION("a remote end that refuses one changes nothing and says so")
    {
        factory->hostedOperationRoute = HostedOperationRoute::Refused;

        CHECK_FALSE((*session)(contour::actions::ClearToPrompt {}));
        CHECK(historyOf() == history);
        REQUIRE(notices.size() == 1);
        CHECK(notices[0].contains("clear to the prompt"));
    }

    SECTION("a remote end out of reach changes nothing, reports no success, and says so")
    {
        factory->hostedOperationRoute = HostedOperationRoute::Unreachable;

        CHECK_FALSE((*session)(contour::actions::ClearHistoryAndReset {}));
        CHECK(historyOf() == history);
        REQUIRE(notices.size() == 1);
        CHECK(notices[0].contains("connection"));
        CHECK(notices[0].contains("clear the history and reset"));
    }
}

TEST_CASE("TerminalSession: a default background the terminal changes reaches the panes bound to it",
          "[contour][session][colors]")
{
    // The pane card, header and background bind session.backgroundColor: a palette that changes without the
    // signal -- OSC 11, or a mirror taking the host's -- leaves them showing the old color until something
    // re-evaluates them, such as a split.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto spy = QSignalSpy(session.get(), &contour::session::TerminalSession::backgroundColorChanged);
    REQUIRE(spy.isValid());

    session->terminal().writeToScreen("\033]11;rgb:12/34/56\033\\");
    QCoreApplication::processEvents();

    CHECK(session->terminal().colorPalette().defaultBackground == vtbackend::RGBColor { 0x123456 });
    CHECK(spy.count() >= 1);
}

TEST_CASE("TerminalSession: a mirrored session states the preference it shows to its host",
          "[contour][session][operations][colors]")
{
    using contour::session::HostedOperation;
    using contour::session::HostedOperationRequest;
    using contour::session::HostedOperationRoute;

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    factory->reportedHosting = contour::session::SessionHosting::Remote;
    factory->hostedOperationRoute = HostedOperationRoute::RunOnHost;
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };
    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);

    auto const operationFor = [](vtbackend::ColorPreference preference) {
        return HostedOperationRequest { .operation = preference == vtbackend::ColorPreference::Dark
                                                         ? HostedOperation::UseDarkColors
                                                         : HostedOperation::UseLightColors };
    };

    SECTION("a session born under the app's preference says so once attached")
    {
        CHECK(factory->requestedHostedOperations
              == std::vector { operationFor(app.app().colorPreference()) });
    }

    SECTION("a change of preference reaches the host, not only the mirror")
    {
        factory->requestedHostedOperations.clear();
        auto const other = app.app().colorPreference() == vtbackend::ColorPreference::Dark
                               ? vtbackend::ColorPreference::Light
                               : vtbackend::ColorPreference::Dark;
        session->updateColorPreference(other);
        CHECK(factory->requestedHostedOperations == std::vector { operationFor(other) });
    }

    SECTION("a local session has no host to tell")
    {
        auto localOwned = std::make_unique<contour::test::MockPtySessionFactory>();
        auto* local = localOwned.get();
        TestApp localApp { std::move(localOwned) };
        contour::test::ScopedController const localWin { localApp.manager() };
        auto* localSession = localApp.manager().createSession(localWin.id);
        REQUIRE(localSession != nullptr);
        localSession->updateColorPreference(vtbackend::ColorPreference::Light);
        localSession->updateColorPreference(vtbackend::ColorPreference::Dark);
        CHECK(local->requestedHostedOperations.empty());
    }
}

TEST_CASE("TerminalSession: a vi mark a mirror asks for is placed where the session is hosted",
          "[contour][session][operations][vi]")
{
    // A mirror's terminal raises the request rather than marking its row
    // (vtbackend::Terminal::toggleUserMark); the session asks its factory where the mark goes. Raised
    // directly here: only a mirror raises it.
    using contour::session::HostedOperation;
    using contour::session::HostedOperationRequest;
    using contour::session::HostedOperationRoute;
    using contour::session::HostedRow;

    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };
    auto* session = app.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    auto& terminal = session->terminal();
    terminal.writeToScreen("one\r\ntwo");
    auto const& grid = terminal.primaryScreen().grid();
    auto const requestFor = [&](vtbackend::UserMarkChange change) {
        return vtbackend::UserMarkRequest { .stableId = grid.stableLineIdOf(vtbackend::LineOffset(1)),
                                            .generation = grid.stableIdGeneration(),
                                            .change = change };
    };
    auto const added = requestFor(vtbackend::UserMarkChange::Added);
    auto const marked = [&] {
        return terminal.primaryScreen().isLogicalLineFlagEnabled(vtbackend::LineOffset(1),
                                                                 vtbackend::LineFlag::UserMark);
    };

    auto notices = std::vector<std::string> {};
    QObject::connect(
        session,
        &contour::session::TerminalSession::showNotification,
        [&](QString const&, QString const& content) { notices.push_back(content.toStdString()); });

    SECTION("a mirrored session's host is asked, by the row, and the line waits for the host's")
    {
        factory->hostedOperationRoute = HostedOperationRoute::RunOnHost;

        session->userMarkRequested(added);
        // Not yet: the request is raised under the terminal lock, and asking the host takes the mirror's.
        CHECK(factory->requestedHostedOperations.empty());
        QCoreApplication::processEvents();

        CHECK(factory->requestedHostedOperations
              == std::vector { HostedOperationRequest {
                  .operation = HostedOperation::SetUserMark,
                  .row = HostedRow { .stableId = added.stableId, .generation = added.generation } } });
        CHECK_FALSE(marked());
        CHECK(notices.empty());
    }

    SECTION("a session that runs here after all places the mark itself, and takes it off")
    {
        session->userMarkRequested(added);
        QCoreApplication::processEvents();
        CHECK(marked());

        session->userMarkRequested(requestFor(vtbackend::UserMarkChange::Removed));
        QCoreApplication::processEvents();
        REQUIRE(factory->requestedHostedOperations.size() == 2);
        CHECK(factory->requestedHostedOperations[1].operation == HostedOperation::ClearUserMark);
        CHECK_FALSE(marked());
    }

    SECTION("a remote end out of reach marks nothing, and says so")
    {
        factory->hostedOperationRoute = HostedOperationRoute::Unreachable;

        session->userMarkRequested(added);
        QCoreApplication::processEvents();
        CHECK_FALSE(marked());
        REQUIRE(notices.size() == 1);
        CHECK(notices[0].contains("mark the line"));
    }
}

TEST_CASE("TerminalSession: a tab refresh a hard reset queued does not outlive the manager",
          "[contour][session][actions]")
{
    // A hard reset withdraws the tab color and the progress bar, and the tab strip learns of it through calls
    // queued to the GUI thread. A session is not deleted with its manager, so a call queued on the SESSION
    // still ran once the manager was gone -- in whichever test pumped the event loop next.
    {
        TestApp app { std::make_unique<contour::test::MockPtySessionFactory>() };
        contour::test::ScopedController const win { app.manager() };
        auto* session = app.manager().createSession(win.id);
        REQUIRE(session != nullptr);
        session->terminal().writeToScreen("\033]9;4;1;50\033\\"); // a progress bar for the reset to withdraw
        CHECK((*session)(contour::actions::ClearHistoryAndReset {}));
    }
    QCoreApplication::processEvents();
    SUCCEED("the queued refreshes were dropped with the manager");
}

TEST_CASE("TerminalSession: the context-menu actions and the state they are gated on",
          "[contour][session][actions][contextmenu]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    SECTION("a fresh session offers nothing to copy")
    {
        auto const state = session->contextMenuState();
        CHECK_FALSE(state.hasSelection);
        CHECK_FALSE(state.hasLastCommand); // no OSC 133 marks in an empty scrollback
        CHECK(state.hyperlinkUnderCursor.empty());
        CHECK(state.activeProfile == session->profileName());
        CHECK_FALSE(state.profileNames.empty());
    }

    SECTION("SelectAll selects, and the state says so")
    {
        for (int i = 0; i < 40; ++i)
            session->terminal().writeToScreen(std::format("line {}\r\n", i));

        CHECK((*session)(contour::actions::SelectAll {}));
        CHECK(session->contextMenuState().hasSelection);
        CHECK(session->terminal().extractSelectionText().contains("line 0"));
    }

    SECTION("a finished OSC 133 command block lights up the last-command rows")
    {
        writeFinishedBlock(*session, "ls", "file1\r\nfile2\r\n");

        CHECK(session->contextMenuState().hasLastCommand);

        auto const textOf = [&](vtbackend::CommandBlockPart part) {
            return session->terminal()
                .commandBlockText(
                    vtbackend::CommandBlockTarget::Last, part, vtbackend::CaptureRendition::PlainText)
                .value_or(std::string {});
        };
        CHECK(textOf(vtbackend::CommandBlockPart::Prompt) == "$ ls");
        CHECK(textOf(vtbackend::CommandBlockPart::Output) == "file1\nfile2");
    }

    SECTION("a shell that never reports a command's start offers no last-command rows")
    {
        // Without ;C nothing ran, so nothing finished: the rows would act on no block and copy nothing.
        session->terminal().writeToScreen("\033]133;A\033\\$ ls\r\nfile1\r\n");
        session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK_FALSE(session->contextMenuState().hasLastCommand);
        CHECK_FALSE((*session)(contour::actions::CopyLastCommandPrompt {}));
    }

    SECTION("a finished block whose rows were cleared away offers no last-command rows")
    {
        // ClearToPrompt keeps the block's record but takes its rows, which are what the menu rows copy.
        writeFinishedBlock(*session, "ls", "file1\r\n");
        REQUIRE(session->contextMenuState().hasLastCommand);
        REQUIRE((*session)(contour::actions::ClearToPrompt {}));
        REQUIRE(session->terminal().commandBlocks().lastFinished() != nullptr);

        CHECK_FALSE(session->contextMenuState().hasLastCommand);
        CHECK_FALSE((*session)(contour::actions::CopyLastCommandBlock {}));
    }

    SECTION("SoftReset keeps the scrollback that a hard reset would wipe")
    {
        for (int i = 0; i < 40; ++i)
            session->terminal().writeToScreen(std::format("line {}\r\n", i));
        REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));

        // DECSTR exits the status display "but not erased" (DEC STD 070 ch. 14 §2.3): the profile's status
        // line stays, and with it the page's height. What matters is that the scrollback SURVIVES.
        CHECK((*session)(contour::actions::SoftReset {}));
        CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
        CHECK(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));
        CHECK(
            session->terminal().primaryScreen().grid().lineText(vtbackend::LineOffset(-1)).contains("line"));

        // A hard reset, by contrast, throws the whole scrollback away. That is the difference the
        // Advanced submenu offers the user, so it is pinned here.
        CHECK((*session)(contour::actions::ClearHistoryAndReset {}));
        CHECK(session->terminal().primaryScreen().historyLineCount() == vtbackend::LineCount(0));
    }

    SECTION("SoftReset leaves the terminal WRAPPING, or it has broken more than it repaired")
    {
        REQUIRE(session->terminal().isModeEnabled(vtbackend::DECMode::AutoWrap));

        CHECK((*session)(contour::actions::SoftReset {}));

        // The VT510 manual has DECSTR clear DECAWM; xterm, foot and wezterm all decline to, and so does
        // Contour. A soft reset is the thing a user reaches for to FIX a garbled terminal, and no shell
        // ever re-enables autowrap on its own — obeying the letter of the spec here would hand back a
        // terminal whose every long line piles up in the last column.
        CHECK(session->terminal().isModeEnabled(vtbackend::DECMode::AutoWrap));
    }

    SECTION("a command that printed nothing does not wipe the clipboard")
    {
        // `cd /tmp` prints not one character. The block exists, its Output is empty — and copying "" would
        // silently replace whatever the user had on their clipboard with nothing at all.
        writeFinishedBlock(*session, "cd /tmp", "");

        // Precondition: there IS a block, and it printed nothing.
        REQUIRE(session->terminal().commandBlocks().lastFinished() != nullptr);
        REQUIRE(session->terminal()
                    .commandBlockText(vtbackend::CommandBlockTarget::Last,
                                      vtbackend::CommandBlockPart::Output,
                                      vtbackend::CaptureRendition::PlainText)
                    .value_or(std::string {})
                    .empty());

        // Declined, rather than "copied" as an empty string.
        CHECK_FALSE((*session)(contour::actions::CopyLastCommandOutput {}));

        // The prompt is there, so that row still has something to give.
        CHECK((*session)(contour::actions::CopyLastCommandPrompt {}));
    }

    SECTION("the hyperlink rows carry the link that was right-clicked, not the one under the pointer now")
    {
        // A menu row must act on what the user CLICKED. The terminal's own idea of "the hyperlink under
        // the cursor" tracks the live mouse position, and the pointer leaves the link the moment it travels
        // to the menu row — so the action carries the URI rather than asking again.
        CHECK((*session)(contour::actions::CopyHyperlink { .uri = "https://contour-terminal.org/" }));

        // Nothing is hovered in this headless session, so an unpinned CopyHyperlink has nothing to copy —
        // which is exactly the state the context menu's rows would have found themselves in.
        CHECK_FALSE((*session)(contour::actions::CopyHyperlink {}));
    }
}

TEST_CASE("TerminalSession: the selection is read aloud through the app's one voice",
          "[contour][session][actions][speech]")
{
    auto speech = std::make_unique<contour::test::RecordingSpeechSynthesizer>();
    auto* const voice = speech.get();
    TestApp testApp(nullptr, nullptr, nullptr, std::move(speech));
    auto session = makeDisplaylessSession(testApp.app());

    SECTION("what is spoken is the prepared text, not the grid")
    {
        for (auto i = 0; i < 3; ++i)
            session->terminal().writeToScreen(std::format("line {}\r\n", i));
        REQUIRE((*session)(contour::actions::SelectAll {}));

        CHECK((*session)(contour::actions::SpeakSelection {}));
        REQUIRE(voice->spoken.size() == 1);
        // Passed through speakableText: the padding every terminal line carries out to the right
        // margin is silence, and the blank rows below the text are not read as pauses.
        CHECK(voice->spoken.front() == "line 0\nline 1\nline 2");
    }

    SECTION("with nothing selected there is nothing to say")
    {
        CHECK_FALSE((*session)(contour::actions::SpeakSelection {}));
        CHECK(voice->spoken.empty());
    }

    SECTION("a machine with no voice neither offers the row nor speaks")
    {
        voice->isAvailable = false;
        session->terminal().writeToScreen("hello\r\n");
        REQUIRE((*session)(contour::actions::SelectAll {}));

        CHECK_FALSE(session->contextMenuState().canSpeak);
        CHECK_FALSE((*session)(contour::actions::SpeakSelection {}));
        CHECK(voice->spoken.empty());
    }

    SECTION("a second session speaks and stops through the same voice")
    {
        // A machine has one voice, so the synthesizer belongs to the app rather than the session.
        // With one engine per session, StopSpeaking in this tab could not stop what another started.
        auto other = makeDisplaylessSession(testApp.app());
        session->terminal().writeToScreen("hello\r\n");
        REQUIRE((*session)(contour::actions::SelectAll {}));
        REQUIRE((*session)(contour::actions::SpeakSelection {}));

        CHECK((*other)(contour::actions::StopSpeaking {}));
        CHECK(voice->stopCount == 1);
        CHECK(voice->spoken.size() == 1);
    }
}

TEST_CASE("TerminalSession: scrollback actions move the viewport over seeded history",
          "[contour][session][actions][scroll]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // Seed more lines than one page so history exists (24-line page).
    for (int i = 0; i < 60; ++i)
        session->terminal().writeToScreen(std::format("line {}\r\n", i));

    auto& viewport = session->terminal().viewport();
    REQUIRE_FALSE(viewport.scrolled());

    CHECK((*session)(contour::actions::ScrollToTop {}));
    CHECK(viewport.scrolled());

    CHECK((*session)(contour::actions::ScrollToBottom {}));
    CHECK_FALSE(viewport.scrolled());

    CHECK((*session)(contour::actions::ScrollUp {}));
    CHECK(viewport.scrolled());
    CHECK((*session)(contour::actions::ScrollDown {}));

    CHECK((*session)(contour::actions::ScrollPageUp {}));
    CHECK((*session)(contour::actions::ScrollPageDown {}));
    CHECK((*session)(contour::actions::ScrollOneUp {}));
    CHECK((*session)(contour::actions::ScrollOneDown {}));
}

TEST_CASE("TerminalSession: opacity actions clamp and notify", "[contour][session][actions]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    auto const initial = session->profile().background.value().opacity;
    CHECK((*session)(contour::actions::DecreaseOpacity {}));
    CHECK(static_cast<uint8_t>(session->profile().background.value().opacity)
          < static_cast<uint8_t>(initial));
    CHECK((*session)(contour::actions::IncreaseOpacity {}));
    CHECK(session->profile().background.value().opacity == initial);

    // Saturation: opacity is already at max (1.0) in the default profile.
    CHECK((*session)(contour::actions::IncreaseOpacity {}));
    CHECK(session->profile().background.value().opacity == initial);
}

TEST_CASE("TerminalSession: a font-size change is session-local and does not leak to another session",
          "[contour][session][font]")
{
    // The data-model half of the "font leaks across tabs" fix: each TerminalSession owns a by-value
    // _profile copy, so changing font size on one session must not touch another. (The observable leak
    // was in the SHARED renderer, re-seeded per session on tab switch — pinned end-to-end by the
    // [display][fonts] session-rebind case in DisplayRendering_test; this guards the data-model
    // isolation the fix relies on, so a future regression that makes the profile shared is caught here.)
    TestApp testApp;
    auto sessionA = makeDisplaylessSession(testApp.app());
    auto sessionB = makeDisplaylessSession(testApp.app());

    auto const baseA = sessionA->profile().fonts.value().size;
    auto const baseB = sessionB->profile().fonts.value().size;
    REQUIRE(baseA.pt == baseB.pt); // both start from the same profile default

    // Grow session A's font a few steps.
    CHECK((*sessionA)(contour::actions::IncreaseFontSize {}));
    CHECK((*sessionA)(contour::actions::IncreaseFontSize {}));
    CHECK(sessionA->profile().fonts.value().size.pt > baseA.pt);

    // Session B is untouched — the change is confined to A's own profile copy.
    CHECK(sessionB->profile().fonts.value().size.pt == baseB.pt);

    // The reverse direction is just as isolated.
    CHECK((*sessionB)(contour::actions::DecreaseFontSize {}));
    CHECK(sessionB->profile().fonts.value().size.pt < baseB.pt);
    CHECK(sessionA->profile().fonts.value().size.pt > baseA.pt); // A still holds its own larger size
}

TEST_CASE("TerminalSession: tab actions from an unregistered session are guarded manager no-ops",
          "[contour][session][actions]")
{
    // Tab actions route through the manager keyed by the acting session\'s model id. A session that
    // was never registered in the vtworkspace model (this fixture) must resolve to no target tab and be a
    // safe no-op — the guard every keybinding rides when a pane acts during teardown.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    CHECK_NOTHROW((*session)(contour::actions::CreateNewTab {}));
    CHECK_NOTHROW((*session)(contour::actions::CloseTab {}));
    CHECK_NOTHROW((*session)(contour::actions::SwitchToTabLeft {}));
    CHECK_NOTHROW((*session)(contour::actions::SwitchToTabRight {}));
    CHECK_NOTHROW((*session)(contour::actions::SwitchToPreviousTab {}));
    CHECK_NOTHROW((*session)(contour::actions::MoveTabToLeft {}));
    CHECK_NOTHROW((*session)(contour::actions::MoveTabToRight {}));
    CHECK_NOTHROW((*session)(contour::actions::SplitVertical {}));
    CHECK_NOTHROW((*session)(contour::actions::SplitHorizontal {}));
    CHECK_NOTHROW((*session)(contour::actions::ClosePane {}));
    CHECK_NOTHROW((*session)(contour::actions::FocusPaneLeft {}));
    CHECK_NOTHROW((*session)(contour::actions::FocusPaneRight {}));
    CHECK_NOTHROW((*session)(contour::actions::FocusPaneUp {}));
    CHECK_NOTHROW((*session)(contour::actions::FocusPaneDown {}));
    CHECK_NOTHROW((*session)(contour::actions::TogglePaneZoom {}));
    CHECK_NOTHROW((*session)(contour::actions::MoveTabTo { .position = 1 }));
    CHECK_NOTHROW((*session)(contour::actions::SwitchToTab { .position = 1 }));
    CHECK_NOTHROW((*session)(contour::actions::SetTabTitle {}));
    CHECK_NOTHROW((*session)(contour::actions::SetTabColor {}));
    CHECK_NOTHROW((*session)(contour::actions::SetTabColor { vtbackend::RGBColor { 0xFF, 0x00, 0x00 } }));
    CHECK_NOTHROW((*session)(contour::actions::ResetTabColor {}));
}

TEST_CASE("TerminalSession: opener, paste and reload actions run without a display",
          "[contour][session][actions]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;
    auto& launcher = testApp.launcher();

    // A working directory on this host, so OpenFileManager has a local folder to open (it now refuses a
    // remote or absent one).
    session->terminal().setCurrentWorkingDirectory(
        "file://" + std::string { contour::test::TestApp::HostName } + "/tmp");

    // The document/URL openers route through the injected ExternalLauncher (no desktop touched).
    CHECK((*session)(actions::OpenConfiguration { .target = actions::ConfigurationTarget::Editor }));
    CHECK((*session)(actions::OpenFileManager {}));
    CHECK((*session)(actions::OpenSelection {}));
    // OpenConfiguration{in_editor} opens the config file URL, OpenFileManager the local cwd, and
    // OpenSelection the (empty) selection.
    CHECK(launcher.openedUrls.size() == 3);

    // OpenConfiguration WITHOUT in_editor opens the app's Settings window instead, hosting window or none:
    // no URL is launched.
    CHECK((*session)(actions::OpenConfiguration {}));
    CHECK(launcher.openedUrls.size() == 3);

    // FollowHyperlink with nothing hovered/selected returns false (no target).
    CHECK_FALSE((*session)(actions::FollowHyperlink {}));

    // Paste actions route to the clipboard paste path (empty clipboard is a safe no-op).
    CHECK((*session)(actions::PasteClipboard {}));
    CHECK((*session)(actions::PasteSelection {}));

    // Config reload re-reads the (unchanged) config file.
    CHECK_NOTHROW((*session)(actions::ReloadConfig {}));

    // ScreenshotVT writes the screen's VT dump to a file (display-independent); CreateDebugDump
    // runs the terminal's inspect(). Both are headless-safe.
    CHECK((*session)(actions::ScreenshotVT {}));
    CHECK((*session)(actions::CreateDebugDump {}));

    // Search-highlight navigation actions with no active search are safe.
    CHECK_NOTHROW((*session)(actions::FocusNextSearchMatch {}));
    CHECK_NOTHROW((*session)(actions::FocusPreviousSearchMatch {}));
    CHECK_NOTHROW((*session)(actions::NoSearchHighlight {}));
}

TEST_CASE("TerminalSession: window title flows from OSC to title() and resolvedWindowTitle",
          "[contour][session][title]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    session->terminal().writeToScreen("\033]0;my-title\033\\");
    CHECK(session->resolvedWindowTitle() == "my-title");
    // title() decorates for the window frame ("<title> - Contour (DEBUG)" in debug builds);
    // only the resolved-title prefix is contractual here.
    CHECK(session->title().toStdString().starts_with("my-title"));
}

TEST_CASE("TerminalSession: display-dependent requests are safe no-ops without a display",
          "[contour][session][guards]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    CHECK_NOTHROW(session->requestWindowResize(vtbackend::LineCount(30), vtbackend::ColumnCount(90)));
    CHECK_NOTHROW(session->requestWindowResize(vtbackend::Width(800), vtbackend::Height(600)));
    CHECK_NOTHROW(session->resizeTerminalToDisplaySize());
    CHECK_NOTHROW(session->scheduleRedraw());

    // Pending-permission executors with nothing pending are guarded no-ops.
    CHECK_NOTHROW(session->applyPendingFontChange(PermissionDecision::Allow, PermissionMemory::Once));
    CHECK_NOTHROW(session->applyPendingPaste(PermissionDecision::Deny, PermissionMemory::Once));
    CHECK_NOTHROW(session->executePendingBufferCapture(PermissionDecision::Deny, PermissionMemory::Once));
}

TEST_CASE("TerminalSession: pasteFromClipboard writes clipboard text and enforces the size guards",
          "[contour][session][clipboard]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    clipboard->setText(QStringLiteral("pasted"));
    session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("pasted"));

    // > 1 MB: hard-rejected — and (regression) must not crash on a display-less session.
    mockPtyOf(*session).stdinBuffer().clear();
    clipboard->setText(QString((1024 * 1024) + 1, QChar('x')));
    CHECK_NOTHROW(session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep));
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // > 512 KB: requires permission; nothing written until granted.
    int permissionRequests = 0;
    contour::test::countAsks(*session,
                             contour::session::PermissionAsk { contour::session::GuardedRole::BigPaste },
                             permissionRequests);
    clipboard->setText(QString((1024 * 512) + 1, QChar('y')));
    session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
    CHECK(permissionRequests == 1);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // "Yes to all" applies the paste and stores the verdict — and (#2089) the next big paste must resolve
    // from that memory rather than asking a second time. Only requestPermission() reads the remembered
    // answer, so a bare emit here would ask again forever.
    session->applyPendingPaste(PermissionDecision::Allow, PermissionMemory::Session);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("yyy"));

    mockPtyOf(*session).stdinBuffer().clear();
    session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
    CHECK(permissionRequests == 1);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("yyy"));

    clipboard->clear();
}

TEST_CASE("TerminalSession: an approved large paste sends what was approved, not what the clipboard "
          "holds later",
          "[contour][session][clipboard][permission]")
{
    // A >512 KB paste is held until the user answers, and the answer applies to the payload that was
    // measured and shown — not to a fresh read of the clipboard, which may hold something else by then.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    SECTION("the payload is normalized the same way a small paste is")
    {
        // A large paste must be normalized exactly as the immediate path normalizes a small one: under
        // bracketed paste a stray CR reads as Enter, so a CRLF script that skipped the normalization
        // would EXECUTE line by line instead of being inserted. What that normalization *does* is
        // platform-dependent -- on Windows the clipboard resolves CRLF itself, so normalizeCrlf() is
        // deliberately a no-op there -- so the small paste establishes the expectation rather than a
        // hard-coded string.
        auto const line = QStringLiteral("echo hello\r\n");

        clipboard->setText(line);
        session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
        auto const smallPaste = mockPtyOf(*session).stdinBuffer();
        REQUIRE_FALSE(smallPaste.empty());

        // Sized against the *normalized* line: normalization runs before the soft limit is measured, so
        // a count derived from the CRLF length lands under that limit wherever CRLF becomes LF -- and
        // the paste would take the immediate path, leaving the deferred one untested.
        auto const repeats = static_cast<int>(((size_t { 1024 } * 512) / smallPaste.size()) + 1);
        mockPtyOf(*session).stdinBuffer().clear();
        clipboard->setText(line.repeated(repeats));
        session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
        REQUIRE(mockPtyOf(*session).stdinBuffer().empty()); // held for the verdict, not sent

        session->applyPendingPaste(PermissionDecision::Allow, PermissionMemory::Once);

        auto expected = std::string {};
        expected.reserve(smallPaste.size() * static_cast<size_t>(repeats));
        for ([[maybe_unused]] auto const _: std::views::iota(0, repeats))
            expected += smallPaste;

        auto const& written = mockPtyOf(*session).stdinBuffer();
        CHECK(written.size() == expected.size());
        // Parenthesized so Catch2 reports a bare false: decomposing this would print both operands, and
        // both are half a megabyte of shell script.
        CHECK((written == expected));
    }

    SECTION("swapping the clipboard while the dialog is up does not swap the paste")
    {
        clipboard->setText(QString((1024 * 512) + 1, QChar('y')));
        session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);

        // The user is looking at a dialog about the 'y' payload; something else lands on the clipboard.
        clipboard->setText(QStringLiteral("substituted"));
        session->applyPendingPaste(PermissionDecision::Allow, PermissionMemory::Once);

        auto const& written = mockPtyOf(*session).stdinBuffer();
        CHECK(written.contains("yyy"));
        CHECK_FALSE(written.contains("substituted"));
    }

    SECTION("a remembered refusal keeps refusing")
    {
        auto asks = 0;
        contour::test::countAsks(
            *session, contour::session::PermissionAsk { contour::session::GuardedRole::BigPaste }, asks);

        clipboard->setText(QString((1024 * 512) + 1, QChar('y')));
        session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
        REQUIRE(asks == 1);

        session->applyPendingPaste(PermissionDecision::Deny, PermissionMemory::Session);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());

        session->pasteFromClipboard(1, vtbackend::PasteStrip::Keep);
        CHECK(asks == 1); // remembered: no second dialog
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }

    clipboard->clear();
}

TEST_CASE("TerminalSession: manager creates real tabs headlessly through the injected PTY factory",
          "[contour][session][manager][factory]")
{
    // The composition-root DI seam: with a MockPtySessionFactory injected, the manager's
    // session-creation paths (createNewTab -> createSessionInBackground -> createBackingSession)
    // run end-to-end without spawning processes.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp testApp(std::move(factoryOwned));
    contour::test::ScopedController const controller(testApp.manager());

    controller->createNewTab();
    controller->createNewTab();

    CHECK(factory->createdPtys.size() == 2);
    CHECK(controller->count() == 2);

    // Terminate every created session (deliberate close: PTY devices must close).
    contour::test::closeAllTabs(*controller);
    for (auto* pty: factory->createdPtys)
        CHECK(pty->isClosed());
}

TEST_CASE("TerminalSession: display-independent actions dispatch without a display",
          "[contour][session][actions]")
{
    // Broad executeAction() coverage over the handlers that only touch terminal/session state (no
    // display, no window, no GUI event loop). Each must run and return without crashing; where the
    // effect is observable in terminal state, assert it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    for (int i = 0; i < 60; ++i)
        session->terminal().writeToScreen(std::format("row {}\r\n", i));

    // Scroll-by-mark navigation.
    CHECK_NOTHROW((*session)(actions::ScrollMarkUp {}));
    CHECK_NOTHROW((*session)(actions::ScrollMarkDown {}));

    // Selection lifecycle: create, cancel (no-op when nothing selected), clear.
    CHECK_NOTHROW((*session)(actions::CancelSelection {}));

    // Search highlight clearing.
    CHECK_NOTHROW((*session)(actions::NoSearchHighlight {}));
    CHECK_NOTHROW((*session)(actions::FocusNextSearchMatch {}));
    CHECK_NOTHROW((*session)(actions::FocusPreviousSearchMatch {}));

    // Key-mapping toggle flips a session flag (observable) and back.
    CHECK((*session)(actions::ToggleAllKeyMaps {}));
    CHECK((*session)(actions::ToggleAllKeyMaps {}));

    // Input-protection toggle.
    CHECK_NOTHROW((*session)(actions::ToggleInputProtection {}));
    CHECK_NOTHROW((*session)(actions::ToggleInputProtection {}));

    // Status-line toggle flips the display type between Indicator and None.
    auto const initialStatus = session->terminal().statusDisplayType();
    CHECK((*session)(actions::ToggleStatusLine {}));
    CHECK(session->terminal().statusDisplayType() != initialStatus);
    CHECK((*session)(actions::ToggleStatusLine {}));
    CHECK(session->terminal().statusDisplayType() == initialStatus);

    // Font size actions mutate the profile's font size (display-independent staging).
    auto const baseFont = session->profile().fonts.value().size;
    CHECK((*session)(actions::IncreaseFontSize {}));
    CHECK(session->profile().fonts.value().size.pt > baseFont.pt);
    CHECK((*session)(actions::DecreaseFontSize {}));
    CHECK((*session)(actions::ResetFontSize {}));

    // Hard reset.
    CHECK((*session)(actions::ClearHistoryAndReset {}));

    // Vi normal mode toggle + back to insert.
    CHECK_NOTHROW((*session)(actions::ViNormalMode {}));

    // Trace-mode stepping (execution mode control; no display needed).
    CHECK_NOTHROW((*session)(actions::TraceEnter {}));
    CHECK_NOTHROW((*session)(actions::TraceStep {}));
    CHECK_NOTHROW((*session)(actions::TraceBreakAtEmptyQueue {}));
    CHECK_NOTHROW((*session)(actions::TraceLeave {}));

    // Copy actions with no selection are safe no-ops.
    CHECK_NOTHROW((*session)(actions::CopySelection {}));
    CHECK_NOTHROW((*session)(actions::CopyPreviousMarkRange {}));

    // Opacity actions mutate the profile background opacity (display-independent). Decrement from
    // the default (which is max), then increment back — a round-trip that stays within range and
    // exercises both handlers' effective (non-clamped) paths.
    auto const baseOpacity = session->profile().background.value().opacity;
    CHECK((*session)(actions::DecreaseOpacity {}));
    CHECK(session->profile().background.value().opacity != baseOpacity);
    CHECK((*session)(actions::IncreaseOpacity {}));
    CHECK(session->profile().background.value().opacity == baseOpacity);

    // Profile switch: switching to the loaded "main" profile re-applies it; an unknown name hits
    // activateProfile's "no such profile" guard. The action is consumed (true) either way.
    CHECK((*session)(actions::ChangeProfile { .name = "main" }));
    CHECK((*session)(actions::ChangeProfile { .name = "no-such-profile" }));

    // Focus in/out events are display-independent (the display-touching parts are guarded).
    CHECK_NOTHROW(session->sendFocusInEvent());
    CHECK_NOTHROW(session->sendFocusOutEvent());

    // Selection creation with custom delimiters, then a mark-range copy.
    CHECK_NOTHROW((*session)(actions::CreateSelection { .delimiters = " " }));

    // Reset-config action re-reads the (unchanged) config file.
    CHECK_NOTHROW((*session)(actions::ResetConfig {}));

    // Write-screen and send-chars actions drive the PTY / screen.
    auto& pty = mockPtyOf(*session);
    pty.stdinBuffer().clear();
    CHECK((*session)(actions::SendChars { .chars = "abc" }));
    CHECK(pty.stdinBuffer().contains("abc"));
    CHECK((*session)(actions::WriteScreen { .chars = "xyz" }));

    // Clipboard write action (no display: routed but harmless).
    CHECK_NOTHROW((*session)(actions::PasteSelection {}));
}

TEST_CASE("TerminalSession: a modifier-bound char dispatches its action instead of writing to the PTY",
          "[contour][session][input]")
{
    // Ctrl+0 is bound to ResetFontSize in the default char mappings with no mode restriction, so
    // sending it runs the char keybinding-dispatch path (apply -> executeAllActions) and
    // consumes the character: nothing reaches the PTY.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    auto& pty = mockPtyOf(*session);
    pty.stdinBuffer().clear();

    session->sendCharEvent(U'0',
                           vtbackend::KeyIdentity { .unshiftedKey = U'0' },
                           Modifiers { vtbackend::Modifier::Control },
                           KeyboardEventType::Press,
                           now);
    // The bound action consumed the char: PTY stays empty (an unbound char would echo a byte).
    CHECK(pty.stdinBuffer().empty());
}

TEST_CASE("TerminalSession: scroll actions move the viewport over seeded history without a display",
          "[contour][session][actions]")
{
    // The full family of viewport-scrolling actions is display-independent (they mutate the
    // terminal's viewport/smooth-scroll state). Seed enough history that scrolling up has somewhere
    // to go, then assert the direction of viewport movement where observable.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    for (int i = 0; i < 200; ++i)
        session->terminal().writeToScreen(std::format("history row {}\r\n", i));

    auto& vp = session->terminal().viewport();

    // From the bottom, page/line/mark up all scroll into history (offset grows toward history top).
    (*session)(actions::ScrollToBottom {});
    CHECK((*session)(actions::ScrollPageUp {}));
    CHECK((*session)(actions::ScrollOneUp {}));
    CHECK((*session)(actions::ScrollUp {}));
    CHECK(vp.scrollOffset().value > 0);

    // ...and the down family walks it back toward the bottom.
    CHECK((*session)(actions::ScrollPageDown {}));
    CHECK((*session)(actions::ScrollOneDown {}));
    CHECK((*session)(actions::ScrollDown {}));

    // Snap-to-edge actions reset smooth scroll and jump.
    CHECK((*session)(actions::ScrollToTop {}));
    CHECK(vp.scrollOffset().value > 0);
    CHECK((*session)(actions::ScrollToBottom {}));

    // Mark navigation and reverse-search entry are display-independent no-throws.
    CHECK((*session)(actions::ScrollMarkUp {}));
    CHECK((*session)(actions::ScrollMarkDown {}));
    CHECK_NOTHROW((*session)(actions::SearchReverse {}));
}

TEST_CASE("TerminalSession: folding actions collapse and expand command output",
          "[contour][session][actions][folding]")
{
    // The five folding actions, driven end to end against a session whose scrollback holds two commands
    // marked up exactly as a shell with OSC 133 integration would mark them.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    auto const runCommand = [&](std::string const& command, int outputLines) {
        session->terminal().writeToScreen(vtbackend::test::commandRun(command, numberedOutput(outputLines)));
    };

    runCommand("ls", 3);
    runCommand("pwd", 2);

    auto& terminal = session->terminal();
    REQUIRE(terminal.foldRanges().size() == 2);
    REQUIRE(terminal.foldState().empty());

    SECTION("CollapseLastFold hides the most recent command, and is idempotent")
    {
        CHECK((*session)(actions::CollapseLastFold {}));
        CHECK(terminal.hiddenLineCount() > vtbackend::LineCount(0));
        // Already collapsed: nothing to do, and it says so.
        CHECK(!(*session)(actions::CollapseLastFold {}));
    }

    SECTION("ToggleLastFold goes both ways")
    {
        CHECK((*session)(actions::ToggleLastFold {}));
        auto const hidden = terminal.hiddenLineCount();
        CHECK(hidden > vtbackend::LineCount(0));

        CHECK((*session)(actions::ToggleLastFold {}));
        CHECK(terminal.hiddenLineCount() == vtbackend::LineCount(0));
    }

    SECTION("CollapseAllFolds then ExpandAllFolds returns to where it started")
    {
        CHECK((*session)(actions::CollapseAllFolds {}));
        auto const allHidden = terminal.hiddenLineCount();
        CHECK(allHidden > vtbackend::LineCount(0));
        // Everything is already collapsed, so a second sweep changes nothing.
        CHECK(!(*session)(actions::CollapseAllFolds {}));

        CHECK((*session)(actions::ExpandAllFolds {}));
        CHECK(terminal.hiddenLineCount() == vtbackend::LineCount(0));
        CHECK(!(*session)(actions::ExpandAllFolds {}));
    }

    SECTION("the vi z keys typed in normal mode fold through the session's key path")
    {
        terminal.inputHandler().setMode(vtbackend::ViMode::Normal);

        typeKeys(*session, "zM");
        for (auto const& range: terminal.foldRanges())
            CHECK(terminal.foldState().isCollapsed(range.headStableId));
        CHECK(mockPtyOf(*session).stdinBuffer().empty());

        typeKeys(*session, "zR");
        CHECK(terminal.foldState().empty());
    }

    SECTION("ToggleFold acts on the block at the top of the viewport")
    {
        // Nothing is at the viewport top but the oldest prompt, so this is the fold that toggles.
        (*session)(actions::ScrollToTop {});
        auto const marker =
            terminal.foldMarkerAt(terminal.viewport().translateScreenToGridLine(vtbackend::LineOffset(0)));
        if (marker != vtbackend::FoldMarker::None)
        {
            CHECK((*session)(actions::ToggleFold {}));
            CHECK(terminal.hiddenLineCount() > vtbackend::LineCount(0));
        }
    }
}

TEST_CASE("TerminalSession: scrollbar marks follow the command blocks, and a click jumps to one",
          "[contour][session][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();

    terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
    terminal.writeToScreen(vtbackend::test::commandRun("ls", numberedOutput(3), 0));
    writeFiller(terminal, 60);

    pane->setScrollbarMarkTrackPixels(200);
    pane->refreshScrollbarMarks();

    REQUIRE(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::Failure, ScrollbarMarkKind::Command });
    CHECK(markPosition(*pane, 0) < markPosition(*pane, 1));
    CHECK(markPosition(*pane, 1) < 1.0);
    CHECK(pane->scrollbarMarkColors().size()
          == static_cast<qsizetype>(vtbackend::ScrollbarMarkKindList.size()));

    auto const& grid = terminal.primaryScreen().grid();
    auto const target = markTarget(*pane, 0);

    SECTION("a click puts that block's head on the viewport's top row")
    {
        pane->scrollToScrollbarMark(target);
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("a click on a collapsed block opens it as well")
    {
        REQUIRE(terminal.foldRanges().size() == 2);
        terminal.foldState().collapse(target);
        pane->scrollToScrollbarMark(target);
        CHECK_FALSE(terminal.foldState().isCollapsed(target));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("a tick published before a reflow is refused rather than misread")
    {
        // The column change renamed every row, so the id the tick carries would now name some other line.
        terminal.resizeScreen(vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(60) });
        auto const before = terminal.viewport().scrollOffset();
        pane->scrollToScrollbarMark(target);
        CHECK(terminal.viewport().scrollOffset() == before);

        // The refresh the reflow asks for publishes ticks a click follows again.
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 2);
        pane->scrollToScrollbarMark(markTarget(*pane, 0));
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == markTarget(*pane, 0));
    }

    SECTION("the alternate screen shows none, and leaving it brings them back")
    {
        terminal.writeToScreen("\033[?1049h");
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().isEmpty());

        terminal.writeToScreen("\033[?1049l");
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().size() == 2);
    }

    SECTION("a refresh that finds nothing moved announces nothing; a new track height does")
    {
        auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 0);

        pane->setScrollbarMarkTrackPixels(300);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 1);
    }

    SECTION("a refresh whose inputs moved but whose ticks did not announces nothing")
    {
        auto const colors = pane->scrollbarMarkColors();
        auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);

        // A prompt nothing has run from yet: one record more in the store, and no tick for it.
        auto const blockRevision = terminal.commandBlocks().revision();
        terminal.writeToScreen(vtbackend::test::PromptStart);
        REQUIRE(terminal.commandBlocks().revision() != blockRevision);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 0);

        // The history growing under them moves both ticks, in the colours they had: those are not rebuilt.
        writeFiller(terminal, 60);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 1);
        CHECK(pane->scrollbarMarkColors().isSharedWith(colors));

        // A track a pixel taller buckets the ticks onto the same pixels, which are other fractions of it now.
        auto const position = markPosition(*pane, 1);
        pane->setScrollbarMarkTrackPixels(201);
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 2);
        CHECK(markPosition(*pane, 1) != position);
    }

    SECTION("a reflow that drops the folds is announced once, not again by the refresh after it")
    {
        // The reflow renamed every row, so the collapsed fold is forgotten -- by whichever read first
        // reconciles the fold state. The refresh must be that read, or the next one finds it moved.
        terminal.foldState().collapse(target);
        pane->refreshScrollbarMarks();
        auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);

        terminal.resizeScreen(vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(60) });
        pane->refreshScrollbarMarks();
        pane->refreshScrollbarMarks();
        CHECK(spy.count() == 1);
    }

    SECTION("ClearToPrompt drops the ticks of the blocks it removed")
    {
        // A third command still running: everything above its prompt goes, make's block and ls's with it.
        terminal.writeToScreen(vtbackend::test::commandStarted("tail -f log"));
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 3);

        REQUIRE((*pane)(contour::actions::ClearToPrompt {}));
        pane->refreshScrollbarMarks();
        CHECK(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::Running });
    }
}

TEST_CASE("TerminalSession: a running block's tick is hollow, and the others are solid",
          "[contour][session][scrollbar-marks]")
{
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
    terminal.writeToScreen(vtbackend::test::commandRun("ls", numberedOutput(3), 0));
    terminal.writeToScreen(vtbackend::test::commandStarted("tail -f log"));
    writeFiller(terminal, 60);
    pane->setScrollbarMarkTrackPixels(200);
    pane->refreshScrollbarMarks();

    REQUIRE(markKindsOf(*pane)
            == std::vector {
                ScrollbarMarkKind::Failure, ScrollbarMarkKind::Command, ScrollbarMarkKind::Running });
    for (auto const index: std::views::iota(qsizetype { 0 }, pane->scrollbarMarks().size()))
    {
        INFO("tick " << index);
        auto const kind = markKindsOf(*pane).at(static_cast<size_t>(index));
        CHECK(markStyle(*pane, index) == static_cast<int>(vtbackend::scrollbarMarkKindRow(kind).style));
    }
    CHECK(markStyle(*pane, 2) == static_cast<int>(vtbackend::ScrollbarTickStyle::Hollow));
    CHECK(markStyle(*pane, 0) == static_cast<int>(vtbackend::ScrollbarTickStyle::Solid));
}

TEST_CASE("TerminalSession: a profile without the commands source leaves a finished success unticked",
          "[contour][session][scrollbar-marks]")
{
    // Native2026's default marks: failures, running commands and search matches. A success leaves none.
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(
            testApp.app(),
            "native",
            contour::config::editionDefaults(contour::config::DefaultsEdition::Native2026).scrollBarMarks));
    auto& terminal = pane->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("ls", numberedOutput(3), 0));
    terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
    terminal.writeToScreen(vtbackend::test::commandStarted("tail -f log"));
    writeFiller(terminal, 60);
    pane->setScrollbarMarkTrackPixels(200);
    pane->refreshScrollbarMarks();

    CHECK(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::Failure, ScrollbarMarkKind::Running });
}

TEST_CASE("TerminalSession: no shell integration, or scrollbar.marks: [], leaves the scrollbar unmarked",
          "[contour][session][scrollbar-marks]")
{
    TestApp testApp;

    SECTION("plain output carries no marks, and the scrollbar's travel is published all the same")
    {
        auto pane = makeSessionWithSurface(
            testApp.app(),
            registerMarkedScrollbarProfile(testApp.app(), "plain", vtbackend::AllScrollbarMarkSources));
        writeFiller(pane->terminal(), 60);

        pane->setScrollbarMarkTrackPixels(200);
        pane->refreshScrollbarMarks();

        CHECK(pane->scrollbarMarks().isEmpty());
        CHECK(pane->historyLineCount() > 0);
        CHECK(pane->historyLineCount() == unbox<int>(pane->terminal().viewport().scrollableLineCount()));
    }

    SECTION("[] hides every tick even with blocks present")
    {
        auto pane = makeSessionWithSurface(
            testApp.app(),
            registerMarkedScrollbarProfile(testApp.app(), "unmarked", vtbackend::ScrollbarMarkSources {}));
        pane->terminal().writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));

        pane->setScrollbarMarkTrackPixels(200);
        pane->refreshScrollbarMarks();

        CHECK(pane->scrollbarMarks().isEmpty());
    }

    SECTION("a hidden scrollbar draws no ticks either")
    {
        auto const hidden = registerProfile(testApp.app(), "hidden", [](contour::config::TerminalProfile& p) {
            auto scrollbar = p.scrollbar.value();
            scrollbar.position = contour::config::ScrollBarPosition::Hidden;
            p.scrollbar = scrollbar;
        });
        auto pane = makeSessionWithSurface(testApp.app(), hidden);
        pane->terminal().writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));

        pane->setScrollbarMarkTrackPixels(200);
        pane->refreshScrollbarMarks();

        CHECK(pane->scrollbarMarks().isEmpty());
    }
}

TEST_CASE("TerminalSession: a scrollbar marks refresh notices every input the ticks are drawn from",
          "[contour][session][scrollbar-marks]")
{
    // Each section moves one input and nothing else the ticks depend on, then asks for the ticks again: a
    // refresh that missed the move would hand back the ticks it published before.
    using vtbackend::ScrollbarMarkKind;

    TestApp testApp;
    // Registered before the session is made: it takes its own copy of the configuration.
    auto const unmarked =
        registerMarkedScrollbarProfile(testApp.app(), "unmarked", vtbackend::ScrollbarMarkSources {});
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    pane->setScrollbarMarkTrackPixels(200);

    SECTION("a command finishing")
    {
        terminal.writeToScreen(vtbackend::test::commandStarted("make"));
        pane->refreshScrollbarMarks();
        REQUIRE(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::Running });

        terminal.writeToScreen(vtbackend::test::commandEnd(2));
        pane->refreshScrollbarMarks();
        CHECK(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::Failure });
    }

    SECTION("one fold closing as another opens")
    {
        // As many rows hidden before as after, so only which ones moved: every tick below the first fold.
        for (auto const command: { "one", "two", "three" })
            terminal.writeToScreen(vtbackend::test::commandRun(command, numberedOutput(3), 0));
        writeFiller(terminal, 40);
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 3);
        auto const one = markTarget(*pane, 0);
        auto const two = markTarget(*pane, 1);

        terminal.foldState().collapse(one);
        auto const hidden = terminal.hiddenLineCount();
        pane->refreshScrollbarMarks();
        auto const twoBelowAFold = markPosition(*pane, 1);

        terminal.foldState().expand(one);
        terminal.foldState().collapse(two);
        REQUIRE(terminal.hiddenLineCount() == hidden);
        pane->refreshScrollbarMarks();
        CHECK(markPosition(*pane, 1) > twoBelowAFold);
    }

    SECTION("a vi mark placed, and an erase that takes it")
    {
        terminal.writeToScreen("one\r\ntwo\r\n\033[2;1H");
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().isEmpty());

        markCursorLine(terminal); // on "two"
        pane->refreshScrollbarMarks();
        REQUIRE(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::UserMark });

        // ED resets the rows below the cursor whole, the mark's flag with them, and tells the index nothing.
        terminal.writeToScreen("\033[H\033[J");
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().isEmpty());
    }

    SECTION("a reflow that hands every row its id again")
    {
        // No block to re-anchor, and lines no narrower window rewraps: the vi mark keeps its id, so its index
        // is not rebuilt into a new revision, and the generation is all that moves. The tick stands where it
        // stood, and a click on it lands under the generation the refresh read.
        terminal.writeToScreen("one\r\ntwo\r\n\033[2;1H");
        markCursorLine(terminal); // on "two"
        terminal.writeToScreen("\033[3;1H");
        writeFiller(terminal, 60);
        pane->refreshScrollbarMarks();
        REQUIRE(markKindsOf(*pane) == std::vector { ScrollbarMarkKind::UserMark });
        auto const target = markTarget(*pane, 0);
        auto const& grid = terminal.primaryScreen().grid();
        auto const generation = grid.stableIdGeneration();

        terminal.resizeScreen(
            vtbackend::PageSize { terminal.totalPageSize().lines, vtbackend::ColumnCount(60) });
        REQUIRE(grid.stableIdGeneration() != generation);
        pane->refreshScrollbarMarks();
        REQUIRE(markTarget(*pane, 0) == target);

        pane->scrollToScrollbarMark(target);
        CHECK(grid.stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("the history growing")
    {
        writeFiller(terminal, 30);
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 0));
        writeFiller(terminal, 30);
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 1);
        auto const before = markPosition(*pane, 0);

        writeFiller(terminal, 30);
        pane->refreshScrollbarMarks();
        CHECK(markPosition(*pane, 0) < before);
    }

    SECTION("rows evicted from a full history")
    {
        // The history stays as deep, so only the oldest row moved: every tick slides up the track.
        auto shallow = makeSessionWithSurface(
            testApp.app(),
            registerMarkedScrollbarProfile(
                testApp.app(), "shallow", vtbackend::AllScrollbarMarkSources, vtbackend::LineCount(20)));
        auto& shallowTerminal = shallow->terminal();
        writeFiller(shallowTerminal, 30);
        shallowTerminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 0));
        writeFiller(shallowTerminal, 15);
        shallow->setScrollbarMarkTrackPixels(200);
        shallow->refreshScrollbarMarks();
        REQUIRE(shallow->scrollbarMarks().size() == 1);
        auto const before = markPosition(*shallow, 0);
        auto const travel = shallowTerminal.viewport().scrollableLineCount();
        auto const floor = shallowTerminal.primaryScreen().grid().stableRangeFloor();

        writeFiller(shallowTerminal, 5);
        REQUIRE(shallowTerminal.viewport().scrollableLineCount() == travel);
        REQUIRE(shallowTerminal.primaryScreen().grid().stableRangeFloor() > floor);
        shallow->refreshScrollbarMarks();
        REQUIRE(shallow->scrollbarMarks().size() == 1);
        CHECK(markPosition(*shallow, 0) < before);
    }

    SECTION("the cursor leaving for another page")
    {
        // DECPCCM reset: page 0 stays on display, and the viewport, the window onto it, still scrolls to a
        // tick wherever the cursor writes.
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 0));
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 1);

        terminal.writeToScreen("\033[?64l");
        terminal.setPage(vtbackend::PageIndex(3), vtbackend::PageCursorPlacement::Home);
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().size() == 1);
    }

    SECTION("another page put on display")
    {
        // The cursor comes back to page 0, but page 3 stays on display: the ticks would point into a page
        // nobody sees.
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 0));
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 1);

        terminal.setPage(vtbackend::PageIndex(3), vtbackend::PageCursorPlacement::Home);
        terminal.writeToScreen("\033[?64l");
        terminal.setPage(vtbackend::PageIndex(0), vtbackend::PageCursorPlacement::PagesOwn);
        REQUIRE(terminal.isPrimaryScreen());
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().isEmpty());
    }

    SECTION("a profile that switches scrollbar.marks off")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 0));
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarks().size() == 1);

        REQUIRE((*pane)(contour::actions::ChangeProfile { unmarked }));
        REQUIRE(pane->profileName() == unmarked);
        pane->refreshScrollbarMarks();
        CHECK(pane->scrollbarMarks().isEmpty());
    }

    SECTION("a palette change")
    {
        // A failure's tick derives from the palette's red unless the scheme names a colour of its own.
        pane->refreshScrollbarMarks();
        REQUIRE(pane->scrollbarMarkColors().size()
                == static_cast<qsizetype>(vtbackend::ScrollbarMarkKindList.size()));
        REQUIRE(markColor(*pane, ScrollbarMarkKind::Failure) != QColor(0x12, 0x34, 0x56));

        terminal.writeToScreen("\033]4;1;rgb:12/34/56\033\\");
        pane->refreshScrollbarMarks();
        CHECK(markColor(*pane, ScrollbarMarkKind::Failure) == QColor(0x12, 0x34, 0x56));
    }
}

TEST_CASE("TerminalSession: scrollbar marks are rebuilt a frame after they are asked for, and once",
          "[contour][session][scrollbar-marks]")
{
    TestApp testApp;
    // Registered before the session is made: it takes its own copy of the configuration.
    auto const unmarked =
        registerMarkedScrollbarProfile(testApp.app(), "unmarked", vtbackend::ScrollbarMarkSources {});
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    pane->setScrollbarMarkTrackPixels(200);

    // Whatever attaching the display asked for, done before anything is counted.
    auto settled = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
    REQUIRE(settled.wait(1000));

    auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);

    SECTION("a burst of output costs one rebuild, done on the GUI thread")
    {
        for (auto const i: std::views::iota(0, 20))
            terminal.writeToScreen(
                vtbackend::test::commandRun(std::format("cmd {}", i), numberedOutput(1), 0));
        CHECK(spy.count() == 0);

        REQUIRE(spy.wait(1000));
        QTest::qWait(50);
        CHECK(spy.count() == 1);
        CHECK(pane->scrollbarMarks().size() == 20);
    }

    SECTION("the end of a synchronized update, raised under the terminal lock, only asks for one")
    {
        terminal.writeToScreen("\033[?2026h" + vtbackend::test::commandRun("make", numberedOutput(3), 2)
                               + "\033[?2026l");
        REQUIRE(spy.wait(1000));
        CHECK(markKindsOf(*pane) == std::vector { vtbackend::ScrollbarMarkKind::Failure });
    }

    SECTION("a folding action asks for one")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("one", numberedOutput(3), 0));
        terminal.writeToScreen(vtbackend::test::commandRun("two", numberedOutput(3), 0));
        REQUIRE(spy.wait(1000));
        auto const before = markPosition(*pane, 1);

        REQUIRE((*pane)(contour::actions::CollapseAllFolds {}));
        REQUIRE(spy.wait(1000));
        CHECK(markPosition(*pane, 1) < before);
    }

    // Each section below settles the output it set up first, then acts with no output after it: only the
    // action itself can ask for the refresh it waits for.

    SECTION("a reflow asks for one, and a click on the tick it publishes lands")
    {
        // Lines wider than the window is about to be, so the reflow rewraps them and the tick moves.
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
        for ([[maybe_unused]] auto const i: std::views::iota(0, 40))
            terminal.writeToScreen(std::string(70, 'w') + "\r\n");
        REQUIRE(spy.wait(1000));

        // A narrower window renames every row, while the program in the pane prints nothing.
        terminal.resizeScreen(vtbackend::PageSize { vtbackend::LineCount(24), vtbackend::ColumnCount(60) });
        REQUIRE(spy.wait(1000));
        REQUIRE(pane->scrollbarMarks().size() == 1);
        auto const target = markTarget(*pane, 0);
        pane->scrollToScrollbarMark(target);
        CHECK(terminal.primaryScreen().grid().stableLineIdOf(terminal.viewport().topLine()) == target);
    }

    SECTION("a reset that clears the history asks for one")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
        writeFiller(terminal, 60);
        REQUIRE(spy.wait(1000));
        REQUIRE(pane->scrollbarMarks().size() == 1);

        REQUIRE((*pane)(contour::actions::ClearHistoryAndReset {}));
        REQUIRE(spy.wait(1000));
        CHECK(pane->scrollbarMarks().isEmpty());
        CHECK(pane->historyLineCount() == 0);
    }

    SECTION("a profile switch refreshes them at once")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
        REQUIRE(spy.wait(1000));
        REQUIRE(pane->scrollbarMarks().size() == 1);

        // Synchronously, so the overlay bar's held marks are the new profile's before anything reads them.
        auto const before = spy.count();
        REQUIRE((*pane)(contour::actions::ChangeProfile { unmarked }));
        CHECK(spy.count() == before + 1);
        CHECK(pane->scrollbarMarks().isEmpty());
    }

    SECTION("a new track height asks for one")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
        REQUIRE(spy.wait(1000));

        pane->setScrollbarMarkTrackPixels(400);
        REQUIRE(spy.wait(1000));
        CHECK(pane->scrollbarMarks().size() == 1);
    }

    SECTION("attaching a display asks for one, for what ran while none was attached")
    {
        pane->detachDisplay(*pane.surface);
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));

        pane->attachDisplay(*pane.surface);
        REQUIRE(spy.wait(1000));
        CHECK(markKindsOf(*pane) == std::vector { vtbackend::ScrollbarMarkKind::Failure });
    }

    SECTION("a switch between the dark and the light colour scheme asks for one")
    {
        // A pane of its own, whose profile gives each scheme its own red: a failure's tick is drawn in it.
        auto const dualName = registerProfile(testApp.app(), "dual", [](contour::config::TerminalProfile& p) {
            auto scrollbar = p.scrollbar.value();
            scrollbar.position = contour::config::ScrollBarPosition::Right;
            scrollbar.marks = vtbackend::AllScrollbarMarkSources;
            p.scrollbar = scrollbar;

            auto dual = contour::config::DualColorConfig {};
            dual.darkMode.palette.at(1) = vtbackend::RGBColor { 0x80, 0x10, 0x10 };
            dual.lightMode.palette.at(1) = vtbackend::RGBColor { 0xF0, 0x40, 0x40 };
            p.colors.value() = dual;
        });
        auto dual = makeSessionWithSurface(testApp.app(), dualName);
        dual->setScrollbarMarkTrackPixels(200);
        auto dualSpy =
            QSignalSpy(dual.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
        REQUIRE(dualSpy.wait(1000));

        auto const other = testApp.app().colorPreference() == vtbackend::ColorPreference::Dark
                               ? vtbackend::ColorPreference::Light
                               : vtbackend::ColorPreference::Dark;
        dual->updateColorPreference(other);
        REQUIRE(dualSpy.wait(1000));
        CHECK(markColor(*dual, vtbackend::ScrollbarMarkKind::Failure)
              == (other == vtbackend::ColorPreference::Light ? QColor(0xF0, 0x40, 0x40)
                                                             : QColor(0x80, 0x10, 0x10)));
    }
}

TEST_CASE(
    "TerminalSession: scrollbar marks asked for by output on the parser's thread are rebuilt on the GUI "
    "thread",
    "[contour][session][scrollbar-marks][threading]")
{
    TestApp testApp;
    auto pane = makeSessionWithSurface(
        testApp.app(),
        registerMarkedScrollbarProfile(testApp.app(), "marked", vtbackend::AllScrollbarMarkSources));
    auto& terminal = pane->terminal();
    pane->setScrollbarMarkTrackPixels(200);

    // Whatever attaching the display asked for, done before anything is counted.
    auto settled = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);
    REQUIRE(settled.wait(1000));
    auto spy = QSignalSpy(pane.session.get(), &contour::session::TerminalSession::scrollbarMarksChanged);

    // A direct connection, so the slot runs on whichever thread emits: a queued one would carry an emission
    // from the wrong thread over to the right one and hide it. Its context dies with this scope, before the
    // locals the slot captures do, so no emission after it can reach them.
    auto const* gui = QThread::currentThread();
    auto offThread = std::atomic<int> { 0 };
    QObject const context;
    QObject::connect(
        pane.session.get(),
        &contour::session::TerminalSession::scrollbarMarksChanged,
        &context,
        [&]() {
            if (QThread::currentThread() != gui)
                ++offThread;
        },
        Qt::DirectConnection);

    auto writer = std::thread([&]() {
        for (auto const i: std::views::iota(0, 50))
            terminal.writeToScreen(vtbackend::test::commandRun(std::format("cmd {}", i), {}, 0));
    });
    // The GUI thread keeps turning meanwhile, as it does while a shell floods the terminal.
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        if (spy.count() > 0)
            break;
        QTest::qWait(5);
    }
    writer.join();
    for ([[maybe_unused]] auto const attempt: std::views::iota(0, 200))
    {
        if (pane->scrollbarMarks().size() == 50)
            break;
        QTest::qWait(5);
    }

    CHECK(spy.count() > 0);
    CHECK(offThread == 0);
    CHECK(pane->scrollbarMarks().size() == 50);
}

TEST_CASE("TerminalSession: a configuration reload hands folding.enabled to the running terminal",
          "[contour][session][folding]")
{
    // Every fold verb asks the engine's copy of the switch, the vi `z` keys among them.
    // A reload has to reach that copy too, or switching folding off would leave `zM` folding regardless.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const& settings = session->terminal().settings();
    REQUIRE(settings.foldingAvailability == vtbackend::FoldingAvailability::Enabled);

    auto const reloadWithFolding = [&](std::string_view enabled) {
        reloadSessionWith(*session, std::format("folding:\n    enabled: {}\n", enabled));
        REQUIRE(session->config().folding.value().enabled == (enabled == "true"));
    };

    reloadWithFolding("false");
    CHECK(settings.foldingAvailability == vtbackend::FoldingAvailability::Disabled);

    reloadWithFolding("true");
    CHECK(settings.foldingAvailability == vtbackend::FoldingAvailability::Enabled);
}

TEST_CASE("TerminalSession: a configuration reload hands the engine-read emulation settings over",
          "[contour][session][folding]")
{
    // Each of these is read where the engine uses it, so a reload that does not assign it leaves the
    // terminal on the value it was born with. Switching folding off left the previous command collapsing
    // the moment the next prompt started, while the `z` keys already refused to fold.
    contour::test::ConfiguredTestApp testApp;
    testApp.app().config().folding.value().autoCollapseOnNewCommand = true;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.settings().autoCollapseFoldOnNewCommand);

    SECTION("switching folding off stops collapsing the previous command")
    {
        reloadSessionWith(*session,
                          "folding:\n"
                          "    enabled: false\n"
                          "    auto_collapse_on_new_command: true\n");

        // The second prompt is where the first command would collapse.
        for (auto const* const command: { "ls", "pwd" })
            terminal.writeToScreen(vtbackend::test::commandRun(command, { "out" }));
        CHECK(terminal.hiddenLineCount() == vtbackend::LineCount(0));
    }

    SECTION("every one of them takes the reloaded value")
    {
        // Each differs from what the session was born with, so a field the reload skips fails here.
        reloadSessionWith(*session,
                          "folding:\n"
                          "    show_markers: false\n"
                          "    on_jump_into_fold: skip\n"
                          "osc_context:\n"
                          "    enabled: false\n"
                          "    tinting: off\n"
                          "images:\n"
                          "    good_image_protocol: true\n");

        auto const& settings = terminal.settings();
        CHECK_FALSE(settings.autoCollapseFoldOnNewCommand);
        CHECK(settings.foldJumpBehavior == vtbackend::FoldJumpBehavior::Skip);
        CHECK(settings.gutter
              == contour::config::gutterSettingsFor(session->config(), session->gutterChrome().form));
        CHECK_FALSE(settings.gutter.foldMarkers);
        CHECK(settings.gutter.exitStatus); // the block column stays, for the exit status
        CHECK(settings.contextSignalling == vtbackend::ContextSignalling::Disabled);
        CHECK(settings.contextTintScope == vtbackend::ContextTintScope::Off);
        CHECK(settings.goodImageProtocol);
    }
}

TEST_CASE("TerminalSession: a configuration reload hands the preedit style to the running terminal",
          "[contour][session][ime]")
{
    // The render buffer reads the terminal's own copy of the style, so a reload has to reach that copy.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const& settings = session->terminal().settings();
    auto const born = settings.preeditStyle;
    auto const other = born == vtbackend::PreeditStyle::Inline ? std::string_view { "highlighted" }
                                                               : std::string_view { "inline" };

    reloadSessionWith(*session, {}, std::format("        input_method_editor_style: {}\n", other));
    CHECK(settings.preeditStyle != born);
}

TEST_CASE("TerminalSession: a configuration reload hands the context limits and mark policy over",
          "[contour][session][config]")
{
    // Both are copied out of the settings when the terminal is built, so assigning the settings on a
    // reload would change nothing: the terminal has to be handed them.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.contexts().limits().maxDepth > 2);
    REQUIRE(terminal.markArbiter().contextMayMark());

    reloadSessionWith(*session,
                      "osc_context:\n"
                      "    max_depth: 2\n"
                      "    max_retained_contexts: 8\n"
                      "    derive_markers: never\n");

    CHECK(terminal.contexts().limits() == vtbackend::ContextStackLimits { .maxDepth = 2, .maxRetained = 8 });
    CHECK_FALSE(terminal.markArbiter().contextMayMark());

    // The new depth holds from the next push on.
    for (auto const* const id: { "one", "two", "three" })
        terminal.writeToScreen(std::format("\033]3008;start={};type=shell\033\\", id));
    CHECK(terminal.contexts().depth() == 2);
    CHECK(terminal.contexts().droppedPushes() == 1);
}

TEST_CASE("TerminalSession: a configuration reload hands the presentation settings over",
          "[contour][session][config]")
{
    // Read where the engine uses them, as the emulation settings are, so a reload that does not assign one
    // leaves the terminal on the value the session was born with.
    using namespace std::chrono_literals;
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const& settings = session->terminal().settings();
    REQUIRE_FALSE(settings.autoScrollOnUpdate);
    REQUIRE_FALSE(settings.isInsertAfterYank);
    REQUIRE(settings.blinkStyle == vtbackend::BlinkStyle::Smooth);
    REQUIRE(settings.screenTransitionStyle == vtbackend::ScreenTransitionStyle::Fade);
    REQUIRE(settings.smoothScrolling);
    REQUIRE(settings.momentumScrolling);

    // Each differs from what the session was born with, so a field the reload skips fails here.
    reloadSessionWith(*session,
                      "",
                      "        history:\n"
                      "            auto_scroll_on_update: true\n"
                      "        insert_after_yank: true\n"
                      "        blink_style: linger\n"
                      "        screen_transition: classic\n"
                      "        screen_transition_duration: 333\n"
                      "        cursor_motion_animation_duration: 77\n"
                      "        slow_scrolling_time: 250\n"
                      "        smooth_scrolling: false\n"
                      "        momentum_scrolling: false\n");

    CHECK(settings.autoScrollOnUpdate);
    CHECK(settings.isInsertAfterYank);
    CHECK(settings.blinkStyle == vtbackend::BlinkStyle::Linger);
    CHECK(settings.screenTransitionStyle == vtbackend::ScreenTransitionStyle::Classic);
    CHECK(settings.screenTransitionDuration == 333ms);
    CHECK(settings.cursorMotionAnimationDuration == 77ms);
    CHECK(settings.smoothLineScrolling == 250ms);
    CHECK_FALSE(settings.smoothScrolling);
    CHECK_FALSE(settings.momentumScrolling);
}

TEST_CASE("TerminalSession: a configuration reload hands the status line's layout over",
          "[contour][session][config]")
{
    // Read where the engine uses them, like the emulation settings, so a reload that does not assign
    // them leaves the status line where, and as, the session was born with it.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.settings().statusDisplayPosition == vtbackend::StatusDisplayPosition::Bottom);
    REQUIRE(terminal.settings().tabNamingMode == vtbackend::TabsNamingMode::Indexing);

    reloadSessionWith(*session,
                      "",
                      "        status_line:\n"
                      "            display: indicator\n"
                      "            position: top\n"
                      "            indicator:\n"
                      "                left: \"{Text:text=ReloadedLeft}\"\n"
                      "                middle: \"{Tabs:Indexing=Title}\"\n"
                      "                right: \"\"\n");

    auto const& settings = terminal.settings();
    CHECK(settings.statusDisplayPosition == vtbackend::StatusDisplayPosition::Top);
    CHECK(settings.tabNamingMode == vtbackend::TabsNamingMode::Title);
    CHECK(settings.indicatorStatusLine.left == "{Text:text=ReloadedLeft}");
    CHECK(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("ReloadedLeft"));

    // Disabled key bindings put up a line of their own, and a reload must not take it down while they
    // are still disabled.
    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    REQUIRE(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("key bindings disabled"));
    REQUIRE((*session)(contour::actions::ReloadConfig {}));
    CHECK(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("key bindings disabled"));
}

TEST_CASE(
    "TerminalSession: a reload that changes the status line type selects it and leaves a host-writable one",
    "[contour][session][config]")
{
    // DEC STD 070 ch. 14 §3 DECSSDT: "The type of Status Line can also be changed through a Set-Up field",
    // which the configuration is; §2.3: "If the Status Display Type is chosen to be something other than
    // "Host Writable", the Status Display will be exited". A reload that changes the configured type is that
    // change: here from none to the indicator.
    contour::test::ConfiguredTestApp testApp;
    setStatusLineDisplay(testApp.app(), vtbackend::StatusDisplayType::None);
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    writeToAllowedStatusLine(*session);

    reloadSessionWith(*session,
                      "",
                      "        status_line:\n"
                      "            display: indicator\n");
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);

    terminal.writeToScreen("\033[1$}X"); // DECSASD again, and text
    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
    CHECK(terminal.primaryScreen().renderMainPageText().contains('X'));
    CHECK_FALSE(terminal.hostWritableStatusLineDisplay().renderMainPageText().contains('X'));
}

TEST_CASE("TerminalSession: a reload that keeps the status line type leaves the host's selection alone",
          "[contour][session][config]")
{
    // A reload that leaves `status_line.display` as it was is no Set-Up change of the type (DEC STD 070 ch.
    // 14 §3): what the host selected since stays, as in mintty's term_reconfig(), which acts only where the
    // configured value changed.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    writeToAllowedStatusLine(*session);

    reloadSessionWith(*session,
                      "",
                      "        status_line:\n"
                      "            display: indicator\n");
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
    CHECK(terminal.activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);

    terminal.writeToScreen("X");
    CHECK(terminal.hostWritableStatusLineDisplay().renderMainPageText().contains('X'));
    CHECK_FALSE(terminal.primaryScreen().renderMainPageText().contains('X'));
}

TEST_CASE("TerminalSession: a status line type a reload changes during vi mode outlasts it",
          "[contour][session][config]")
{
    // Vi mode's indicator is a notice put up for a while (pushStatusDisplay), not the user's type. A reload
    // that changes the configured type meanwhile leaves the notice up, and is what leaving vi mode shows.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

    (*session)(contour::actions::ViNormalMode {});
    REQUIRE(terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
    reloadSessionWith(*session,
                      "",
                      "        status_line:\n"
                      "            display: none\n");
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

    (*session)(contour::actions::ViNormalMode {});
    REQUIRE(terminal.inputHandler().mode() == vtbackend::ViMode::Insert);
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
}

TEST_CASE("TerminalSession: re-enabling key bindings brings back the status line type shown before",
          "[contour][session][config]")
{
    // The "key bindings disabled" indicator is a notice put up for a while, as vi mode's is (and as DEC STD
    // 070's Error Line, ch. 14 §2.1.1, "replaces the currently displayed Status Line" until a key restores
    // it): taking it down shows again what it replaced.
    auto const shownBefore =
        GENERATE(vtbackend::StatusDisplayType::None, vtbackend::StatusDisplayType::Indicator);
    INFO("shown before: " << static_cast<int>(shownBefore));
    contour::test::ConfiguredTestApp testApp;
    setStatusLineDisplay(testApp.app(), shownBefore);
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.statusDisplayType() == shownBefore);

    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    CHECK(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("key bindings disabled"));

    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    CHECK(terminal.statusDisplayType() == shownBefore);
}

TEST_CASE("TerminalSession: the key bindings notice outlasts a host's keyboard unlock",
          "[contour][session][config]")
{
    // KAM reset -- DECSTR, or CSI 2 l -- takes down the notice of a locked keyboard, which it never put up
    // here: the key bindings stay disabled, and so does their notice. "The host may not directly program or
    // affect the Error Line" (DEC STD 070 ch. 14 §2.1.1), the notice DEC has.
    auto const unlock = GENERATE(std::string_view { "\033[!p" }, std::string_view { "\033[2l" });
    INFO("unlock: " << core::escape(unlock));
    contour::test::ConfiguredTestApp testApp;
    setStatusLineDisplay(testApp.app(), vtbackend::StatusDisplayType::None);
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();

    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    REQUIRE(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    terminal.writeToScreen(unlock);

    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);
    CHECK(terminal.indicatorStatusLineDisplay().renderMainPageText().contains("key bindings disabled"));

    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
}

TEST_CASE("TerminalSession: vi mode's notice outlasts the key bindings notice", "[contour][session][config]")
{
    // Two notices up at once: each comes down alone, and the type shown before the first comes back with the
    // last.
    contour::test::ConfiguredTestApp testApp;
    setStatusLineDisplay(testApp.app(), vtbackend::StatusDisplayType::None);
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();

    (*session)(contour::actions::ViNormalMode {});
    REQUIRE(terminal.inputHandler().mode() == vtbackend::ViMode::Normal);
    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::Indicator);

    (*session)(contour::actions::ViNormalMode {});
    REQUIRE(terminal.inputHandler().mode() == vtbackend::ViMode::Insert);
    CHECK(terminal.statusDisplayType() == vtbackend::StatusDisplayType::None);
}

TEST_CASE("TerminalSession: a denied status line's text becomes the window title when configured",
          "[contour][session][config]")
{
    // status_line.sync_to_window_title: with the host-writable status line denied, the text an
    // application writes there goes to the window title instead.
    contour::test::ConfiguredTestApp testApp;

    SECTION("not configured: the title stays")
    {
        setSyncToWindowTitle(testApp.app(), false);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().setWindowTitle("before");
        writeToDeniedStatusLine(*session, "status text");
        CHECK(session->terminal().windowTitle() == "before");
    }

    SECTION("configured at construction")
    {
        setSyncToWindowTitle(testApp.app(), true);
        auto session = makeDisplaylessSession(testApp.app());
        writeToDeniedStatusLine(*session, "status text");
        CHECK(session->terminal().windowTitle() == "status text");
    }

    SECTION("configured by a reload")
    {
        setSyncToWindowTitle(testApp.app(), false);
        auto session = makeDisplaylessSession(testApp.app());
        reloadSessionWith(*session,
                          "",
                          "        status_line:\n"
                          "            sync_to_window_title: true\n");
        writeToDeniedStatusLine(*session, "status text");
        CHECK(session->terminal().windowTitle() == "status text");
    }

    SECTION("an allowed status line keeps it to itself")
    {
        setSyncToWindowTitle(testApp.app(), true);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().setWindowTitle("before");
        session->executeShowHostWritableStatusLine(PermissionDecision::Allow, PermissionMemory::Once);
        session->terminal().writeToScreen("\033[1$}status text\033[0$}");
        CHECK(session->terminal().windowTitle() == "before");
    }

    // The setting is read where the text would go to the title, not when the user denied the line: a
    // reload in between is the setting in force when the application returns to the main display.
    SECTION("turned off by a reload after the denial")
    {
        setSyncToWindowTitle(testApp.app(), true);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().setWindowTitle("before");
        session->terminal().writeToScreen("\033[2$~");
        session->executeShowHostWritableStatusLine(PermissionDecision::Deny, PermissionMemory::Once);
        reloadSessionWith(*session,
                          "",
                          "        status_line:\n"
                          "            sync_to_window_title: false\n");
        session->terminal().writeToScreen("\033[1$}status text\033[0$}");
        CHECK(session->terminal().windowTitle() == "before");
    }

    SECTION("turned on by a reload after the denial")
    {
        setSyncToWindowTitle(testApp.app(), false);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().writeToScreen("\033[2$~");
        session->executeShowHostWritableStatusLine(PermissionDecision::Deny, PermissionMemory::Once);
        reloadSessionWith(*session,
                          "",
                          "        status_line:\n"
                          "            sync_to_window_title: true\n");
        session->terminal().writeToScreen("\033[1$}status text\033[0$}");
        CHECK(session->terminal().windowTitle() == "status text");
    }

    // Every way out of the status line hands the text over as DECSASD 0 does: DECSSDT selecting another type
    // exits it (DEC STD 070 ch. 14 §2.3), and so does DECSTR ("The Status Display will be exited, but not
    // erased").
    SECTION("left by a control other than DECSASD 0")
    {
        using Exit = std::pair<std::string_view, std::string_view>;
        auto const [name, exit] = GENERATE(
            Exit { "DECSSDT 0", "\033[0$~" }, Exit { "DECSSDT 1", "\033[1$~" }, Exit { "DECSTR", "\033[!p" });
        INFO("exit: " << name);
        setSyncToWindowTitle(testApp.app(), true);
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().writeToScreen("\033[2$~");
        session->executeShowHostWritableStatusLine(PermissionDecision::Deny, PermissionMemory::Once);
        session->terminal().writeToScreen("\033[1$}status text");
        session->terminal().writeToScreen(exit);
        CHECK(session->terminal().activeStatusDisplay() == vtbackend::ActiveStatusDisplay::Main);
        CHECK(session->terminal().windowTitle() == "status text");
    }
}

TEST_CASE("TerminalSession: a hard reset returns sixel scrolling to images.sixel_scrolling",
          "[contour][session][config]")
{
    // A configured setting, as the VT340's Graphics Set-Up feature of that name is, so RIS restores it
    // rather than DECSDM's own reset state -- as xterm restores its sixelScrolling resource.
    contour::test::ConfiguredTestApp testApp;
    auto const hardResetAfterTurningItOn = [](vtbackend::Terminal& terminal) {
        terminal.writeToScreen("\033[?80l\033c"); // DECRST 80 (scrolling on), then RIS
    };

    SECTION("configured at construction")
    {
        testApp.app().config().images.value().sixelScrolling = false;
        auto session = makeDisplaylessSession(testApp.app());
        auto& terminal = session->terminal();
        REQUIRE(terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling));

        hardResetAfterTurningItOn(terminal);
        CHECK(terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling));
    }

    SECTION("configured by a reload")
    {
        auto session = makeDisplaylessSession(testApp.app());
        auto& terminal = session->terminal();
        REQUIRE_FALSE(terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling));

        reloadSessionWith(*session, "images:\n    sixel_scrolling: false\n");
        REQUIRE(terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling));

        hardResetAfterTurningItOn(terminal);
        CHECK(terminal.isModeEnabled(vtbackend::DECMode::NoSixelScrolling));
    }
}

TEST_CASE("TerminalSession: highlight_word_and_matches_on_double_click reaches the terminal",
          "[contour][session][config]")
{
    contour::test::ConfiguredTestApp testApp;

    SECTION("at construction")
    {
        for (auto& [name, profile]: testApp.app().config().profiles.value())
            profile.highlightDoubleClickedWord.value() = false;
        auto session = makeDisplaylessSession(testApp.app());
        CHECK_FALSE(session->terminal().visualizeSelectedWord());
    }

    SECTION("on a reload")
    {
        auto session = makeDisplaylessSession(testApp.app());
        REQUIRE(session->terminal().visualizeSelectedWord());
        reloadSessionWith(*session, "", "        highlight_word_and_matches_on_double_click: false\n");
        CHECK_FALSE(session->terminal().visualizeSelectedWord());
    }
}

TEST_CASE("TerminalSession: a configuration reload hands the profile's colors over",
          "[contour][session][config]")
{
    // The palette used to be re-seeded only when the light/dark preference changed, so a reload that
    // changed the colors and nothing else kept the old ones until the next session.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto const reloaded = vtbackend::RGBColor { 0x12, 0x34, 0x56 };
    REQUIRE(terminal.colorPalette().defaultBackground != reloaded);

    reloadSessionWith(*session,
                      "color_schemes:\n"
                      "    reloaded:\n"
                      "        default:\n"
                      "            background: '#123456'\n",
                      "        colors: reloaded\n");
    auto const* const configured = contour::config::preferredColorPalette(
        session->config().profile(session->profileName())->colors.value(), testApp.app().colorPreference());
    REQUIRE(configured != nullptr);
    REQUIRE(configured->defaultBackground == reloaded);

    CHECK(terminal.colorPalette().defaultBackground == reloaded);
    CHECK(terminal.defaultColorPalette().defaultBackground == reloaded);
}

TEST_CASE("TerminalSession: a reset restores what a configuration reload configured",
          "[contour][session][config]")
{
    // RIS returns the terminal to its power-up state, and a reload has changed what that is: the
    // terminal is now configured as something else, and a reset must not bring back what it was born as.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE(terminal.terminalId() == vtbackend::VTType::VT525);

    SECTION("RIS keeps the reloaded terminal ID, in the operating level and in DA1")
    {
        reloadSessionWith(*session, "", "        terminal_id: VT220\n");
        REQUIRE(terminal.terminalId() == vtbackend::VTType::VT220);

        terminal.writeToScreen("\033c");
        CHECK(terminal.operatingLevel() == vtbackend::VTType::VT220);

        mockPtyOf(*session).stdinBuffer().clear();
        terminal.writeToScreen("\033[c");
        terminal.flushInput();
        CHECK(mockPtyOf(*session).stdinBuffer().starts_with("\033[?62;"));
    }

    SECTION("RIS restores the reloaded status line")
    {
        using vtbackend::StatusDisplayType;
        auto const wanted = terminal.statusDisplayType() == StatusDisplayType::Indicator
                                ? StatusDisplayType::None
                                : StatusDisplayType::Indicator;
        reloadSessionWith(*session,
                          "",
                          std::format("        status_line:\n"
                                      "            display: {}\n",
                                      wanted == StatusDisplayType::None ? "none" : "indicator"));
        REQUIRE(terminal.statusDisplayType() == wanted);

        terminal.writeToScreen("\033c");
        CHECK(terminal.statusDisplayType() == wanted);
    }

    SECTION("DECSCUSR 0 restores the reloaded cursor")
    {
        using vtbackend::CursorShape;
        auto const wanted =
            terminal.cursorShape() == CursorShape::Block ? CursorShape::Underscore : CursorShape::Block;
        reloadSessionWith(*session,
                          "",
                          std::format("        cursor:\n"
                                      "            shape: {}\n",
                                      wanted == CursorShape::Block ? "block" : "underscore"));
        REQUIRE(terminal.cursorShape() == wanted);

        // DECSCUSR 6 (steady bar) first, so the reset has something to undo.
        terminal.writeToScreen("\033[6 q");
        REQUIRE(terminal.cursorShape() == CursorShape::Bar);
        terminal.writeToScreen("\033[0 q");
        CHECK(terminal.cursorShape() == wanted);
    }
}

TEST_CASE("TerminalSession: a saved contour.yml reaches every open session and the next new tab",
          "[contour][session][config]")
{
    // live_config: one watcher for the app reads a save once, updates the configuration the next tab is
    // built from, and reloads every open session under its own profile.
    auto app = TwoProfileApp { LiveConfig::On };
    auto& manager = app.testApp.manager();
    auto* first = manager.createSession(app.window.id);
    auto* work = manager.createSession(app.window.id, "work");
    REQUIRE(first != nullptr);
    REQUIRE(work != nullptr);

    // One save that reaches the disk in two writes, as an editor's often does: the watcher reports each,
    // and the app reads the file once.
    auto const capture = core::log::ScopedCapture { "config" };
    writeTwoProfileConfig(app.testApp.configFile(), app.mainProfile, "VT100", "VT100");
    QCoreApplication::processEvents();
    std::this_thread::sleep_for(std::chrono::milliseconds { 20 });
    QCoreApplication::processEvents();
    writeTwoProfileConfig(app.testApp.configFile(), app.mainProfile, "VT220", "VT320");

    REQUIRE(pumpUntil([&] { return first->terminal().terminalId() == vtbackend::VTType::VT220; }));
    CHECK(work->terminal().terminalId() == vtbackend::VTType::VT320);
    CHECK(work->profileName() == "work");

    // Let every notification of that save arrive, then count the reads it caused. The count is checked
    // where the watcher reports a write as it lands (inotify, ReadDirectoryChangesW), so that both writes
    // fall inside one coalescing window. macOS's FSEvents reports changes with a latency of its own, and
    // on a loaded machine far enough apart that the window does not always take in both: CI saw this
    // save read twice there.
    std::ignore = pumpUntil([] { return false; });
#ifndef __APPLE__
    CHECK(capture.count("Loading configuration from file") == 1);
#endif

    auto* next = manager.createSession(app.window.id);
    REQUIRE(next != nullptr);
    CHECK(next->terminal().terminalId() == vtbackend::VTType::VT220);
    CHECK(primaryDeviceAttributesOf(*next).starts_with("\033[?62;"));
}

TEST_CASE("TerminalSession: ReloadConfig reaches every open session and the next new tab",
          "[contour][session][config]")
{
    auto app = TwoProfileApp { LiveConfig::Off };
    auto& manager = app.testApp.manager();
    auto* first = manager.createSession(app.window.id);
    auto* work = manager.createSession(app.window.id, "work");
    REQUIRE(first != nullptr);
    REQUIRE(work != nullptr);

    writeTwoProfileConfig(app.testApp.configFile(), app.mainProfile, "VT220", "VT320");
    REQUIRE((*first)(contour::actions::ReloadConfig {}));

    CHECK(first->terminal().terminalId() == vtbackend::VTType::VT220);
    CHECK(work->terminal().terminalId() == vtbackend::VTType::VT320);
    CHECK(work->profileName() == "work");

    auto* next = manager.createSession(app.window.id);
    REQUIRE(next != nullptr);
    CHECK(next->terminal().terminalId() == vtbackend::VTType::VT220);
    CHECK(primaryDeviceAttributesOf(*next).starts_with("\033[?62;"));
}

TEST_CASE("TerminalSession: a reload after ResetConfig reads the configuration file it reset",
          "[contour][session][config]")
{
    // ResetConfig rewrites the user's file with the defaults. The session has to go on reading THAT file:
    // a later edit and reload must be seen, not ignored until the next start.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    REQUIRE((*session)(contour::actions::ResetConfig {}));
    CHECK(session->config().configFile == testApp.configFile());
    CHECK(session->config().folding.value().enabled);

    reloadSessionWith(*session, "folding:\n    enabled: false\n");
    CHECK_FALSE(session->config().folding.value().enabled);
    CHECK(session->terminal().settings().foldingAvailability == vtbackend::FoldingAvailability::Disabled);
}

TEST_CASE("TerminalSession: display-guarded toggle actions are safe no-ops without a display",
          "[contour][session][actions]")
{
    // These actions forward to the display when present and must be harmless (consumed, no crash)
    // when there is none — the same guard family covered elsewhere for input.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    CHECK((*session)(actions::ToggleFullscreen {}));
    CHECK((*session)(actions::ToggleInputMethodHandling {}));
    CHECK((*session)(actions::ToggleTitleBar {}));

    // Vi-mode toggle round-trips Insert -> Normal -> Insert. Note this is NOT merely input-handler
    // state: entering Normal mode pushes the indicator status line, which resizes the page and
    // reflows the grid -- see the lock-contract case below.
    using vtbackend::ViMode;
    CHECK(session->terminal().inputHandler().mode() == ViMode::Insert);
    (*session)(actions::ViNormalMode {});
    CHECK(session->terminal().inputHandler().mode() == ViMode::Normal);
    (*session)(actions::ViNormalMode {});
    CHECK(session->terminal().inputHandler().mode() == ViMode::Insert);
}

TEST_CASE("TerminalSession: ViNormalMode holds the terminal lock while switching mode",
          "[contour][session][actions][threading]")
{
    // Regression guard for #1495 ("Normal mode crashes ... WSL -> ssh -> tmux").
    //
    // The action runs on the GUI thread, and entering Normal mode is not a local state flip:
    // ViCommands::modeChanged() pushes the indicator status display, which resizes the page and
    // reflows the grid (Terminal::setStatusDisplay -> resizeScreen -> Screen::applyPageSizeToMainDisplay
    // -> Grid::resize). The parser thread writes cells into that very grid while holding the terminal
    // lock, so doing this unlocked reallocates the grid underneath a live write -- a torn grid or a
    // dangling line iterator, surfacing as a segmentation fault or a verifyState() abort (Require() is NOT
    // compiled out in release builds).
    //
    // Rather than race the real threads, assert the contract directly: hold the lock exactly as the
    // parser thread does, and require the action to block until it is released.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    auto parserHold = std::unique_lock { session->terminal() };

    auto started = std::atomic<bool> { false };
    auto completed = std::atomic<bool> { false };
    auto worker = std::thread { [&]() {
        started.store(true, std::memory_order_release);
        (*session)(actions::ViNormalMode {});
        completed.store(true, std::memory_order_release);
    } };

    // Wait until the worker is actually running, so the check below cannot pass merely because the
    // thread had not been scheduled yet.
    while (!started.load(std::memory_order_acquire))
        std::this_thread::yield();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    CHECK_FALSE(completed.load(std::memory_order_acquire));

    parserHold.unlock();
    worker.join();

    CHECK(completed.load(std::memory_order_acquire));
    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Normal);
}
