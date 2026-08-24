// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for TerminalSession that exercise the *real* class (not a Qt-free surrogate).
//
// Unlike the model-layer tests (TabListModel_test / vtworkspace SessionModel_test), these construct an
// actual TerminalSession around a MockPty and a test-configured ContourGuiApp. That is only possible
// because the contour frontend is built as the `contour_core` object library the test links against,
// and because crispy::App exposes parseParametersForTesting() to populate parameters() without
// launching the GUI event loop.
//
// The headline case is the regression behind the "close leaks background tabs" finding:
// TerminalSession::terminate() must close the PTY device even when NO display is attached (a
// background tab/split pane whose display was detached on the last tab switch). Before the fix
// terminate() early-returned for a display-less session, so the device stayed open and the session —
// plus its shell process — leaked.

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <vtbackend/core/Hyperlink.hpp>

#include <vtpty/MockPty.hpp>

#include <crispy/Utils.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QHostInfo>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

using namespace std::string_literals;

namespace
{

using contour::test::TestApp;

constexpr auto TestPageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

/// Creates a TerminalSession backed by @p pty, with NO display attached (the default state).
/// The session owns the PTY; the caller owns the session.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeSessionWith(
    contour::ContourGuiApp& app,
    std::unique_ptr<vtpty::Pty> pty,
    std::unique_ptr<contour::platform::Notifier> notifier = nullptr)
{
    // Pass the app's real session manager so the sessionClosed->removeSession wiring matches
    // production; we never pump the Qt event loop here, so that slot does not actually fire and the
    // test stays deterministic.
    return std::make_unique<contour::session::TerminalSession>(&app.sessionsManager(),
                                                               std::move(pty),
                                                               app,
                                                               std::string {},
                                                               std::nullopt,
                                                               std::nullopt,
                                                               std::move(notifier));
}

/// The common case: a session over a plain MockPty.
/// @param notifier the desktop notifier to inject; null (the default) takes the platform's own.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeDisplaylessSession(
    contour::ContourGuiApp& app, std::unique_ptr<contour::platform::Notifier> notifier = nullptr)
{
    return makeSessionWith(app, std::make_unique<vtpty::MockPty>(TestPageSize), std::move(notifier));
}

/// A Notifier that raises nothing and records everything, and can play the desktop's own close and
/// activation events back at the session. Deliberately not a mock framework: the recorded vectors
/// ARE the assertions.
///
/// Without this the notification path could only be asserted not to throw: the real backend reaches
/// a D-Bus session that a headless run does not have.
class RecordingNotifier final: public contour::platform::Notifier
{
  public:
    void notify(vtbackend::DesktopNotification const& notification) override
    {
        raised.push_back(notification);
    }

    void close(std::string const& identifier) override { closed.push_back(identifier); }

    /// Plays back what the desktop would report once the user (or a timeout) retired a notification.
    /// @param report Whether the backend observed the close or a timer merely assumed it; the
    ///               portal backend can only ever produce the latter.
    void fireClosed(std::string const& identifier,
                    uint reason,
                    vtbackend::CloseReport report = vtbackend::CloseReport::Observed)
    {
        emit notificationClosed(QString::fromStdString(identifier), reason, report);
    }

    /// Plays back what the desktop would report when the user clicked a notification.
    void fireActivated(std::string const& identifier)
    {
        emit actionInvoked(QString::fromStdString(identifier));
    }

    std::vector<vtbackend::DesktopNotification> raised;
    std::vector<std::string> closed;
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

    // Reported where the user is looking, carrying the reason rather than a bare "it failed".
    auto const screen = session->terminal().primaryScreen().renderMainPageText();
    CHECK(screen.contains(contour::test::ConfigurableStartPty::FailureText));

    // The device never came up, so it is left closed — which is what makes the pane dismissible
    // through the paths that already exist (terminate()'s already-closed branch, and the
    // acknowledging key press below).
    CHECK(session->terminal().device().isClosed());

    // The pane is not a zombie: the next key press prunes it, exactly as the early-exit notice does
    // for a shell that dies right after starting.
    auto closed = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&](contour::session::TerminalSession&) { ++closed; });
    session->sendKeyEvent(vtbackend::Key::Enter,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
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

    CHECK_NOTHROW(session->start());

    // The platform's own reason reaches the user — a bare "could not start" would leave them with no
    // idea that it was the directory, which is exactly what they need to fix.
    auto const screen = session->terminal().primaryScreen().renderMainPageText();
    CHECK(screen.contains(contour::test::ConfigurableStartPty::FailureText));
    CHECK(session->terminal().device().isClosed());
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

// ============================================================================================
// Headless input, action, and lifecycle coverage (MockPty-backed, no display, no event loop).
// ============================================================================================

namespace
{

using vtbackend::KeyboardEventType;
using vtbackend::Modifiers;

using contour::test::mockPtyOf;

/// Registers a copy of the "main" profile under @p name in @p app's config, letting @p mutate it, so
/// a session constructed under that name exercises config-driven behaviour (hint patterns, bell
/// sound, mode cursors, ...) without touching the default profile. Returns @p name for chaining.
template <typename Mutator>
std::string registerProfile(contour::ContourGuiApp& app, std::string const& name, Mutator&& mutate)
{
    auto& config = app.config();
    auto profile = *config.profile(app.profileName()); // copy the default as the baseline
    std::forward<Mutator>(mutate)(profile);
    config.profiles.value().insert_or_assign(name, std::move(profile));
    return name;
}

/// Creates a display-less MockPty-backed session running under the named @p profileName (which must
/// already be registered in @p app's config, e.g. via registerProfile()).
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeSessionWithProfile(
    contour::ContourGuiApp& app, std::string profileName)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    return std::make_unique<contour::session::TerminalSession>(
        &app.sessionsManager(), std::move(pty), app, std::move(profileName));
}

/// A session with a recording surface attached, so the two die together and in the right order.
struct SessionWithSurface
{
    std::unique_ptr<contour::session::TerminalSession> session;
    std::unique_ptr<contour::test::FakeDisplaySurface> surface;

    contour::session::TerminalSession* operator->() const noexcept { return session.get(); }
    contour::session::TerminalSession& operator*() const noexcept { return *session; }

    ~SessionWithSurface()
    {
        // The session outlives nothing here, but it holds a raw back-pointer to the surface: detach
        // first so a destructor-time post cannot reach freed memory.
        if (session && surface && session->display() == surface.get())
            session->detachDisplay(*surface);
    }

    SessionWithSurface(SessionWithSurface const&) = delete;
    SessionWithSurface& operator=(SessionWithSurface const&) = delete;
    SessionWithSurface(SessionWithSurface&&) = default;
    SessionWithSurface& operator=(SessionWithSurface&&) = delete;

    SessionWithSurface(std::unique_ptr<contour::session::TerminalSession> s,
                       std::unique_ptr<contour::test::FakeDisplaySurface> d):
        session { std::move(s) }, surface { std::move(d) }
    {
    }
};

/// Builds a MockPty-backed session with a recording surface already attached.
/// @param profileName A profile registered via registerProfile(); absent takes the app's default one.
[[nodiscard]] SessionWithSurface makeSessionWithSurface(contour::ContourGuiApp& app,
                                                        std::optional<std::string> profileName = std::nullopt)
{
    auto session =
        profileName ? makeSessionWithProfile(app, *std::move(profileName)) : makeDisplaylessSession(app);
    auto surface = std::make_unique<contour::test::FakeDisplaySurface>();
    surface->attachedSession = session.get();
    session->attachDisplay(*surface);
    return { std::move(session), std::move(surface) };
}

} // namespace

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

TEST_CASE("TerminalSession: mouse events without a mouse protocol write nothing and do not crash",
          "[contour][session][input]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();
    auto const pos = vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(1) };
    auto const pixels = vtbackend::PixelCoordinate {};
    (void) now;

    session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Left, pixels);
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
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const pixels = vtbackend::PixelCoordinate {};

    auto menuRequests = 0;
    QObject::connect(&testApp.app().sessionsManager(),
                     &contour::session::TerminalSessionManager::contextMenuRequested,
                     [&](contour::session::TerminalSession*) { ++menuRequests; });

    SECTION("no mouse protocol: the application hears nothing, and the menu takes the click")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pixels);
        CHECK(menuRequests == 1);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("mouse protocol on: the application gets its right-click and the menu stays shut")
    {
        // DECSET 1000 -- what vim and tmux turn on.
        session->terminal().writeToScreen("\033[?1000h");
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pixels);
        CHECK(menuRequests == 0);
        CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("mouse protocol on, plus Shift: the bypass modifier hands the click back to the menu")
    {
        session->terminal().writeToScreen("\033[?1000h");
        session->sendMousePressEvent(
            Modifiers { vtbackend::Modifier::Shift }, vtbackend::MouseButton::Right, pixels);
        // Shift is the bypass modifier, so the application is skipped and the bare `Right` fallback
        // matches (sendMousePressEvent strips the bypass modifier before looking the mapping up).
        CHECK(menuRequests == 1);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }

    SECTION("a left-drag in flight keeps the menu shut: the popup would steal the button-release")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Left, pixels);
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Right, pixels);
        CHECK(menuRequests == 0);
    }

    SECTION("a middle-click still pastes: the fallback claims the right button and nothing else")
    {
        session->sendMousePressEvent(Modifiers {}, vtbackend::MouseButton::Middle, pixels);
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
        session->terminal().writeToScreen("\033]133;A\033\\$ ls\r\n");
        session->terminal().writeToScreen("\033]133;C\033\\file1\r\nfile2\r\n");
        session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

        CHECK(session->contextMenuState().hasLastCommand);

        auto const block = session->terminal().lastCommandBlock();
        REQUIRE(block.has_value());
        CHECK(block->prompt == "$ ls");
        CHECK(block->output == "file1\nfile2");
    }

    SECTION("SoftReset keeps the scrollback that a hard reset would wipe")
    {
        for (int i = 0; i < 40; ++i)
            session->terminal().writeToScreen(std::format("line {}\r\n", i));
        REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));

        // DECSTR resets the status display (setStatusDisplay(None)), which gives the main page back the
        // row the profile's status line was using and so pulls exactly one line out of the history. That
        // is the sequence doing its job -- what matters is that the scrollback SURVIVES.
        CHECK((*session)(contour::actions::SoftReset {}));
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
        session->terminal().writeToScreen("\033]133;A\033\\$ cd /tmp\r\n");
        session->terminal().writeToScreen("\033]133;C\033\\");
        session->terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");

        auto const block = session->terminal().lastCommandBlock();
        REQUIRE(block.has_value());
        REQUIRE(block->output.empty()); // precondition: there IS a block, and it printed nothing

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
    session->terminal().setCurrentWorkingDirectory("file://" + QHostInfo::localHostName().toStdString()
                                                   + "/tmp");

    // The document/URL openers route through the injected ExternalLauncher (no desktop touched).
    CHECK((*session)(actions::OpenConfiguration { .inEditor = true }));
    CHECK((*session)(actions::OpenFileManager {}));
    CHECK((*session)(actions::OpenSelection {}));
    // OpenConfiguration{in_editor} opens the config file URL, OpenFileManager the local cwd, and
    // OpenSelection the (empty) selection.
    CHECK(launcher.openedUrls.size() == 3);

    // OpenConfiguration WITHOUT in_editor opens the in-app settings page instead — no URL is launched
    // (headless, no hosting window, so it is a safe no-op that still returns true).
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
    CHECK_NOTHROW(session->applyPendingFontChange(true, false));
    CHECK_NOTHROW(session->applyPendingPaste(false, false));
    CHECK_NOTHROW(session->executePendingBufferCapture(false, false));
}

TEST_CASE("TerminalSession: pasteFromClipboard writes clipboard text and enforces the size guards",
          "[contour][session][clipboard]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);

    clipboard->setText(QStringLiteral("pasted"));
    session->pasteFromClipboard(1, /*strip*/ false);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("pasted"));

    // > 1 MB: hard-rejected — and (regression) must not crash on a display-less session.
    mockPtyOf(*session).stdinBuffer().clear();
    clipboard->setText(QString((1024 * 1024) + 1, QChar('x')));
    CHECK_NOTHROW(session->pasteFromClipboard(1, false));
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // > 512 KB: requires permission; nothing written until granted.
    int permissionRequests = 0;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::requestPermissionForPasteLargeFile,
                     [&] { ++permissionRequests; });
    clipboard->setText(QString((1024 * 512) + 1, QChar('y')));
    session->pasteFromClipboard(1, false);
    CHECK(permissionRequests == 1);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // "Yes to all" applies the paste and stores the verdict — and (#2089) the next big paste must resolve
    // from that memory rather than asking a second time. Only requestPermission() reads the remembered
    // answer, so a bare emit here would ask again forever.
    session->applyPendingPaste(/*allow=*/true, /*remember=*/true);
    CHECK(mockPtyOf(*session).stdinBuffer().contains("yyy"));

    mockPtyOf(*session).stdinBuffer().clear();
    session->pasteFromClipboard(1, false);
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
        session->pasteFromClipboard(1, /*strip=*/false);
        auto const smallPaste = mockPtyOf(*session).stdinBuffer();
        REQUIRE_FALSE(smallPaste.empty());

        // Sized against the *normalized* line: normalization runs before the soft limit is measured, so
        // a count derived from the CRLF length lands under that limit wherever CRLF becomes LF -- and
        // the paste would take the immediate path, leaving the deferred one untested.
        auto const repeats = static_cast<int>(((size_t { 1024 } * 512) / smallPaste.size()) + 1);
        mockPtyOf(*session).stdinBuffer().clear();
        clipboard->setText(line.repeated(repeats));
        session->pasteFromClipboard(1, /*strip=*/false);
        REQUIRE(mockPtyOf(*session).stdinBuffer().empty()); // held for the verdict, not sent

        session->applyPendingPaste(/*allow=*/true, /*remember=*/false);

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
        session->pasteFromClipboard(1, /*strip=*/false);

        // The user is looking at a dialog about the 'y' payload; something else lands on the clipboard.
        clipboard->setText(QStringLiteral("substituted"));
        session->applyPendingPaste(/*allow=*/true, /*remember=*/false);

        auto const& written = mockPtyOf(*session).stdinBuffer();
        CHECK(written.contains("yyy"));
        CHECK_FALSE(written.contains("substituted"));
    }

    SECTION("a remembered refusal keeps refusing")
    {
        auto asks = 0;
        QObject::connect(session.get(),
                         &contour::session::TerminalSession::requestPermissionForPasteLargeFile,
                         [&asks] { ++asks; });

        clipboard->setText(QString((1024 * 512) + 1, QChar('y')));
        session->pasteFromClipboard(1, /*strip=*/false);
        REQUIRE(asks == 1);

        session->applyPendingPaste(/*allow=*/false, /*remember=*/true);
        CHECK(mockPtyOf(*session).stdinBuffer().empty());

        session->pasteFromClipboard(1, /*strip=*/false);
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
    for (int row = controller->count() - 1; row >= 0; --row)
        controller->closeTabAtIndex(row);
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
        session->terminal().writeToScreen("\033]133;A\033\\$ \033]133;B\033\\" + command + "\r\n");
        session->terminal().writeToScreen("\033]133;C\033\\");
        for (auto i = 0; i < outputLines; ++i)
            session->terminal().writeToScreen(std::format("out {}\r\n", i));
        session->terminal().writeToScreen("\033]133;D;0\033\\");
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

    auto stop = std::atomic<bool> { false };
    auto writer = std::thread { [&]() {
        // Stands in for Terminal::processInputOnce(): writeToScreen() takes the terminal lock around
        // the parse exactly as the real parser thread does.
        while (!stop.load(std::memory_order_acquire))
        {
            session->terminal().writeToScreen("hello \033[1mworld\033[m\r\n");
            std::this_thread::yield();
        }
    } };

    auto constexpr ToggleCount = 100;
    for ([[maybe_unused]] auto const i: std::views::iota(0, ToggleCount))
        (*session)(actions::ViNormalMode {});

    stop.store(true, std::memory_order_release);
    writer.join();

    // An even number of toggles lands back in Insert mode, and the terminal is still consistent.
    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Insert);
    CHECK(session->terminal().totalPageSize().lines > vtbackend::LineCount(0));
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
    auto const restore = crispy::Finally { [&] {
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
    session->sendMousePressEvent(Modifiers {}, MouseButton::Left, at);
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
    session->executeShowHostWritableStatusLine(false, true);
    CHECK(session->terminal().statusDisplayType() != vtbackend::StatusDisplayType::HostWritable);

    // Allow + remember: switches the status display to host-writable and unsyncs the title.
    session->executeShowHostWritableStatusLine(true, true);
    CHECK(session->terminal().statusDisplayType() == vtbackend::StatusDisplayType::HostWritable);
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
    session->applyPendingFontChange(false, true);
    session->applyPendingFontChange(true, false); // no pending change left: early-out
    SUCCEED("font permission paths are display-safe");
}

TEST_CASE("TerminalSession: clipboard, selection and notification paths are display-safe",
          "[contour][session][guards]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    session->copyToClipboard("hello clipboard"); // display-less: guarded no-op
    session->onSelectionCompleted();             // display-less: guarded no-op
    session->focusTerminalWindow();              // display-less: guarded no-op
    session->discardDesktopNotification("no-such-id");

    // Multi-count paste replicates the clipboard text N times into the PTY.
    auto& pty = mockPtyOf(*session);
    if (QClipboard* clipboard = QGuiApplication::clipboard(); clipboard != nullptr)
    {
        clipboard->setText(QStringLiteral("ab"));
        pty.stdinBuffer().clear();
        session->pasteFromClipboard(3, /*strip*/ false);
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

    // Where a notification ends up is platform-dependent: Linux raises it through the desktop
    // notifier, and every other platform hands it to the QML tray icon as a signal instead. Record
    // both, and assert against whichever one this build actually uses.
    auto shown = std::vector<std::pair<QString, QString>> {};
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::showNotification,
                     [&](QString const& title, QString const& body) { shown.emplace_back(title, body); });

    // A plain notification is passed on as it was parsed.
    auto plain = vtbackend::DesktopNotification {};
    plain.identifier = "n1";
    plain.title = "Build finished";
    plain.body = "OK";
    session->showDesktopNotification(plain);

#ifdef __linux__
    REQUIRE(raised->raised.size() == 1);
    CHECK(raised->raised[0].identifier == "n1");
    CHECK(raised->raised[0].title == "Build finished");
    CHECK(raised->raised[0].body == "OK");
#else
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].first == QStringLiteral("Build finished"));
    CHECK(shown[0].second == QStringLiteral("OK"));
    // One route or the other, never both -- a notification shown twice is a bug the user sees.
    CHECK(raised->raised.empty());
#endif

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

    SECTION("discarding a notification asks the notifier to withdraw it")
    {
        // Not platform-conditional: withdrawing goes through the notifier everywhere, which off
        // Linux is a NullNotifier that accepts it and does nothing.
        session->discardDesktopNotification("n2");

        REQUIRE(raised->closed.size() == 1);
        CHECK(raised->closed[0] == "n2");
    }

// The OSC 99 close/activation replies exist only where a notifier raises the notification in the
// first place; off Linux the tray icon reports nothing back, so there is nothing to assert.
#ifdef __linux__
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
#endif
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

TEST_CASE("TerminalSession: a spontaneous early exit shows the notice and a key press closes it",
          "[contour][session][close]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    bool closed = false;
    QObject::connect(session.get(),
                     &contour::session::TerminalSession::sessionClosed,
                     [&closed](contour::session::TerminalSession&) { closed = true; });

    // The shell "dies" moments after startup: onClosed() must route through the early-exit notice
    // (deliberately NOT emitting sessionClosed) because the default early-exit threshold is 5s.
    pty.close();
    session->onClosed();
    CHECK_FALSE(closed);

    // The acknowledging key press prunes the pane: sessionClosed fires now.
    session->sendCharEvent(U'x',
                           vtbackend::KeyIdentity { .unshiftedKey = U'x' },
                           vtbackend::Modifiers {},
                           vtbackend::KeyboardEventType::Press,
                           std::chrono::steady_clock::now());
    CHECK(closed);
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

// Only Linux raises these through the notifier; elsewhere they go to the QML tray icon and the
// per-request report wiring below does not exist at all. @see the display-safe case above.
#ifdef __linux__
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
#endif

    // Withdrawing runs through the notifier on every platform.
    session->discardDesktopNotification("note-1");
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
    // title and body and passes that on -- to the notifier on Linux, to the tray icon elsewhere.
    session->notify("Build finished", "All targets are up to date");

#ifdef __linux__
    REQUIRE(raised->raised.size() == 1);
    CHECK(raised->raised[0].title == "Build finished");
    CHECK(raised->raised[0].body == "All targets are up to date");
#else
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].first == QStringLiteral("Build finished"));
    CHECK(shown[0].second == QStringLiteral("All targets are up to date"));
    // One route or the other, never both -- a notification shown twice is a bug the user sees.
    CHECK(raised->raised.empty());
#endif
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
    // FocusNextSearchMatch/FocusPreviousSearchMatch return true (and move the normal-mode cursor) only
    // when a search has matches; with no active search they return false. Drive both paths.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;

    for (int i = 0; i < 20; ++i)
        session->terminal().writeToScreen(std::format("needle row {} needle\r\n", i));

    // No search set yet: nothing to focus.
    CHECK_FALSE((*session)(actions::FocusNextSearchMatch {}));

    // Set a search pattern through the terminal, then the match-focus actions have somewhere to go.
    {
        auto const l = std::scoped_lock { session->terminal() };
        session->terminal().setNewSearchTerm(U"needle", vtbackend::SearchOrigin::Typed);
    }
    // At least one of next/prev must find a match now (position-dependent), and neither throws.
    auto const nextFound = (*session)(actions::FocusNextSearchMatch {});
    auto const prevFound = (*session)(actions::FocusPreviousSearchMatch {});
    CHECK((nextFound || prevFound));
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

        // Smart, and unpinned: the glyph is the capitalised one and the button is not lit.
        CHECK(session->searchCaseGlyph() == QStringLiteral("Aa"));
        CHECK_FALSE(session->searchCasePinned());
        auto const smart = session->searchSummary();

        // Smart -> Sensitive. Still "Aa", now lit, and the uppercase line stops counting.
        session->cycleSearchCaseSensitivity();
        CHECK(session->searchCaseGlyph() == QStringLiteral("Aa"));
        CHECK(session->searchCasePinned());
        CHECK(session->searchSummary() != smart);

        // Sensitive -> Insensitive. THIS is the one that shows the lowercase glyph. An earlier
        // version had it the other way round, and a test asserting enumerator integers could not
        // tell -- which is why this asserts what the user actually sees.
        session->cycleSearchCaseSensitivity();
        CHECK(session->searchCaseGlyph() == QStringLiteral("aa"));
        CHECK(session->searchCasePinned());
        CHECK(session->searchHasMatches());

        // Insensitive -> Smart, closing the cycle.
        session->cycleSearchCaseSensitivity();
        CHECK(session->searchCaseGlyph() == QStringLiteral("Aa"));
        CHECK_FALSE(session->searchCasePinned());
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

    CHECK((*session)(actions::OpenConfiguration { .inEditor = true }));
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
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    // A URL with an explicit scheme is opened verbatim (no filesystem probe).
    session->openDocument("https://contour-terminal.org/");
    REQUIRE(launcher.openedUrls.size() == 1);
    CHECK(launcher.openedUrls.back().toString().toStdString() == "https://contour-terminal.org/");

    // A scheme-less path that EXISTS is resolved to an absolute file:// URL.
    QTemporaryDir const dir;
    auto const filePath = std::filesystem::path(dir.path().toStdString()) / "doc.txt";
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

namespace
{

/// The line sessionCapturing() puts on the screen, the APC introducer a capture reply opens with, and
/// the empty terminating chunk that closes EVERY reply — including a refused one, which consists of
/// nothing else. Spelled out rather than derived from @c vtbackend::CaptureBufferCode, so a test pins
/// the bytes that go on the wire. @see docs/vt-extensions/buffer-capture.md
constexpr auto SeededLine = std::string_view { "capture me" };
constexpr auto CaptureReplyMarker = std::string_view { "\033^314;" };
constexpr auto CaptureReplyTerminator = std::string_view { "\033^314;\033\\" };

/// Builds a session whose profile answers capture_buffer with @p permission, with a recording surface
/// attached — requestCaptureBuffer() early-returns without a display, so the gate is only reachable
/// with one. The screen is seeded and the PTY buffer cleared, so what a test reads back afterwards is
/// the capture and nothing else.
[[nodiscard]] SessionWithSurface sessionCapturing(contour::ContourGuiApp& app,
                                                  contour::config::Permission permission)
{
    auto const name = registerProfile(app, "capture-perm", [permission](contour::config::TerminalProfile& p) {
        p.permissions.value().captureBuffer = permission;
    });
    auto held = makeSessionWithSurface(app, name);
    held->terminal().writeToScreen(std::format("{}\r\n", SeededLine));
    mockPtyOf(*held).stdinBuffer().clear();
    return held;
}

/// Asks @p session to capture its whole page — the seeded line sits at the top, and a capture is
/// counted from the page's bottom upwards, so a shorter request would take only blank lines.
void captureWholePage(contour::session::TerminalSession& session)
{
    session.requestCaptureBuffer(session.terminal().pageSize().lines, /*logical=*/false);
}

/// Whether @p session's PTY holds a capture of the seeded screen. Both halves matter: capturing an
/// empty region still emits the closing marker, so the marker alone would not tell a capture that
/// carried the screen from one that carried nothing.
[[nodiscard]] bool capturedSeededScreen(contour::session::TerminalSession& session)
{
    auto const& written = mockPtyOf(session).stdinBuffer();
    return written.contains(CaptureReplyMarker) && written.contains(SeededLine);
}

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
    QObject::connect(
        &session, &contour::session::TerminalSession::requestPermissionForBufferCapture, [&asks] { ++asks; });
}

} // namespace

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
        held->executePendingBufferCapture(/*allow=*/true, /*remember=*/true);
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

        held->executePendingBufferCapture(/*allow=*/false, /*remember=*/true);
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
        held->executePendingBufferCapture(/*allow=*/true, /*remember=*/false);
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

namespace
{
/// Writes an OSC-8 hyperlink cell to @p session's screen and hovers the mouse over it, so
/// terminal().tryGetHoveringHyperlink() resolves — the precondition FollowHyperlink needs to reach
/// TerminalSession::followHyperlink() without a display. @p uri is the OSC-8 target.
void seedHoveredHyperlink(contour::session::TerminalSession& session, std::string const& uri)
{
    // OSC 8 ; params ; URI ST  <text>  OSC 8 ; ; ST
    session.terminal().writeToScreen(std::format("\033]8;;{}\033\\LINK\033]8;;\033\\", uri));
    // Hover the first cell of the link text (row 0, col 0 in screen space).
    session.terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { vtbackend::LineOffset(0), vtbackend::ColumnOffset(0) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
}
} // namespace

TEST_CASE("TerminalSession: FollowHyperlink routes each URI class through the external launcher",
          "[contour][session][hyperlink]")
{
    // followHyperlink() branches on the hovered link's URI: a remote link and a local non-file both
    // open via openUrl; a local link to an existing executable file runs `contour config ...` via the
    // launcher's execute(). Driving them headlessly (hover + FollowHyperlink) exercises all the
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
        CHECK(launcher.executed.empty());
    }

    SECTION("a local file:// link to a non-existent path opens via openUrl (not executed)")
    {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        seedHoveredHyperlink(*session, "file://localhost/no/such/file/here.txt");
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        CHECK(launcher.openedUrls.size() == 1);
        CHECK(launcher.executed.empty());
    }

    // A file:// URL that survives with its authority intact is read by the desktop's URL handler as a
    // network location ("//host/path"), which does not exist -- issue #2057. Every spelling of THIS
    // machine must therefore reach openUrl() as a plain local file URL.
    SECTION("every spelling of this host resolves to a plain local file URL")
    {
        auto const shortName = QHostInfo::localHostName().toStdString();

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
        CHECK(launcher.executed.empty());
    }

#ifndef _WIN32
    // The "is this an executable file" branch of followHyperlink() keys off POSIX owner-execute
    // permission; Windows determines executability by extension, not filesystem perms, so this Unix
    // semantics section does not apply there.
    SECTION("a local file:// link to an executable file runs `contour config` via execute()")
    {
        // The authority-free form reaches the executable branch too: it used to be classified as remote
        // (its host is ""), so a file:///path link to a script fell through to openUrl() instead.
        auto const authority =
            GENERATE(std::string {}, QHostInfo::localHostName().toStdString() + ".example.invalid");

        INFO("authority: " << authority);
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        // Create a real executable file and hover a file://<authority>/<path> link to it.
        QTemporaryDir const tmp;
        REQUIRE(tmp.isValid());
        auto const exePath = std::filesystem::path(tmp.path().toStdString()) / "script.sh";
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
        REQUIRE(launcher.executed.size() == 1);
        CHECK(launcher.executed.back().arguments.contains(QStringLiteral("config")));
        // The path reaches `contour config` as the filesystem spells it, not as a URL.
        CHECK(launcher.executed.back().arguments.back() == QString::fromStdString(exePath.generic_string()));
    }
#endif
}

TEST_CASE("TerminalSession: NewTerminal with spawn_new_process launches a detached process",
          "[contour][session][spawn]")
{
    // With spawn_new_process:true the NewTerminal action shells out via the launcher's runDetached()
    // rather than minting an in-process window (which would need the QML/window machinery). The
    // spawn command carries the `config` sub-command + the profile name.
    contour::test::TestApp testApp;
    auto const profileName =
        registerProfile(testApp.app(), "spawner", [](contour::config::TerminalProfile&) {});
    testApp.app().config().spawnNewProcess.value() = true;

    auto session = makeSessionWithProfile(testApp.app(), profileName);
    auto& launcher = testApp.launcher();

    CHECK((*session)(contour::actions::NewTerminal { .profileName = profileName }));
    // The spawn-process branch shelled out exactly once (rather than minting an in-process window),
    // carrying the profile name so the new process opens under the same profile.
    REQUIRE(launcher.detached.size() == 1);
    CHECK(launcher.detached.back().arguments.contains(QStringLiteral("profile")));
    CHECK(launcher.detached.back().arguments.contains(QString::fromStdString(profileName)));
}

TEST_CASE("TerminalSession: the opener actions log (do not crash) when the launcher fails",
          "[contour][session][actions]")
{
    // openUrl failing drives the "could not open" error-log branch of OpenConfiguration /
    // OpenFileManager / OpenSelection — a diagnostic, not a crash.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;
    testApp.launcher().openUrlError = contour::platform::LaunchError::NoHandler;

    // A local cwd, so OpenFileManager reaches its openUrl() (and thus the failure branch) rather than
    // short-circuiting on an absent/remote directory.
    session->terminal().setCurrentWorkingDirectory("file://" + QHostInfo::localHostName().toStdString()
                                                   + "/tmp");

    // in_editor so OpenConfiguration takes the openUrl() (file-open) path rather than the settings page.
    CHECK((*session)(actions::OpenConfiguration { .inEditor = true }));
    CHECK((*session)(actions::OpenFileManager {}));
    CHECK((*session)(actions::OpenSelection {}));
    CHECK(testApp.launcher().openedUrls.size() == 3); // all attempted despite the failure
}

TEST_CASE("TerminalSession: OpenFileManager strips the OSC 7 host and skips remote directories",
          "[contour][session][actions]")
{
    // OSC 7 reports the cwd as file://HOST/PATH. Handing that raw URL to the file manager makes its host
    // authority read as a network share ("//host/path"); it must be resolved to a plain local path, and
    // only for a directory on THIS host — a remote (SSH) cwd is not openable here.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;
    auto& launcher = testApp.launcher();

    SECTION("a working directory on this host opens as a plain local path")
    {
        session->terminal().setCurrentWorkingDirectory("file://" + QHostInfo::localHostName().toStdString()
                                                       + "/home/user/proj");
        CHECK((*session)(actions::OpenFileManager {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        // The host authority is gone: a local file URL, not a //host network path.
        CHECK(launcher.openedUrls.back().isLocalFile());
        CHECK(launcher.openedUrls.back().toLocalFile() == QStringLiteral("/home/user/proj"));
    }

    SECTION("a remote (SSH) working directory is not opened")
    {
        session->terminal().setCurrentWorkingDirectory("file://some-remote-host/home/user");
        CHECK((*session)(actions::OpenFileManager {}));
        CHECK(launcher.openedUrls.empty()); // nothing local to open
    }
}

TEST_CASE("TerminalSessionManager::setFocusedSession moves terminal focus symmetrically",
          "[contour][session][focus]")
{
    // The manager is the single authority for terminal (VT) focus: every focus move — Qt display
    // focus events AND model active-tab/active-pane changes (including cross-window tab moves) — routes
    // through setFocusedSession, which must send a focus-OUT to the previously focused session and a
    // focus-IN to the new one so exactly one terminal is focused at a time. This is what keeps the
    // focused (i-beam) vs. unfocused (hollow block) cursor correct after a tab/pane switch where no Qt
    // focus event fires (e.g. a session swapped onto a reused display). terminal().focused() is
    // display-independent, so this is exercised headless.
    contour::test::TestApp testApp;
    auto& manager = testApp.app().sessionsManager();
    auto a = makeDisplaylessSession(testApp.app());
    auto b = makeDisplaylessSession(testApp.app());

    // Focus A, then B: the transition must focus out A and focus in B (symmetric out/in).
    manager.setFocusedSession(a.get());
    manager.setFocusedSession(b.get());
    CHECK_FALSE(a->terminal().focused());
    CHECK(b->terminal().focused());

    // Move focus back to A: symmetric the other way.
    manager.setFocusedSession(a.get());
    CHECK(a->terminal().focused());
    CHECK_FALSE(b->terminal().focused());

    // Idempotent: re-focusing the already-focused session emits no further events and changes nothing.
    manager.setFocusedSession(a.get());
    CHECK(a->terminal().focused());
    CHECK_FALSE(b->terminal().focused());

    // Clearing focus (nullptr) removes focus from the current session, leaving nothing focused.
    manager.setFocusedSession(nullptr);
    CHECK_FALSE(a->terminal().focused());
    CHECK_FALSE(b->terminal().focused());
}

TEST_CASE("TerminalSession: Ctrl+Shift+P opens the palette instead of reaching the shell",
          "[contour][session][input][palette]")
{
    // Ctrl+Shift+P must be CONSUMED by its binding. If the action did not fire (or reported that it had
    // not applied), the chord would fall through to the terminal and the shell would receive a stray
    // control byte — the failure mode of every mis-wired keybinding.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    auto const ctrlShift = Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift };

    // Ctrl+printable arrives as a CHARACTER (Qt::Key_P is not in helper.cpp's KeyMappings table), which
    // is why the default binding is a CharInputMapping on 'P' rather than a KeyInputMapping.
    session->sendCharEvent(
        U'P', vtbackend::KeyIdentity { .unshiftedKey = U'P' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // The session is not registered with a window here, so the palette has nowhere to open — the point
    // is that the action still consumed the key rather than letting it through. A chord with no binding,
    // by contrast, is encoded and written to the PTY.
    session->sendCharEvent(
        U'Y', vtbackend::KeyIdentity { .unshiftedKey = U'Y' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: Ctrl+Shift+, fires despite Qt delivering the shifted '<'",
          "[contour][session][input]")
{
    // The default binds Ctrl+Shift+',' to OpenConfiguration, but Qt reports a Shift+punctuation chord as
    // the shifted keysym — comma+Shift arrives as '<' (Qt::Key_Less). Without base-char normalization the
    // chord would miss its binding and fall through to the shell (a stray '<'). It must be CONSUMED.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();
    auto const ctrlShift = Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift };

    session->sendCharEvent(
        U'<', vtbackend::KeyIdentity { .unshiftedKey = U'<' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // consumed by the ',' binding via its base char

    // A shifted symbol whose base key has no binding still reaches the terminal (retry misses, falls
    // through) — the normalization only rescues chords that are actually bound.
    session->sendCharEvent(U'~',
                           vtbackend::KeyIdentity { .unshiftedKey = U'~' },
                           ctrlShift,
                           KeyboardEventType::Press,
                           now); // base '`', unbound
    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: executeAction runs a palette-picked command", "[contour][session][palette]")
{
    // The palette does not synthesize key events — it hands the chosen action straight to the session
    // through executeAction(), the same visit a key binding takes. This is that path, and it is the one
    // that had to be made public for the palette to exist.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // SendChars writes to the PTY, so a successful dispatch is directly observable.
    CHECK(
        session->executeAction(contour::actions::Action { contour::actions::SendChars { .chars = "xyz" } }));
    CHECK(mockPtyOf(*session).stdinBuffer() == "xyz");

    // An action may decline (FollowHyperlink with no link under the cursor), and executeAction reports
    // that faithfully — which is exactly what lets a key binding fall through to the terminal.
    CHECK_FALSE(session->executeAction(contour::actions::Action { contour::actions::FollowHyperlink {} }));
}

TEST_CASE("TerminalSession: a Ctrl-spelled chord fires its binding (issue #1987)",
          "[contour][session][input]")
{
    // End-to-end counterpart to the parse-level test in Config_test.cpp. That one proves a binding
    // OBJECT exists; this proves the chord actually fires — which additionally exercises the exact
    // modifier equality in apply, the mode gate, and the codepoint the input routes deliver.
    //
    // NB: this drives sendCharEvent directly rather than a QKeyEvent, so it is independent of
    // makeModifiers and runs identically on every platform. (On Windows the real Qt path additionally
    // strips Ctrl+Alt as AltGr, which is a separate pre-existing limitation.)
    TestApp testApp;
    testApp.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Shift,Alt,Ctrl], key: 'Q', action: ClearHistoryAndReset }
)")
                                               .inputMappings;

    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    // Give the action something to actually do, so this asserts the action RAN rather than merely
    // that something ate the keystroke.
    for ([[maybe_unused]] auto const _: std::views::iota(0, 40))
        session->terminal().writeToScreen("scrollback\r\n");
    REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));

    mockPtyOf(*session).stdinBuffer().clear();
    auto const chord =
        Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Alt, vtbackend::Modifier::Shift };
    session->sendCharEvent(
        U'Q', vtbackend::KeyIdentity { .unshiftedKey = U'Q' }, chord, KeyboardEventType::Press, now);

    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // consumed by the binding
    CHECK(session->terminal().primaryScreen().historyLineCount() == vtbackend::LineCount(0));

    // A subset of the chord must NOT match: apply compares modifiers with ==, and this is the
    // property that keeps Ctrl+Q from firing a binding written for Ctrl+Alt+Shift+Q.
    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(U'Q',
                           vtbackend::KeyIdentity { .unshiftedKey = U'Q' },
                           Modifiers { vtbackend::Modifier::Control },
                           KeyboardEventType::Press,
                           now);
    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: a lowercase `key:` binding fires", "[contour][session][input]")
{
    // A single-character binding is stored folded, and the delivered codepoint is folded to match, so
    // the case the user happened to write is irrelevant. Before that, `key: 'p'` parsed cleanly and
    // produced a binding that could never fire — the same silent symptom as issue #1987.
    //
    // The `input_mapping:` section replaces the built-in defaults, so this config holds exactly one
    // binding and the test cannot pass by accident through the default Ctrl+Shift+P.
    TestApp testApp;
    testApp.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control, Shift], key: 'p', action: OpenCommandPalette }
)")
                                               .inputMappings;

    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();
    auto const ctrlShift = Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift };

    // Both cases must fire: which one arrives depends on the route the Qt event took, not the user.
    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(
        U'P', vtbackend::KeyIdentity { .unshiftedKey = U'P' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(
        U'p', vtbackend::KeyIdentity { .unshiftedKey = U'p' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // An unbound letter still reaches the shell — the fold must not swallow everything.
    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(
        U'y', vtbackend::KeyIdentity { .unshiftedKey = U'y' }, ctrlShift, KeyboardEventType::Press, now);
    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
}

TEST_CASE("TerminalSession: a tab-strip swipe follows the finger while a wheel tilt does not",
          "[contour][session][wheel]")
{
    // End-to-end for the direction rule, with real tabs behind a mock PTY factory. Both events below
    // carry the SAME sign, so any difference in where they land is the rule itself and nothing else.
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    TestApp testApp(std::move(factoryOwned));
    contour::test::ScopedController const controller(testApp.manager());

    controller->createNewTab();
    controller->createNewTab();
    controller->createNewTab();
    REQUIRE(controller->count() == 3);

    auto constexpr NoPhase = static_cast<int>(vtbackend::ScrollPhase::NoPhase);
    auto constexpr Begin = static_cast<int>(vtbackend::ScrollPhase::Begin);

    SECTION("a leftward swipe moves to the tab on the right")
    {
        controller->activateTab(0);
        REQUIRE(controller->activeTabIndex() == 0);

        // Fingers drag the content left, so what was to the right comes into view -- the carousel
        // sense a browser and a photo viewer use for the same gesture.
        controller->dispatchTabStripWheel(-120, 0, 0, 0, Begin, /*inverted=*/false);
        CHECK(controller->activeTabIndex() == 1);
    }

    SECTION("a leftward wheel tilt moves to the tab on the left")
    {
        controller->activateTab(0);
        REQUIRE(controller->activeTabIndex() == 0);

        // Same sign, no pixel precision: a tilt means the direction it names, and SwitchToTabLeft wraps.
        controller->dispatchTabStripWheel(0, 0, -120, 0, NoPhase, /*inverted=*/false);
        CHECK(controller->activeTabIndex() == 2);
    }

    SECTION("a platform that already inverted the delta is not inverted twice")
    {
        controller->activateTab(0);
        REQUIRE(controller->activeTabIndex() == 0);

        // Same swipe as the first section, but the platform's own natural-scrolling setting already
        // flipped it. Inverting again would undo that and send the user the wrong way.
        controller->dispatchTabStripWheel(-120, 0, 0, 0, Begin, /*inverted=*/true);
        CHECK(controller->activeTabIndex() == 2);
    }

    for (int row = controller->count() - 1; row >= 0; --row)
        controller->closeTabAtIndex(row);
}

namespace
{
/// An Announcer that records instead of speaking, so the DECISIONS are assertable with no
/// accessibility bridge in sight (offscreen QPA has none).
class RecordingAnnouncer final: public contour::platform::Announcer
{
  public:
    struct Said
    {
        QString message;
        QAccessible::AnnouncementPoliteness politeness;
    };

    void announce(QString const& message, QAccessible::AnnouncementPoliteness politeness) override
    {
        said.push_back({ message, politeness });
    }

    std::vector<Said> said;
};
} // namespace

TEST_CASE("TerminalSession: things with no place in the accessibility tree are announced",
          "[contour][session][a11y]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    auto announcer = std::make_unique<RecordingAnnouncer>();
    auto* recorder = announcer.get();
    session->setAnnouncer(std::move(announcer));

    SECTION("the bell rings for a screen reader too")
    {
        // A bell changes no object's state, so without this nothing reaches an assistive client at all.
        session->bell();
        REQUIRE(recorder->said.size() == 1);
        CHECK(recorder->said.front().message == "Bell");
        CHECK(recorder->said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);
    }

    SECTION("a read-only toggle interrupts, because it changes what typing does")
    {
        (*session)(contour::actions::ToggleInputProtection {});
        REQUIRE(recorder->said.size() == 1);
        // Assertive: telling the user AFTER they have typed into a terminal that ignored them is too
        // late to be worth saying.
        CHECK(recorder->said.front().politeness == QAccessible::AnnouncementPoliteness::Assertive);
        auto const first = recorder->said.front().message;

        // ...and toggling back says the opposite, rather than repeating itself.
        (*session)(contour::actions::ToggleInputProtection {});
        REQUIRE(recorder->said.size() == 2);
        CHECK(recorder->said.back().message != first);
    }

    SECTION("switching announcements off silences them")
    {
        // The gate is checked at the call, so a user who does not want them pays nothing at all.
        testApp.app().config().accessibilityAnnouncements = false;
        auto quiet = makeDisplaylessSession(testApp.app());
        auto quietAnnouncer = std::make_unique<RecordingAnnouncer>();
        auto* quietRecorder = quietAnnouncer.get();
        quiet->setAnnouncer(std::move(quietAnnouncer));

        quiet->bell();
        (*quiet)(contour::actions::ToggleInputProtection {});
        CHECK(quietRecorder->said.empty());
    }
}

// {{{ What a session asks of its VIEW
//
// Every case above runs display-less, because until session::DisplaySurface existed the only view was a
// QQuickItem needing a window, a scene graph and an RHI device — so "does this reach the display?" could
// not be asked at all, only "does it survive the display being absent?". These attach a recording
// surface and assert the other half. The SessionWithSurface fixture they share lives up with the other
// session factories, because the permission cases need it too.

TEST_CASE("TerminalSession::attachDisplay hands the surface the state it missed", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;

    // The surface IS the session's view now, by the interface and not by the concrete type.
    CHECK(held->display() == &surface);

    // Attach establishes the pointer shape (the primary screen's default, since no OSC 22 is
    // outstanding) and asks for a frame — a freshly bound pane must not stay blank until the next
    // keystroke.
    REQUIRE_FALSE(surface.cursorShapes.empty());
    CHECK(surface.cursorShapes.back() == contour::input::MouseCursorShape::IBeam);
    CHECK(surface.redrawCount >= 1);
}

TEST_CASE("TerminalSession routes the application's pointer shape to the surface", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;
    surface.cursorShapes.clear();

    // OSC 22 names a CSS pointer; the surface speaks the input:: enum, and the mapping is the point.
    held->terminal().writeToScreen("\033]22;pointer\033\\");
    REQUIRE_FALSE(surface.cursorShapes.empty());
    CHECK(surface.cursorShapes.back() == contour::input::MouseCursorShape::PointingHand);

    // Withdrawing it returns to the screen-type default rather than leaving the last shape pinned.
    surface.cursorShapes.clear();
    held->terminal().writeToScreen("\033]22;\033\\");
    REQUIRE_FALSE(surface.cursorShapes.empty());
    CHECK(surface.cursorShapes.back() == contour::input::MouseCursorShape::IBeam);
}

TEST_CASE("TerminalSession relays window-resize requests to the surface", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;

    // CSI 8 t: cells. The request crosses to the GUI thread through post(), which is why the fake runs
    // posted work immediately — the assertion is about what ARRIVES, not about the hop.
    held->requestWindowResize(vtbackend::LineCount(30), vtbackend::ColumnCount(90));
    REQUIRE(surface.pageResizeRequests.size() == 1);
    CHECK(surface.pageResizeRequests.front().first == vtbackend::LineCount(30));
    CHECK(surface.pageResizeRequests.front().second == vtbackend::ColumnCount(90));

    // CSI 4 t: device pixels, a separate entry point that must not be folded into the cell one.
    held->requestWindowResize(vtbackend::Width(800), vtbackend::Height(600));
    REQUIRE(surface.pixelResizeRequests.size() == 1);
    CHECK(surface.pixelResizeRequests.front().first == vtbackend::Width(800));
    CHECK(surface.pixelResizeRequests.front().second == vtbackend::Height(600));

    // And the window->grid direction is a third, distinct request.
    held->resizeTerminalToDisplaySize();
    CHECK(surface.terminalResizeRequests == 1);
}

TEST_CASE("TerminalSession arms the surface's screenshot capture", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;

    // SaveScreenshot names a file; CopyScreenshot asks for the frame in memory. Both go through the
    // same one-shot arm, and telling them apart is exactly what the variant is for.
    CHECK((*held)(contour::actions::SaveScreenshot {}));
    REQUIRE(surface.screenshotOutputs.size() == 1);
    REQUIRE(surface.screenshotOutputs.front().has_value());
    CHECK(std::holds_alternative<std::filesystem::path>(*surface.screenshotOutputs.front()));

    CHECK((*held)(contour::actions::CopyScreenshot {}));
    REQUIRE(surface.screenshotOutputs.size() == 2);
    REQUIRE(surface.screenshotOutputs.back().has_value());
    CHECK(std::holds_alternative<std::monostate>(*surface.screenshotOutputs.back()));
}

TEST_CASE("TerminalSession relays the window-scoped view actions", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;

    // These are WINDOW decisions the session is asked to make while knowing only its surface, which
    // forwards them to display::WindowHost. What is assertable here is that they leave the session.
    CHECK((*held)(contour::actions::ToggleFullscreen {}));
    CHECK(surface.fullScreenToggles == 1);

    CHECK((*held)(contour::actions::ToggleTitleBar {}));
    CHECK(surface.titleBarToggles == 1);

    CHECK((*held)(contour::actions::ToggleInputMethodHandling {}));
    CHECK(surface.imeToggles == 1);

    CHECK((*held)(contour::actions::SetTabBarVisibility { contour::config::TabBarVisibility::Never }));
    REQUIRE(surface.tabBarVisibilities.size() == 1);
    CHECK(surface.tabBarVisibilities.front() == contour::config::TabBarVisibility::Never);

    CHECK((*held)(contour::actions::SetTabBarPosition { contour::config::TabBarPosition::Bottom }));
    REQUIRE(surface.tabBarPositions.size() == 1);
    CHECK(surface.tabBarPositions.front() == contour::config::TabBarPosition::Bottom);
}

TEST_CASE("TerminalSession reports a caret move once per pending post", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;

    // The caret notification fires once per frame AND twice a second from the blink, so it coalesces
    // to at most one outstanding post. Hold the queue to observe that: three notifications, one post.
    surface.runPostsImmediately = false;
    for (auto i = 0; i < 3; ++i)
        held->cursorPositionChanged();
    CHECK(surface.pendingPosts.size() == 1);

    surface.drainPosts();
    CHECK(surface.cursorMovedReports == 1);

    // Draining re-arms it: the coalescing latch must not stay set for the session's life.
    held->cursorPositionChanged();
    CHECK(surface.pendingPosts.size() == 1);
    surface.drainPosts();
    CHECK(surface.cursorMovedReports == 2);
}

TEST_CASE("TerminalSession tells the surface which screen buffer is live", "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;
    surface.bufferChanges.clear();

    // DECSET 1049: to the alternate screen and back. The surface keys state to the buffer, so it must
    // be told — and told the direction, not merely that something changed.
    held->terminal().writeToScreen("\033[?1049h");
    REQUIRE_FALSE(surface.bufferChanges.empty());
    CHECK(surface.bufferChanges.back() == vtbackend::ScreenType::Alternate);

    held->terminal().writeToScreen("\033[?1049l");
    CHECK(surface.bufferChanges.back() == vtbackend::ScreenType::Primary);
}

TEST_CASE("TerminalSession::attachDisplay releases a surface it is taking the session from",
          "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    // One session, one surface: binding a second must tell the first to let go, or both believe they
    // own the session and the loser's later detach trips the precondition. (The split hand-off.)
    auto second = contour::test::FakeDisplaySurface {};
    second.attachedSession = held.session.get();
    held->attachDisplay(second);

    CHECK(held.surface->releaseSessionCount == 1);
    CHECK(held->display() == &second);

    held->detachDisplay(second);
}
// }}}

TEST_CASE("TerminalSession survives a synchronized-output frame with a surface attached",
          "[contour][session][view]")
{
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    // DECRST 2026 ends a synchronized update, and Terminal::synchronizedOutput() reports the frame to
    // the event listener while the parser thread still HOLDS the state mutex (it calls
    // refreshRenderBuffer(true), i.e. "already locked"). A screenUpdated() that takes that same
    // non-recursive mutex therefore self-deadlocks on the very sequence neovim, tmux and every other
    // modern TUI ends each frame with.
    held->terminal().writeToScreen("\033[?2026h");
    held->terminal().writeToScreen("hello");
    held->terminal().writeToScreen("\033[?2026l");

    CHECK(held->terminal().primaryScreen().grid().lineText(vtbackend::LineOffset(0)).starts_with("hello"));
}

TEST_CASE("TerminalSession publishes the scrollbar's travel whenever folding moves it",
          "[contour][session][actions][folding][view]")
{
    // historyLineCount is what QML sizes the scrollbar from, and it reports what was last PUBLISHED
    // rather than a count of its own. Every action that moves rows into or out of the scrollable range
    // therefore has to say so -- including the ones that only ever reveal output.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& terminal = held->terminal();
    namespace actions = contour::actions;

    auto const runCommand = [&](std::string const& command, int outputLines) {
        terminal.writeToScreen("\033]133;A\033\\$ \033]133;B\033\\" + command + "\r\n");
        terminal.writeToScreen("\033]133;C\033\\");
        for (auto i = 0; i < outputLines; ++i)
            terminal.writeToScreen(std::format("out {}\r\n", i));
        terminal.writeToScreen("\033]133;D;0\033\\");
    };

    // Deep enough that folding the output actually shortens the scrollable range.
    runCommand("ls", 40);
    runCommand("pwd", 40);
    REQUIRE(terminal.foldRanges().size() == 2);

    auto const expanded = held->historyLineCount();
    REQUIRE(expanded > 0);

    REQUIRE((*held)(actions::CollapseAllFolds {}));
    auto const collapsed = held->historyLineCount();
    CHECK(collapsed < expanded);

    // The one that used to bypass the announce: the rows come back, and so must the travel.
    REQUIRE((*held)(actions::ExpandAllFolds {}));
    CHECK(held->historyLineCount() == expanded);
}

TEST_CASE("TerminalSession::attachDisplay publishes the scrollbar's travel it accumulated unseen",
          "[contour][session][view]")
{
    // A background pane has no display, and screenUpdated() -- the only other publisher -- returns
    // early without one. Binding a display therefore has to publish, or the freshly shown scrollbar is
    // sized for a history the session had when its display went away: zero, for one that never had a
    // display, and an idle shell never produces the screen update that would correct it.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    for (auto i = 0; i < 100; ++i)
        session->terminal().writeToScreen(std::format("line {}\r\n", i));

    REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));
    REQUIRE(session->historyLineCount() == 0);

    auto surface = contour::test::FakeDisplaySurface {};
    surface.attachedSession = session.get();
    session->attachDisplay(surface);

    CHECK(session->historyLineCount() == unbox<int>(session->terminal().viewport().scrollableLineCount()));
    CHECK(session->historyLineCount() > 0);

    session->detachDisplay(surface);
}

TEST_CASE("TerminalSession: a find-bar search into the scrollback outlives the auto-scroll",
          "[contour][session][search][view]")
{
    // The regression a display-less session cannot see, because screenUpdated() returns before the
    // auto-scroll without one. Search used to force Vi Normal mode, which is the only reason that
    // auto-scroll ("Insert mode and the viewport is scrolled -> jump to the bottom") never collided
    // with it. The find bar deliberately switches no mode, so a search now runs in Insert mode -- and
    // both Terminal::searchReverse() and ViCommands::moveCursorTo() reveal their match and then raise
    // screenUpdated(), which would scroll the viewport straight back off it.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    held->terminal().writeToScreen("needle in the haystack\r\n");
    for (auto const i: std::views::iota(0, 100))
        held->terminal().writeToScreen(std::format("filler {}\r\n", i));

    REQUIRE(held->terminal().inputHandler().mode() == vtbackend::ViMode::Insert);
    REQUIRE_FALSE(held->terminal().viewport().scrolled());

    held->searchBarOpened();
    held->setSearchPattern(QStringLiteral("needle"));

    CHECK(held->searchHasMatches());
    // The match lives far above the page, so the viewport must have travelled to it AND stayed.
    CHECK(held->terminal().viewport().scrolled());

    // Output arriving while the bar is open must not yank it back either -- the user is reading
    // something they went looking for.
    held->terminal().writeToScreen("more output\r\n");
    CHECK(held->terminal().viewport().scrolled());

    // Closing the bar hands the terminal back to its ordinary follow-the-output behaviour.
    held->searchBarClosed();
    held->terminal().writeToScreen("and more\r\n");
    CHECK_FALSE(held->terminal().viewport().scrolled());
}

TEST_CASE("TerminalSession: stepping PAST the last match keeps the one already revealed",
          "[contour][session][search][view]")
{
    // F3 is bound to the Search match mode -- "a pattern is set", which outlives the bar -- so
    // stepping runs with the bar shut and only _searchMatchRevealed holding the viewport in place. A
    // step that finds nothing must therefore leave that flag as it found it: the user is still
    // standing on the match the PREVIOUS step revealed, and clearing it here handed the viewport back
    // to the auto-scroll, which threw that match away on the very next byte of output.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    held->terminal().writeToScreen("needle one\r\n");
    for (auto const i: std::views::iota(0, 50))
        held->terminal().writeToScreen(std::format("filler {}\r\n", i));
    held->terminal().writeToScreen("needle two\r\n");
    for (auto const i: std::views::iota(50, 100))
        held->terminal().writeToScreen(std::format("filler {}\r\n", i));

    // Reach the nearer of the two matches through the bar, then shut it: from here on, only the
    // revealed-match flag can hold the viewport.
    held->searchBarOpened();
    held->setSearchPattern(QStringLiteral("needle"));
    REQUIRE(held->searchHasMatches());
    held->searchBarClosed();

    // A step that DOES reveal something, which is what raises the flag in the first place.
    REQUIRE((*held)(contour::actions::FocusPreviousSearchMatch {}));
    REQUIRE(held->terminal().viewport().scrolled());

    // Back onto the last match, and then one step past it -- the step that finds nothing. Forwards
    // rather than backwards, because searchPrevMatch() at the very top of the scrollback cannot step
    // back at all and so re-finds the match it is standing on, reporting success. @see stepSearch.
    REQUIRE((*held)(contour::actions::FocusNextSearchMatch {}));
    CHECK_FALSE((*held)(contour::actions::FocusNextSearchMatch {}));

    held->terminal().writeToScreen("more output\r\n");
    CHECK(held->terminal().viewport().scrolled());

    // Dropping the pattern ends the parking with it. Nothing is left to step to -- the Search match
    // mode F3 is bound to is "a pattern is set" -- so the pane goes back to following its output,
    // rather than sitting off the bottom until the user scrolls back down by hand.
    held->terminal().clearSearch();
    held->terminal().writeToScreen("after the clear\r\n");
    CHECK_FALSE(held->terminal().viewport().scrolled());
}

TEST_CASE("TerminalSession applies the profile's history limits to the terminal",
          "[contour][session][history]")
{
    // Closes the last gap between the YAML and the grid: Config_test proves the two bounds survive
    // parsing, and vtbackend proves the grid evicts on them, but only this proves the session hands
    // the one to the other -- and it is the path every profile apply and config reload takes.
    TestApp testApp;
    auto& profile = *testApp.app().config().profile(testApp.app().profileName());
    auto history = profile.history.value();
    history.maxHistoryLineCount = vtbackend::LineCount(1000);
    history.hardLimit = vtbackend::LineCount(2500);
    profile.history = history;

    auto session = makeDisplaylessSession(testApp.app());

    auto const& limits = session->terminal().primaryScreen().grid().historyLimits();
    CHECK(limits.guaranteed == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(1000) });
    CHECK(limits.capacity == vtbackend::MaxHistoryLineCount { vtbackend::LineCount(2500) });
    CHECK(limits.hasHeadroom());

    // And the ring really was allocated to the ceiling, not to the guarantee.
    CHECK(session->terminal().primaryScreen().grid().maxHistoryLineCount() == vtbackend::LineCount(2500));
}
