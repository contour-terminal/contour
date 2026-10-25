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

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/FinishNotification.hpp>
#include <contour/session/HyperlinkTooltip.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppearanceFixtures.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/Hyperlink.hpp>
#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/ProcessPlacement.hpp>
#include <vtpty/test/NonAsciiSample.hpp>
#include <vtpty/test/ProcessTesting.hpp>

#include <core/Escape.hpp>
#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
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
#include <QtGui/QGuiApplication>
#include <QtGui/QHoverEvent>
#include <QtGui/QMouseEvent>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <future>
#include <latch>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>
#include <vthost/testing/TestHelpers.hpp>

using namespace std::string_literals;

namespace
{

using contour::test::TestApp;

constexpr auto TestPageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

/// Creates a TerminalSession backed by @p pty, with NO display attached (the default state).
/// The session owns the PTY; the caller owns the session.
/// @param notifier the desktop notifier to inject; null (the default) takes the test notifier.
/// @param profileName the profile to run under; empty (the default) takes the app's default one.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeSessionWith(
    contour::ContourGuiApp& app,
    std::unique_ptr<vtpty::Pty> pty,
    std::unique_ptr<contour::platform::Notifier> notifier = nullptr,
    std::string profileName = {})
{
    // Pass the app's real session manager so the sessionClosed->removeSession wiring matches
    // production; we never pump the Qt event loop here, so that slot does not actually fire and the
    // test stays deterministic.
    return contour::test::makeTestSession(
        &app.sessionsManager(), std::move(pty), app, std::move(profileName), std::move(notifier));
}

/// The common case: a session over a plain MockPty.
/// @param notifier the desktop notifier to inject; null (the default) takes the test notifier.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> makeDisplaylessSession(
    contour::ContourGuiApp& app, std::unique_ptr<contour::platform::Notifier> notifier = nullptr)
{
    return makeSessionWith(app, std::make_unique<vtpty::MockPty>(TestPageSize), std::move(notifier));
}

/// Writes one finished command as a shell with OSC 133 integration does, and the next prompt below it:
/// the prompt `$ <typed>`, the output start (carrying @p startParameters), @p output as written, and the
/// command end with exit code 0.
/// @param session Where it is written.
/// @param typed The command line typed at the prompt.
/// @param output What the command printed, as written.
/// @param startParameters What the output start carries after its `C`, e.g. ";cmdline_url=make".
void writeFinishedBlock(contour::session::TerminalSession& session,
                        std::string_view typed,
                        std::string_view output,
                        std::string_view startParameters = {})
{
    session.terminal().writeToScreen(
        std::format("\033]133;A\033\\$ {}\r\n\033]133;C{}\033\\{}", typed, startParameters, output));
    session.terminal().writeToScreen("\033]133;D;0\033\\\033]133;A\033\\$ ");
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

/// The lines a command prints in the folding tests: `out 0`, `out 1`, and so on.
/// @param count How many lines.
/// @return The lines, for vtbackend::test::commandRun.
[[nodiscard]] std::vector<std::string> numberedOutput(int count)
{
    return std::ranges::to<std::vector>(
        std::views::iota(0, count) | std::views::transform([](int i) { return std::format("out {}", i); }));
}

/// numberedOutput()'s lines, each an OSC 8 hyperlink to a page of its own, so whichever row the pointer is on
/// holds one.
/// @param count How many lines.
/// @return The lines, for vtbackend::test::commandRun.
[[nodiscard]] std::vector<std::string> linkedOutput(int count)
{
    return std::ranges::to<std::vector>(
        std::views::iota(0, count) | std::views::transform([](int i) {
            return std::format("\033]8;;https://example.com/{}\033\\out {}\033]8;;\033\\", i, i);
        }));
}

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
    return makeSessionWith(
        app, std::make_unique<vtpty::MockPty>(TestPageSize), nullptr, std::move(profileName));
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

/// A directory of the in-memory filesystem a test app's sessions look paths up in. Absolute here: on Windows
/// a root directory without a drive is relative to the current drive.
#ifdef _WIN32
constexpr auto WorkDirectory = std::string_view { "C:/work" };
#else
constexpr auto WorkDirectory = std::string_view { "/work" };
#endif

/// @return The file @p name in WorkDirectory, as a program prints it.
[[nodiscard]] std::string inWorkDirectory(std::string_view name)
{
    return std::format("{}/{}", WorkDirectory, name);
}

/// @return The file @p name in WorkDirectory, as a path resolves to it: normalised, in this platform's
///         separators.
[[nodiscard]] std::string resolvedInWorkDirectory(std::string_view name)
{
    return core::pathToUtf8(
        core::pathFromUtf8(inWorkDirectory(name)).value_or(std::filesystem::path {}).lexically_normal());
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

// {{{ Scrollbar marks
namespace
{

/// Registers a profile that shows the scrollbar on the right with @p marks as its `scrollbar.marks`, and
/// keeps @p history lines of scrollback.
std::string registerMarkedScrollbarProfile(contour::ContourGuiApp& app,
                                           std::string const& name,
                                           vtbackend::ScrollbarMarkSources marks,
                                           vtbackend::LineCount history = vtbackend::LineCount(1000))
{
    return registerProfile(app, name, [marks, history](contour::config::TerminalProfile& p) {
        auto scrollbar = p.scrollbar.value();
        scrollbar.position = contour::config::ScrollBarPosition::Right;
        scrollbar.marks = marks;
        p.scrollbar = scrollbar;

        auto limits = p.history.value();
        limits.maxHistoryLineCount = history;
        p.history = limits;
    });
}

/// Writes @p count lines of plain output, as a command with no shell integration prints them.
void writeFiller(vtbackend::Terminal& terminal, int count)
{
    auto text = std::string {};
    for (auto const i: std::views::iota(0, count))
        text += std::format("filler {}\r\n", i);
    terminal.writeToScreen(text);
}

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

/// The kind of every tick @p session publishes, top first.
[[nodiscard]] std::vector<vtbackend::ScrollbarMarkKind> markKindsOf(
    contour::session::TerminalSession const& session)
{
    return std::ranges::to<std::vector>(session.scrollbarMarks()
                                        | std::views::transform([](QVariant const& mark) {
                                              return static_cast<vtbackend::ScrollbarMarkKind>(
                                                  mark.toMap().value(QStringLiteral("kind")).toInt());
                                          }));
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

/// The colour @p session publishes for ticks of @p kind.
[[nodiscard]] QColor markColor(contour::session::TerminalSession const& session,
                               vtbackend::ScrollbarMarkKind kind)
{
    return session.scrollbarMarkColors().at(static_cast<qsizetype>(kind)).value<QColor>();
}

} // namespace

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

    SECTION("a profile switch asks for one")
    {
        terminal.writeToScreen(vtbackend::test::commandRun("make", numberedOutput(3), 2));
        REQUIRE(spy.wait(1000));
        REQUIRE(pane->scrollbarMarks().size() == 1);

        REQUIRE((*pane)(contour::actions::ChangeProfile { unmarked }));
        REQUIRE(spy.wait(1000));
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
// }}}

namespace
{

/// Rewrites the file @p session's configuration names to hold @p sections and nothing else beside
/// @p session's own profile, then reloads @p session from it: the path a saved configuration file takes.
/// @param session The session to reload.
/// @param sections Top-level YAML sections, each line ending in a newline.
/// @param profileEntries Entries of the profile, each line indented by eight spaces and ending in a newline.
void reloadSessionWith(contour::session::TerminalSession& session,
                       std::string_view sections,
                       std::string_view profileEntries = {})
{
    contour::test::writeConfigFile(
        session.config().configFile,
        contour::test::profileConfigYaml(session.profileName(), sections, profileEntries));
    REQUIRE(session(contour::actions::ReloadConfig {}));
}

} // namespace

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
        CHECK(settings.gutter == contour::config::gutterSettingsFor(session->config()));
        CHECK_FALSE(settings.gutter.foldMarkers);
        CHECK(settings.gutter.exitStatus); // the block column stays, for the exit status
        CHECK(settings.contextSignalling == vtbackend::ContextSignalling::Disabled);
        CHECK(settings.contextTintScope == vtbackend::ContextTintScope::Off);
        CHECK(settings.goodImageProtocol);
    }
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

namespace
{

/// Has the application ask for @p session's host-writable status line (DECSSDT 2), denies it, then has the
/// application write @p text to it and return to the main display, as a program that keeps its status in
/// DEC's status line does.
void writeToDeniedStatusLine(contour::session::TerminalSession& session, std::string_view text)
{
    session.terminal().writeToScreen("\033[2$~");
    session.executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/false);
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
    session.executeShowHostWritableStatusLine(/*allow=*/true, /*remember=*/false);
    session.terminal().writeToScreen("\033[1$}");
    REQUIRE(session.terminal().activeStatusDisplay() == vtbackend::ActiveStatusDisplay::StatusLine);
}

} // namespace

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
        session->executeShowHostWritableStatusLine(/*allow=*/true, /*remember=*/false);
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
        session->executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/false);
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
        session->executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/false);
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
        session->executeShowHostWritableStatusLine(/*allow=*/false, /*remember=*/false);
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

namespace
{

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

namespace
{

/// Types @p keys into @p session, each one pressed and released, as a keyboard delivers them.
void typeKeys(contour::session::TerminalSession& session, std::string_view keys)
{
    for (auto const key: keys)
    {
        auto const ch = static_cast<char32_t>(key);
        auto const identity =
            vtbackend::KeyIdentity { .unshiftedKey = ch, .nativeVirtualKey = static_cast<uint32_t>(ch) };
        for (auto const eventType: { KeyboardEventType::Press, KeyboardEventType::Release })
            session.sendCharEvent(ch, identity, Modifiers {}, eventType, std::chrono::steady_clock::now());
    }
}

/// A finished block printed at the bottom of rows 1-12, so the rows above it scroll up within those margins:
/// marked rows and blocks' heads among them.
constexpr auto MarginScrollingBlock = std::string_view {
    "\033[1;12r\033[12H\r\n\033]133;A\033\\$ x\r\n\033]133;C\033\\o\r\n\033]133;D;0\033\\"
};

} // namespace

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

TEST_CASE("TerminalSession: a second onClosed() does not repeat the early-exit notice",
          "[contour][session][close]")
{
    // Regression (#2102, "the message about terminating too quickly appears twice"): ONE exit
    // reaches onClosed() twice -- from the session's own exit watcher, and from
    // TerminalDisplay::onBeforeSynchronize(), which fires it when the device is already closed by
    // the time the first frame is about to be drawn. That is the norm, not a corner case, for a
    // command that exits immediately (`contour terminal echo hello`). The de-duplication further
    // down onClosed() is below the early-exit branch's `return`, so only the armed notice itself can
    // say that this exit has already been reported.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& pty = mockPtyOf(*session);

    pty.close();
    session->onClosed();
    session->onClosed();

    CHECK(countOnScreen(*session, "Shell terminated too quickly.") == 1);
    CHECK(countOnScreen(*session, "The window will not be closed automatically.") == 1);
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
/// @param path An absolute path.
/// @return @p path as a local file URL, '/'-separated on every platform, as QUrl wants it.
[[nodiscard]] QUrl localFileUrl(std::filesystem::path const& path)
{
    return QUrl::fromLocalFile(QString::fromStdU16String(path.generic_u16string()));
}

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

    SECTION("a local file:// link opens the file its tooltip names")
    {
        // Read by the resolver the tooltip uses (vtbackend::localPathOfFileUrl): a fragment is no part of the
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
            CHECK(contour::session::hyperlinkTooltipText(
                      uri, std::string { contour::test::TestApp::HostName }, 64)
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
    session->terminal().setCurrentWorkingDirectory(
        "file://" + std::string { contour::test::TestApp::HostName } + "/tmp");

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
        session->terminal().setCurrentWorkingDirectory(
            "file://" + std::string { contour::test::TestApp::HostName } + "/home/user/proj");
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

TEST_CASE("TerminalSession: this machine is the one the app was given", "[contour][session][identity]")
{
    // The app reads the machine's identity once and hands it to every session, so a test can name the
    // machine: what counts as local then does not depend on the one the suite runs on -- a runner whose
    // host name is `localhost`, or empty, is a case like any other.
    namespace actions = contour::actions;
    auto const hostname = GENERATE(std::string { "zeta" }, std::string { "localhost" }, std::string {});
    INFO("this machine's host name: \"" << hostname << '"');
    contour::test::TestApp testApp { vtbackend::HostIdentity { .machineId = {}, .hostname = hostname } };
    auto session = makeDisplaylessSession(testApp.app());
    auto& launcher = testApp.launcher();

    SECTION("a working directory it names opens as a local path")
    {
        session->terminal().setCurrentWorkingDirectory("file://" + hostname + "/home/user/proj");
        CHECK((*session)(actions::OpenFileManager {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().isLocalFile());
        CHECK(launcher.openedUrls.back().toLocalFile() == QStringLiteral("/home/user/proj"));
    }

    SECTION("a file:// link it names opens as a local file")
    {
        seedHoveredHyperlink(*session, "file://" + hostname + "/tmp/x.log");
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.openedUrls.size() == 1);
        CHECK(launcher.openedUrls.back().isLocalFile());
        CHECK(launcher.openedUrls.back().toLocalFile() == QStringLiteral("/tmp/x.log"));
    }

    SECTION("a working directory on any other machine is not opened")
    {
        session->terminal().setCurrentWorkingDirectory("file://some-remote-host/home/user");
        CHECK((*session)(actions::OpenFileManager {}));
        CHECK(launcher.openedUrls.empty());
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
    // makeModifiers and runs identically on every platform. (On Windows the real Qt path tells a
    // Ctrl+Alt chord from AltGr by the event's text; SessionInput_test covers that.)
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

    contour::test::closeAllTabs(*controller);
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

TEST_CASE("TerminalSession: the read-only lock is the user's, which no host reset undoes",
          "[contour][session][input]")
{
    // DECSTR, KAM's reset and RIS reset the host's keyboard lock, KAM (DEC STD 070 ch. 6 §6.2); the user's
    // read-only lock is not KAM, and outlasts them.
    auto const reset = GENERATE(
        std::string_view { "\033[!p" }, std::string_view { "\033[2l" }, std::string_view { "\033c" });
    INFO("reset: " << core::escape(reset));
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();

    REQUIRE((*session)(contour::actions::ToggleInputProtection {}));
    REQUIRE_FALSE(terminal.allowInput());
    terminal.writeToScreen(reset);
    CHECK_FALSE(terminal.allowInput());

    REQUIRE((*session)(contour::actions::ToggleInputProtection {}));
    CHECK(terminal.allowInput());
}

TEST_CASE("TerminalSession: the read-only toggle leaves the host's keyboard lock alone",
          "[contour][session][input]")
{
    // KAM is the host's: the user locking and unlocking their own lock over it neither lifts nor sets it. The
    // menu's check mark shows the user's lock, and what is announced says which lock holds the keyboard.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto announcer = std::make_unique<RecordingAnnouncer>();
    auto* recorder = announcer.get();
    session->setAnnouncer(std::move(announcer));
    terminal.writeToScreen("\033[2h"); // KAM: the host locks the keyboard
    REQUIRE_FALSE(terminal.allowInput());
    CHECK_FALSE(session->contextMenuState().inputProtected);

    REQUIRE((*session)(contour::actions::ToggleInputProtection {})); // the user locks too
    CHECK_FALSE(terminal.allowInput());
    CHECK(session->contextMenuState().inputProtected);
    REQUIRE(recorder->said.size() == 1);
    CHECK(recorder->said.back().message == QStringLiteral("Read-only"));

    REQUIRE((*session)(contour::actions::ToggleInputProtection {})); // and unlocks
    CHECK_FALSE(terminal.allowInput());
    CHECK_FALSE(session->contextMenuState().inputProtected);
    REQUIRE(recorder->said.size() == 2);
    CHECK(recorder->said.back().message == QStringLiteral("Read-only: the application locked the keyboard"));

    terminal.writeToScreen("\033[2l"); // the host unlocks
    CHECK(terminal.allowInput());
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

TEST_CASE("TerminalSession tells the surface of the page on display and not of the cursor's page",
          "[contour][session][view]")
{
    // DECPCCM reset: the application writes to page 3 while page 1 stays on display. The scrollbar and the
    // pointer shape belong to the page on display, so nothing changes until page 3 comes on display.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());
    auto& surface = *held.surface;
    REQUIRE(held->getIsScrollbarVisible());
    surface.bufferChanges.clear();

    held->terminal().writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, the cursor to page 3
    CHECK(surface.bufferChanges.empty());
    CHECK(held->getIsScrollbarVisible());
    surface.cursorShapes.clear();
    held->sendFocusInEvent();
    REQUIRE_FALSE(surface.cursorShapes.empty());
    CHECK(surface.cursorShapes.back() == contour::input::MouseCursorShape::IBeam);

    surface.bufferChanges.clear();
    held->terminal().writeToScreen("\033[?64h"); // DECPCCM set: page 3 comes on display
    CHECK(surface.bufferChanges == std::vector { vtbackend::ScreenType::Alternate });
    CHECK_FALSE(held->getIsScrollbarVisible());
    surface.cursorShapes.clear();
    held->sendFocusInEvent();
    REQUIRE_FALSE(surface.cursorShapes.empty());
    CHECK(surface.cursorShapes.back() == contour::input::MouseCursorShape::Arrow);
}

TEST_CASE("TerminalSession: the wheel glides over the page on display while the cursor writes to another",
          "[contour][session][input]")
{
    // Page 1 is on display with its history while the application writes to page 3 (DECPCCM reset): a wheel
    // notch takes the smooth-scrolling path over page 1, as it does with the cursor there.
    TestApp testApp;
    auto pane = makeSessionWithSurface(testApp.app());
    REQUIRE(pane->terminal().settings().smoothScrolling);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 40))
        pane->terminal().writeToScreen("line\r\n");
    pane->terminal().writeToScreen("\033[?64l\033[3 P"); // DECPCCM reset, the cursor to page 3
    REQUIRE(pane->terminal().displayedPageIndex() == vtbackend::PageIndex(0));

    auto wheel = QWheelEvent { QPointF(10, 10), QPointF(10, 10), QPoint(0, 0),      QPoint(0, 120),
                               Qt::NoButton,    Qt::NoModifier,  Qt::NoScrollPhase, false };
    contour::session::sendWheelEvent(&wheel, *pane);

    CHECK(pane->terminal().isMomentumScrollActive());
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
        terminal.writeToScreen(vtbackend::test::commandRun(command, numberedOutput(outputLines)));
    };

    // Deep enough that folding the output actually shortens the scrollable range.
    runCommand("ls", 40);
    runCommand("pwd", 40);
    REQUIRE(terminal.foldRanges().size() == 2);

    // Output's travel is published on the GUI thread, by the refresh it asks for.
    auto published =
        QSignalSpy(held.session.get(), &contour::session::TerminalSession::historyLineCountChanged);
    REQUIRE(published.wait(1000));
    auto const expanded = held->historyLineCount();
    REQUIRE(expanded == unbox<int>(terminal.viewport().scrollableLineCount()));
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
    held->terminal().settings().autoScrollOnUpdate = true; // the auto-scroll under test; off by default

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
    held->terminal().settings().autoScrollOnUpdate = true; // the auto-scroll under test; off by default

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

namespace
{
/// Runs a failing command through @p session's terminal as a shell with OSC 133 integration would:
/// prompt on row 0, its output on row 1, and the end mark with exit code 2. Then lets the GUI thread take
/// what the finish posted to it, as it would before the pointer could reach the block.
void runFailingCommand(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen(vtbackend::test::commandRun("make", { "boom" }, 2));
    QCoreApplication::processEvents();
}

/// A hit on @p segment beside grid line @p line, the page starting at screen row 0.
[[nodiscard]] contour::session::GutterHit gutterHit(int line, vtbackend::GutterSegmentKind segment)
{
    return { .gridLine = vtbackend::LineOffset(line),
             .screenRow = vtbackend::LineOffset(line),
             .segment = segment };
}
} // namespace

TEST_CASE("TerminalSession: a press beside the line numbers is the gutter's, and does nothing",
          "[contour][session][gutter]")
{
    using contour::session::ConsumedByGutter;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    auto const& folds = session->terminal().foldState();

    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::LineNumber),
                                        vtbackend::MouseButton::Left,
                                        vtbackend::Modifiers {})
          == ConsumedByGutter::Yes);
    CHECK(session->sendOwedRelease(vtbackend::MouseButton::Left) == contour::session::PressOwner::Gutter);
    CHECK(folds.empty());                             // nothing toggled
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // and the child saw neither half

    // Every other button still belongs to the application, wherever it lands.
    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::Timestamp),
                                        vtbackend::MouseButton::Right,
                                        vtbackend::Modifiers {})
          == ConsumedByGutter::No);

    // The block column keeps today's behaviour: a press on a fold toggles it.
    CHECK(session->sendGutterPressEvent(gutterHit(0, GutterSegmentKind::BlockColumn),
                                        vtbackend::MouseButton::Left,
                                        vtbackend::Modifiers {})
          == ConsumedByGutter::Yes);
    CHECK_FALSE(folds.empty());
    CHECK(session->sendOwedRelease(vtbackend::MouseButton::Left) == contour::session::PressOwner::Gutter);
}

TEST_CASE("TerminalSession: hovering the line numbers is the gutter's, with nothing to click",
          "[contour][session][gutter]")
{
    using contour::input::MouseCursorShape;
    using contour::session::ConsumedByGutter;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);

    // The colour the block column draws the block's head in: what a hover lights up.
    auto const headColor = [&] {
        auto buffer = vtbackend::RenderBuffer {};
        session->terminal().fillRenderBuffer(buffer, true);
        REQUIRE_FALSE(buffer.gutter.empty());
        return buffer.gutter.front().attributes.foregroundColor;
    };
    auto const atRest = headColor();

    CHECK(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn))
          == ConsumedByGutter::Yes);
    REQUIRE_FALSE(session.surface->cursorShapes.empty());
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);
    CHECK(headColor() != atRest);

    // Beside the numbers nothing lights, and the fold the block column lit goes dark again.
    CHECK(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::LineNumber))
          == ConsumedByGutter::Yes);
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);
    CHECK(headColor() == atRest);

    CHECK(session->sendGutterHoverEvent(std::nullopt) == ConsumedByGutter::No);
}

TEST_CASE("TerminalSession: the terminal draws the gutter the configuration asks for, after a switch too",
          "[contour][session][gutter]")
{
    TestApp testApp;
    testApp.app().config().gutter.value().lineNumbers = vtbackend::LineNumberMode::Absolute;
    registerProfile(testApp.app(), "night", [](contour::config::TerminalProfile&) {});
    auto session = makeDisplaylessSession(testApp.app());

    CHECK(session->terminal().settings().gutter == contour::config::gutterSettingsFor(session->config()));
    CHECK(session->terminal().settings().gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);

    // A profile switch -- the path a configuration reload takes -- re-applies it over whatever was there.
    session->terminal().settings().gutter = vtbackend::GutterSettings {};
    CHECK((*session)(contour::actions::ChangeProfile { "night" }));
    CHECK(session->terminal().settings().gutter.lineNumbers == vtbackend::LineNumberMode::Absolute);
}

TEST_CASE("TerminalSession: a profile switch re-applies sticky_header to the terminal",
          "[contour][session][stickyheader]")
{
    TestApp testApp;
    auto const name = registerProfile(testApp.app(), "sticky", [](contour::config::TerminalProfile& p) {
        p.stickyHeader = contour::config::StickyHeaderConfig { .mode = vtbackend::StickyHeaderMode::Always,
                                                               .showEvicted = false };
    });
    auto session = makeDisplaylessSession(testApp.app());
    REQUIRE(session->terminal().settings().stickyHeader == vtbackend::StickyHeaderSettings {});

    // The path a configuration reload takes too: configureTerminal() re-applies the profile.
    CHECK((*session)(contour::actions::ChangeProfile { .name = name }));
    CHECK(session->terminal().settings().stickyHeader
          == vtbackend::StickyHeaderSettings { .mode = vtbackend::StickyHeaderMode::Always,
                                               .evictedPlaceholder = vtbackend::EvictedPlaceholder::Hidden });
}

namespace
{
/// The scene every sticky header input test starts from: a session with a recording surface of 8x16 cells
/// whose grid starts one cell right of the item's edge -- the pixels left of it lie in the default gutter's
/// block column -- and
/// the Qt event helpers that reach it, which a display-less session would drop. Main-page row 0 spans y 0..15
/// below the status line, if any; page column c starts at x 8 + 8c.
class StickyHeaderScene
{
  public:
    /// @param app The application, its configuration set up as the test needs.
    explicit StickyHeaderScene(contour::ContourGuiApp& app): _held { makeSessionWithSurface(app) }
    {
        _held.surface->gridMetricsValue.cellSize = _held.surface->cellSizeValue;
        _held.surface->gridMetricsValue.pageMargin.left = 8;
    }

    [[nodiscard]] contour::session::TerminalSession& session() const noexcept { return *_held; }
    [[nodiscard]] contour::test::FakeDisplaySurface& surface() const noexcept { return *_held.surface; }
    [[nodiscard]] vtbackend::Terminal& terminal() const noexcept { return _held->terminal(); }

    /// @return What reached the PTY: the application's input.
    [[nodiscard]] std::string& written() const { return mockPtyOf(*_held).stdinBuffer(); }

    /// Turns on normal (1000) and SGR (1006) mouse reporting: a press the grid takes is written to the PTY.
    void reportMouse() const { terminal().writeToScreen("\033[?1000h\033[?1006h"); }

    /// A point in the gutter's block column beside page row @p row: the 8 pixels left of the page, one 8x16
    /// cell per row.
    /// @param row The page row.
    [[nodiscard]] static QPointF besideRow(int row) { return { 4.0, (row * 16.0) + 4.0 }; }

    /// Runs `make` through the shell integration, printing @p output.
    /// @param output The lines it prints.
    void runCommand(std::vector<std::string> const& output) const
    {
        terminal().writeToScreen(vtbackend::test::commandRun("make", output));
    }

    /// runCommand(), then scrolls 5 lines back into what it printed -- where the default `scrolled` mode
    /// shows the header -- and draws the frame a press on row 0 is measured against.
    /// @param output The lines it prints.
    void scrollBackIntoCommand(std::vector<std::string> const& output) const
    {
        runCommand(output);
        REQUIRE(terminal().viewport().scrollUp(vtbackend::LineCount(5)));
        REQUIRE(vtbackend::test::drawFrame(terminal()).stickyHeader.has_value());
    }

    /// runCommand() under the `always` mode, with the view left following the output, and draws the frame:
    /// the header then covers a row of the live page rather than one of the history.
    /// @param output The lines it prints.
    void followIntoCommand(std::vector<std::string> const& output) const
    {
        terminal().settings().stickyHeader.mode = vtbackend::StickyHeaderMode::Always;
        runCommand(output);
        REQUIRE_FALSE(terminal().viewport().scrolled());
        REQUIRE(vtbackend::test::drawFrame(terminal()).stickyHeader.has_value());
    }

    /// Presses @p button at @p position, @p held being every button down once it is.
    void press(QPointF position, Qt::MouseButton button, Qt::MouseButtons held) const
    {
        auto event = QMouseEvent(QEvent::MouseButtonPress, position, position, button, held, Qt::NoModifier);
        contour::session::sendMousePressEvent(&event, session());
    }

    /// Releases @p button at @p position, @p held being every button still down.
    void release(QPointF position, Qt::MouseButton button, Qt::MouseButtons held) const
    {
        auto event =
            QMouseEvent(QEvent::MouseButtonRelease, position, position, button, held, Qt::NoModifier);
        contour::session::sendMouseReleaseEvent(&event, session());
    }

    /// A left click at @p position: its press and its release.
    void click(QPointF position) const
    {
        press(position, Qt::LeftButton, Qt::LeftButton);
        release(position, Qt::LeftButton, Qt::NoButton);
    }

    /// Moves the pointer to @p position with no button down, as the item's hover tracking reports it.
    void hover(QPointF position) const
    {
        auto event = QHoverEvent(QEvent::HoverMove, position, position, position);
        contour::session::sendMouseMoveEvent(&event, session());
    }

    /// Moves the pointer to @p position as a mouse-move event reports it, no button down.
    void move(QPointF position) const
    {
        auto event =
            QMouseEvent(QEvent::MouseMove, position, position, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        contour::session::sendMouseMoveEvent(&event, session());
    }

    /// Drags the pointer to @p position with the left button down, as the item that took the press hears it.
    void drag(QPointF position) const
    {
        auto event =
            QMouseEvent(QEvent::MouseMove, position, position, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        contour::session::sendMouseMoveEvent(&event, session());
    }

  private:
    SessionWithSurface _held;
};
} // namespace

TEST_CASE("TerminalSession: a press on the sticky header jumps to its command",
          "[contour][session][input][stickyheader]")
{
    using contour::session::ConsumedByStickyHeader;
    using contour::session::PressOwner;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    scene.scrollBackIntoCommand(numberedOutput(60));
    auto& session = scene.session();
    auto& terminal = scene.terminal();

    SECTION("a left press takes the viewport to the command, and its release is the header's too")
    {
        CHECK(session.sendStickyHeaderPressEvent(
                  vtbackend::LineOffset(0), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
              == ConsumedByStickyHeader::Yes);
        CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));
        // And the row it landed on flashes, so the eye finds it.
        auto const flash = vtbackend::test::drawFrame(terminal).lineFlash;
        REQUIRE(flash.has_value());
        CHECK(flash->lineOffset == vtbackend::LineOffset(0));

        CHECK(session.sendOwedRelease(vtbackend::MouseButton::Left) == PressOwner::StickyHeader);
        CHECK(session.sendOwedRelease(vtbackend::MouseButton::Left) == PressOwner::None);
    }

    SECTION("another button is consumed -- it would act on the hidden row -- but does not jump")
    {
        auto const before = terminal.viewport().scrollOffset();
        CHECK(session.sendStickyHeaderPressEvent(
                  vtbackend::LineOffset(0), vtbackend::MouseButton::Right, vtbackend::Modifiers {})
              == ConsumedByStickyHeader::Yes);
        CHECK(terminal.viewport().scrollOffset() == before);

        // Owed for the button pressed: another one's release, half of a chord, is the grid's.
        CHECK(session.sendOwedRelease(vtbackend::MouseButton::Left) == PressOwner::None);
        CHECK(session.sendOwedRelease(vtbackend::MouseButton::Right) == PressOwner::StickyHeader);
    }

    SECTION("a press below the header, or off the page, belongs to the grid")
    {
        CHECK(session.sendStickyHeaderPressEvent(
                  vtbackend::LineOffset(1), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
              == ConsumedByStickyHeader::No);
        CHECK(session.sendStickyHeaderPressEvent(
                  std::nullopt, vtbackend::MouseButton::Left, vtbackend::Modifiers {})
              == ConsumedByStickyHeader::No);
        CHECK(session.sendOwedRelease(vtbackend::MouseButton::Left) == PressOwner::None);
    }
}

TEST_CASE("TerminalSession: without shell integration row 0 belongs to the grid",
          "[contour][session][input][stickyheader]")
{
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    for (auto const i: std::views::iota(0, 60))
        terminal.writeToScreen(std::format("plain {}\r\n", i));

    REQUIRE(terminal.viewport().scrollUp(vtbackend::LineCount(5)));
    REQUIRE_FALSE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());

    CHECK(scene.session().sendStickyHeaderPressEvent(
              vtbackend::LineOffset(0), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
          == contour::session::ConsumedByStickyHeader::No);
}

TEST_CASE("TerminalSession: a press on the evicted placeholder is the header's, and stays put",
          "[contour][session][input][stickyheader]")
{
    // A ten-line scrollback: sixty lines of output push the command's own row out of it, and the record that
    // outlives the row leaves the placeholder at the very top of what is left.
    TestApp testApp;
    auto& profile = *testApp.app().config().profile(testApp.app().profileName());
    auto history = profile.history.value();
    history.maxHistoryLineCount = vtbackend::LineCount(10);
    profile.history = history;

    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    scene.runCommand(numberedOutput(60));

    REQUIRE(terminal.viewport().scrollToTop());
    auto const header = vtbackend::test::drawFrame(terminal).stickyHeader;
    REQUIRE(header.has_value());
    REQUIRE(header->kind == vtbackend::StickyHeaderKind::Evicted);

    // There is nowhere to jump to ...
    auto const before = terminal.viewport().scrollOffset();
    auto const jump = terminal.scrollToCommandBlockHead(header->block);
    REQUIRE_FALSE(jump.has_value());
    CHECK(jump.error() == vtbackend::CommandBlockJumpError::HeadEvicted);

    // ... and the press is the placeholder's all the same: it covers row 0 as a header does.
    CHECK(scene.session().sendStickyHeaderPressEvent(
              vtbackend::LineOffset(0), vtbackend::MouseButton::Left, vtbackend::Modifiers {})
          == contour::session::ConsumedByStickyHeader::Yes);
    CHECK(terminal.viewport().scrollOffset() == before);
    CHECK(scene.session().sendOwedRelease(vtbackend::MouseButton::Left)
          == contour::session::PressOwner::StickyHeader);
}

TEST_CASE("TerminalSession: a Qt press on row 0 reaches the sticky header where it is drawn",
          "[contour][session][input][stickyheader]")
{
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    REQUIRE(terminal.mainPageTopRow() == vtbackend::LineOffset(0));

    scene.reportMouse();
    scene.scrollBackIntoCommand(numberedOutput(60));
    scene.written().clear();

    SECTION("pinned: smooth scrolling moves the rows beneath it, not the header")
    {
        // Half a cell of smooth scroll draws the page's rows 8 pixels lower and leaves the header where it
        // was, so a press 4 pixels down lands on the header, above where the shifted row 0 begins.
        terminal.viewport().setPixelOffset(8.0F);
        scene.click(QPointF(100, 4));
        CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));
        CHECK(scene.written().empty());
    }

    SECTION("the gutter is asked first: beside row 0 a press toggles the fold")
    {
        scene.click(QPointF(4, 4));
        CHECK_FALSE(terminal.foldState().empty());
        CHECK(scene.written().empty());
    }

    SECTION("mid-animation the fold column beside the pinned header still toggles the fold")
    {
        // Above where the shifted row 0 begins, beside the header: the gutter's row 0 all the same.
        terminal.viewport().setPixelOffset(8.0F);
        scene.click(QPointF(4, 4));
        CHECK_FALSE(terminal.foldState().empty()); // the header's jump would have collapsed nothing
        CHECK(scene.written().empty());
    }

    SECTION("the header spans the page's columns only: a press in the margin beside them is not the header's")
    {
        auto const pageWidth = unbox<int>(terminal.pageSize().columns) * 8;
        auto const topLine = terminal.viewport().topLine();
        scene.click(QPointF(8 + pageWidth + 2, 4));
        CHECK(terminal.viewport().topLine() == topLine);
    }
}

TEST_CASE("TerminalSession: the gutter's block column swallows its presses, and toggles only what it draws",
          "[contour][session][input][gutter]")
{
    // The scene's gutter is the 8 pixels left of the page: the block column, one 8x16 cell per row.
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    scene.reportMouse();
    terminal.writeToScreen(vtbackend::test::commandRun("true", {}));         // row 0: ●, nothing to fold
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "a" }, 2)); // rows 1-2: a fold
    scene.written().clear();

    auto const beside = &StickyHeaderScene::besideRow;

    SECTION("on a fold control, the fold")
    {
        scene.hover(beside(1));
        CHECK(scene.surface().cursorShapes.back() == MouseCursorShape::PointingHand);
        scene.click(beside(1));
        CHECK_FALSE(terminal.foldState().empty());
    }

    SECTION("beside a block with nothing to fold, nothing")
    {
        scene.click(beside(0));
    }

    SECTION("below the last block, nothing")
    {
        scene.click(beside(10));
    }

    SECTION("with folding off, no hand and nothing")
    {
        terminal.settings().foldingAvailability = vtbackend::FoldingAvailability::Disabled;
        scene.hover(beside(1));
        CHECK(scene.surface().cursorShapes.back() != MouseCursorShape::PointingHand);
        scene.click(beside(1));
        CHECK(terminal.foldState().empty());
    }

    SECTION("with the fold controls hidden, no hand and nothing beside the output")
    {
        // The head shows its status mark then, which offers the block's menu; the output rows show nothing.
        terminal.settings().gutter.foldMarkers = false;
        scene.hover(beside(2));
        CHECK(scene.surface().cursorShapes.back() != MouseCursorShape::PointingHand);
        scene.click(beside(2));
        CHECK(terminal.foldState().empty());
    }

    // Let through, the press reached the application as one on column 0 of the row, and began a selection.
    CHECK(scene.written().empty());
    CHECK_FALSE(terminal.leftMouseButtonPressed());
    CHECK(scene.session().pressOwner() == contour::session::PressOwner::None);
}

TEST_CASE("TerminalSession: a selection dragged from the grid into the gutter keeps extending",
          "[contour][session][input][gutter]")
{
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    terminal.writeToScreen("line 0\r\nline 1\r\nline 2\r\n");

    // From row 2's fourth column, out to the left over the gutter beside row 0: clamped onto column 0 of row
    // 0, as a drag past the page's right edge is clamped onto its last column.
    scene.press(QPointF(8 + (3 * 8) + 4, (2 * 16) + 4), Qt::LeftButton, Qt::LeftButton);
    scene.drag(QPointF(4, 4));
    scene.release(QPointF(4, 4), Qt::LeftButton, Qt::NoButton);

    REQUIRE(terminal.selectionAvailable());
    auto const text = terminal.extractSelectionText();
    CHECK(text.starts_with("line 0"));
    CHECK(text.contains("line 1"));
}

TEST_CASE(
    "TerminalSession: a right press in the gutter opens the menu of the block beside it, its fold row too",
    "[contour][session][input][gutter]")
{
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "a", "b" })); // rows 0-2
    terminal.writeToScreen(vtbackend::test::commandRun("test", { "c", "d" })); // rows 3-5

    // The menu as it opens: built from the session's state at that moment.
    auto state = std::optional<contour::command::ContextMenuState> {};
    QObject::connect(&testApp.app().sessionsManager(),
                     &contour::session::TerminalSessionManager::contextMenuRequested,
                     [&](contour::session::TerminalSession* acting) { state = acting->contextMenuState(); });

    // The pointer crosses the grid over the first block, then the gutter to the second, and presses there.
    scene.hover(QPointF(40, (1 * 16) + 4));
    scene.hover(QPointF(4, (4 * 16) + 4));
    scene.press(QPointF(4, (4 * 16) + 4), Qt::RightButton, Qt::RightButton);

    REQUIRE(state.has_value());
    REQUIRE(state->blockUnderCursor.has_value());
    CHECK(state->blockUnderCursor->id == terminal.commandBlockAt(vtbackend::LineOffset(4))->id);
    REQUIRE(state->foldLine.has_value());
    CHECK(terminal.foldContaining(*state->foldLine) == terminal.foldContaining(vtbackend::LineOffset(4)));
}

TEST_CASE(
    "TerminalSession: a click on a status mark, or a right-click on the block column, opens its block's menu",
    "[contour][session][input][gutter]")
{
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    scene.reportMouse();
    terminal.writeToScreen(vtbackend::test::commandRun("true", {}));              // row 0: a disc
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "a", "b" }, 2)); // rows 1-3: a fold
    scene.written().clear();

    auto menuBlock = std::optional<vtbackend::CommandBlockId> {};
    auto menuRequests = 0;
    QObject::connect(&testApp.app().sessionsManager(),
                     &contour::session::TerminalSessionManager::contextMenuRequested,
                     [&](contour::session::TerminalSession* acting) {
                         ++menuRequests;
                         if (auto const pointed = acting->contextMenuState().blockUnderCursor)
                             menuBlock = pointed->id;
                     });
    auto const beside = &StickyHeaderScene::besideRow;
    auto const blockAt = [&](int row) {
        auto const* const record = terminal.commandBlockAt(vtbackend::LineOffset(row));
        REQUIRE(record != nullptr);
        return record->id;
    };

    SECTION("a left click on the disc: the block has nothing to fold, so its menu")
    {
        scene.hover(beside(0));
        CHECK(scene.surface().cursorShapes.back() == contour::input::MouseCursorShape::PointingHand);
        scene.click(beside(0));
        CHECK(menuRequests == 1);
        CHECK(menuBlock == blockAt(0));
        CHECK(terminal.foldState().empty());
    }

    SECTION("a right click anywhere on a block's run: that block's menu, and the application hears nothing")
    {
        scene.press(beside(2), Qt::RightButton, Qt::RightButton);
        scene.release(beside(2), Qt::RightButton, Qt::NoButton);
        CHECK(menuRequests == 1);
        CHECK(menuBlock == blockAt(1));
        CHECK(scene.written().empty());
    }

    SECTION("a right click beside no block is the application's, as it always was")
    {
        scene.press(beside(10), Qt::RightButton, Qt::RightButton);
        scene.release(beside(10), Qt::RightButton, Qt::NoButton);
        CHECK(menuRequests == 0);
        CHECK_FALSE(scene.written().empty());
    }

    CHECK(scene.session().pressOwner() == contour::session::PressOwner::None);
}

TEST_CASE("TerminalSession: a click on a collapsed fold's label expands it",
          "[contour][session][input][gutter]")
{
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    scene.reportMouse();
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "a", "b" })); // "$ make" on row 0
    REQUIRE(terminal.toggleFoldContaining(vtbackend::LineOffset(0)));
    scene.written().clear();

    // Page column c starts at x 8 + 8c: the label starts at column 7, one after the six of "$ make".
    auto const onLabel = QPointF(8 + (8 * 8) + 4, 4);

    scene.hover(onLabel);
    CHECK(scene.surface().cursorShapes.back() == MouseCursorShape::PointingHand);

    scene.click(onLabel);
    CHECK(terminal.foldState().empty());
    CHECK(scene.written().empty()); // neither half reached the application
    CHECK(scene.session().pressOwner() == contour::session::PressOwner::None);

    // Expanded, the row has no label: the next press there is the grid's.
    scene.click(onLabel);
    CHECK_FALSE(scene.written().empty());
}

TEST_CASE("TerminalSession: RerunCommand types the command at the prompt once it is allowed",
          "[contour][session][semanticblocks]")
{
    using contour::actions::RerunCommand;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("make", { "a" }));
    terminal.writeToScreen(vtbackend::test::PromptStart); // the next prompt, at which the shell waits
    auto& written = mockPtyOf(*session).stdinBuffer();
    written.clear();
    auto asked = QSignalSpy(session.session.get(),
                            &contour::session::TerminalSession::requestPermissionForRerunCommand);

    SECTION("asked first, shown what it runs, and run once allowed")
    {
        CHECK((*session)(RerunCommand {}));
        CHECK(asked.count() == 1);
        CHECK(session->pendingRerunCommandLine() == "make");
        CHECK(written.empty());

        session->applyPendingRerun(true, false);
        CHECK(written == "make\r");
        CHECK(session->pendingRerunCommandLine().isEmpty());
    }

    SECTION("refused, nothing is typed")
    {
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(false, false);
        CHECK(written.empty());
    }

    SECTION("allowed for the session, the next one runs without asking")
    {
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(true, true);
        written.clear();
        CHECK((*session)(RerunCommand {}));
        CHECK(asked.count() == 1);
        CHECK(written == "make\r");
    }

    SECTION("through a bracketed paste where the shell asked for one")
    {
        terminal.writeToScreen("\033[?2004h");
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(true, false);
        CHECK(written == "\033[200~make\033[201~\r");
    }

    SECTION("not while a command runs: typed, it would be that command's input")
    {
        CHECK(session->contextMenuState().livePrompt == contour::command::PromptPresence::Present);
        terminal.writeToScreen(vtbackend::test::commandStarted("sleep 9"));
        CHECK(session->contextMenuState().livePrompt == contour::command::PromptPresence::Absent);
        CHECK_FALSE((*session)(RerunCommand {}));
        CHECK(asked.count() == 0);
    }

    SECTION("the prompt gone by the time the answer comes, nothing is typed")
    {
        REQUIRE((*session)(RerunCommand {}));
        terminal.writeToScreen("sleep 9\r\n\033]133;C\033\\");
        session->applyPendingRerun(true, false);
        CHECK(written.empty());
    }

    SECTION("a command line that cannot be typed as it is is not run at all")
    {
        // Reported with a line feed in it: typed raw, without a bracketed paste, the shell would run the part
        // before it as a command of its own.
        terminal.writeToScreen(std::string { vtbackend::test::PromptStart } + "\033]133;B\033\\\r\n"
                               + vtbackend::test::reportedOutputStart("echo%20a%0Aecho%20b")
                               + vtbackend::test::commandEnd(0)
                               + std::string { vtbackend::test::PromptStart });
        CHECK_FALSE((*session)(RerunCommand {}));
        CHECK(asked.count() == 0);
    }
}

TEST_CASE("TerminalSession: with a status line on top the sticky header is pressed a row lower",
          "[contour][session][input][stickyheader]")
{
    // The header covers the main page's row 0, which a status line above the grid puts on the screen's row 1:
    // the band is drawn there, and a press there is the header's.
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    terminal.settings().statusDisplayPosition = vtbackend::StatusDisplayPosition::Top;
    terminal.setStatusDisplay(vtbackend::StatusDisplayType::Indicator);
    REQUIRE(terminal.mainPageTopRow() == vtbackend::LineOffset(1));

    scene.scrollBackIntoCommand(numberedOutput(60));
    auto const topLine = terminal.viewport().topLine();

    SECTION("on the screen's row 1 it jumps to the command")
    {
        scene.click(QPointF(100, 20));
        CHECK(terminal.primaryScreen().grid().lineText(terminal.viewport().topLine()).starts_with("$ make"));
    }

    SECTION("on the status line above it is not the header's")
    {
        scene.click(QPointF(100, 4));
        CHECK(terminal.viewport().topLine() == topLine);
    }
}

TEST_CASE("TerminalSession: pointer motion over the sticky header acts on the header only",
          "[contour][session][input][stickyheader]")
{
    // The row the header covers is still on the page underneath, and every row here is a link -- that one
    // too.
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    auto& session = scene.session();
    auto& terminal = scene.terminal();
    scene.scrollBackIntoCommand(linkedOutput(60));

    auto const overHeader = QPointF(40, 4);   // column 4 of row 0
    auto const belowHeader = QPointF(40, 20); // column 4 of row 1

    scene.hover(belowHeader);
    REQUIRE_FALSE(session.hyperlinkTooltipText().isEmpty());
    REQUIRE(terminal.isMouseHoveringHyperlink());

    SECTION("the link the pointer came from ends, and the one beneath the header is never hovered")
    {
        scene.hover(overHeader);
        CHECK(session.hyperlinkTooltipText().isEmpty());
        CHECK_FALSE(terminal.isMouseHoveringHyperlink());
    }

    SECTION("the arrow shows: nothing on the header is text to select or a link to follow")
    {
        scene.hover(overHeader);
        REQUIRE_FALSE(scene.surface().cursorShapes.empty());
        CHECK(scene.surface().cursorShapes.back() == MouseCursorShape::Arrow);
    }

    SECTION("leaving the band gives the row below its link and its shape back")
    {
        scene.hover(overHeader);

        // Back onto the very cell it left from.
        scene.hover(belowHeader);
        CHECK_FALSE(session.hyperlinkTooltipText().isEmpty());
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK(scene.surface().cursorShapes.back() == MouseCursorShape::PointingHand);
    }

    SECTION("without a header row 0 is the grid's")
    {
        REQUIRE(terminal.viewport().scrollToBottom());
        REQUIRE_FALSE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());
        scene.hover(overHeader);
        CHECK_FALSE(session.hyperlinkTooltipText().isEmpty());
        CHECK(terminal.isMouseHoveringHyperlink());
    }
}

TEST_CASE("TerminalSession: an application tracking motion hears none of it over the sticky header",
          "[contour][session][input][stickyheader]")
{
    // Over rows of the live page, the only rows a mouse report can name: the history has no coordinates to
    // report (InputGenerator::mouseTransport refuses them), so beside a view scrolled back there is nothing
    // to hear with or without the header.
    TestApp testApp;
    auto const scene = StickyHeaderScene { testApp.app() };
    scene.followIntoCommand(linkedOutput(60));
    scene.terminal().writeToScreen("\033[?1003h\033[?1006h"); // any-event tracking (1003), SGR (1006)

    auto const overHeader = QPointF(40, 4);   // column 4 of row 0
    auto const belowHeader = QPointF(40, 20); // column 4 of row 1

    SECTION("none of it over the header")
    {
        scene.written().clear();
        scene.hover(overHeader);
        scene.move(overHeader + QPointF(8, 0));
        CHECK(scene.written().empty());
    }

    SECTION("leaving the band gives the row below its reports back")
    {
        scene.hover(overHeader);
        scene.written().clear();
        scene.hover(belowHeader);
        CHECK_FALSE(scene.written().empty());
    }
}

TEST_CASE("TerminalSession: a pointer that leaves the grid stops hovering its link",
          "[contour][session][hyperlink]")
{
    // The terminal draws the link under the pointer underlined for as long as it takes the pointer to be on
    // it. Over the gutter, or outside the terminal, the pointer is on no cell at all.
    using contour::input::MouseCursorShape;
    using contour::session::ConsumedByGutter;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto& terminal = session->terminal();
    terminal.writeToScreen("\033]8;;https://example.com/\033\\link\033]8;;\033\\");
    auto const onLink =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) };
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, onLink, vtbackend::PixelCoordinate {});
    REQUIRE(terminal.isMouseHoveringHyperlink());
    REQUIRE_FALSE(session->hyperlinkTooltipText().isEmpty());

    SECTION("into the gutter")
    {
        CHECK(session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::LineNumber))
              == ConsumedByGutter::Yes);
        CHECK_FALSE(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.tryGetHoveringHyperlink() == nullptr); // what the frame underlines
        CHECK(session->hyperlinkTooltipText().isEmpty());
    }

    SECTION("out of the window")
    {
        session->onPointerLeft();
        CHECK_FALSE(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.tryGetHoveringHyperlink() == nullptr);
        CHECK(session->hyperlinkTooltipText().isEmpty());
    }

    SECTION("back onto the cell it left from: the link is hovered again, tooltip and all")
    {
        session->onPointerLeft();
        session->sendMouseMoveEvent(vtbackend::Modifiers {}, onLink, vtbackend::PixelCoordinate {});
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK_FALSE(session->hyperlinkTooltipText().isEmpty());
        CHECK(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);
    }
}

TEST_CASE("TerminalSession: each button on the sticky header acts for the header's block",
          "[contour][session][input][stickyheader]")
{
    // Normal (1000) and SGR (1006) mouse tracking are on, so a press or a release the grid takes is written
    // to the PTY, where it shows. The middle button is bound to paste the clipboard, which a test can fill:
    // the default binding pastes the selection clipboard, which this platform may not have.
    TestApp testApp;
    auto mappings = testApp.app().config().inputMappings.value();
    mappings.mouseMappings.insert(mappings.mouseMappings.begin(),
                                  contour::config::MouseInputMapping {
                                      .modes { vtbackend::MatchModes {} },
                                      .modifiers { vtbackend::Modifiers { vtbackend::Modifier::None } },
                                      .input = vtbackend::MouseButton::Middle,
                                      .binding = { { contour::actions::PasteClipboard {} } } });
    testApp.app().config().inputMappings = std::move(mappings);

    auto const scene = StickyHeaderScene { testApp.app() };
    auto& terminal = scene.terminal();
    scene.reportMouse();
    scene.scrollBackIntoCommand(numberedOutput(60));
    auto const block = terminal.stickyHeaderBlock();
    REQUIRE(block.has_value());
    auto& written = scene.written();
    written.clear();

    auto const onHeader = QPointF(40, 4); // row 0
    auto const onGrid = QPointF(40, 36);  // row 2

    SECTION("middle pastes, as it does anywhere, and the hidden row hears of neither half")
    {
        QGuiApplication::clipboard()->setText(QStringLiteral("pasted"));
        scene.press(onHeader, Qt::MiddleButton, Qt::MiddleButton);
        scene.release(onHeader, Qt::MiddleButton, Qt::NoButton);
        CHECK(written == "pasted");
    }

    SECTION("right opens the context menu of the header's block, and the hidden row hears of neither half")
    {
        auto menuBlock = std::optional<vtbackend::CommandBlockId> {};
        auto menuRequests = 0;
        QObject::connect(&testApp.app().sessionsManager(),
                         &contour::session::TerminalSessionManager::contextMenuRequested,
                         [&](contour::session::TerminalSession* acting) {
                             ++menuRequests;
                             if (auto const pointed = acting->contextMenuState().blockUnderCursor)
                                 menuBlock = pointed->id;
                         });
        // The pointer comes to the header the way a user's does, which takes it off the grid's cells: the
        // menu cannot find the block under it there.
        scene.hover(onHeader);
        scene.press(onHeader, Qt::RightButton, Qt::RightButton);
        scene.release(onHeader, Qt::RightButton, Qt::NoButton);
        CHECK(menuRequests == 1);
        CHECK(menuBlock == block);
        CHECK(written.empty());
    }

    SECTION("a right press whose release the menu took leaves the next click on the grid whole")
    {
        terminal.writeToScreen("\033[?1000l\033[?1006l"); // the grid's own selection, not the application
        scene.press(onHeader, Qt::RightButton, Qt::RightButton);
        scene.press(onGrid, Qt::LeftButton, Qt::LeftButton);
        REQUIRE(terminal.leftMouseButtonPressed());
        scene.release(onGrid, Qt::LeftButton, Qt::NoButton);
        CHECK_FALSE(terminal.leftMouseButtonPressed());
    }

    SECTION("a grab that ends without the release owes the header nothing more")
    {
        // The menu a right press opens takes the release, and the display hears its grab end instead.
        scene.press(onHeader, Qt::RightButton, Qt::RightButton);
        scene.session().dropOwedRelease();
        scene.press(onGrid, Qt::RightButton, Qt::RightButton);
        scene.release(onGrid, Qt::RightButton, Qt::NoButton);
        CHECK(written == "\033[<2;5;3M\033[<2;5;3m");
    }

    SECTION("a chord: the grid's button is reported whole, the header's not at all")
    {
        scene.press(onHeader, Qt::LeftButton, Qt::LeftButton);
        scene.press(onGrid, Qt::RightButton, Qt::LeftButton | Qt::RightButton);
        scene.release(onGrid, Qt::RightButton, Qt::LeftButton);
        scene.release(onHeader, Qt::LeftButton, Qt::NoButton);
        CHECK(written == "\033[<2;5;3M\033[<2;5;3m");
    }
}

TEST_CASE("TerminalSession: a reload that moves the grid refits the page, once", "[contour][session][gutter]")
{
    // The page is fitted around the gutter and the margins, and neither is a size a resize event reports:
    // a reload that changes either has to refit the page itself, or the grid stays where the old frame put
    // it -- drawn over the new gutter's numbers, or a blank strip away from them. The window's size hints
    // reserve the same frame, so the refit is the one that refreshes them too, or the window could still
    // shrink to the old minimum.
    contour::test::ConfiguredTestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const& refits = session.surface->pageRefits;
    auto const refitsBefore = refits;

    constexpr auto WiderGutter = std::string_view { "gutter:\n    line_numbers: absolute\n" };
    constexpr auto OtherMargins =
        std::string_view { "        margins:\n            horizontal: 12\n            vertical: 4\n" };

    SECTION("a reload that changes neither keeps the page")
    {
        reloadSessionWith(*session, {});
        CHECK(refits == refitsBefore);
    }

    SECTION("a wider gutter")
    {
        reloadSessionWith(*session, WiderGutter);
        CHECK(refits == refitsBefore + 1);
    }

    SECTION("other margins")
    {
        reloadSessionWith(*session, {}, OtherMargins);
        CHECK(refits == refitsBefore + 1);
    }

    SECTION("both at once, refitted once")
    {
        reloadSessionWith(*session, WiderGutter, OtherMargins);
        CHECK(refits == refitsBefore + 1);
    }
}

TEST_CASE("TerminalSession: hovering a block's gutter row describes the block", "[contour][session][gutter]")
{
    using contour::session::ConsumedByGutter;
    using contour::session::TerminalSession;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    REQUIRE(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn))
            == ConsumedByGutter::Yes);
    CHECK(spy.count() == 1);
    auto const text = session->commandBlockTooltip().toStdString();
    CHECK(text.starts_with("Exit code 2\n"));
    CHECK(text.contains("$ make"));
    // And what a click does there: its output folds.
    CHECK(text.ends_with("\nClick to collapse · Right-click for actions"));
    CHECK(session->commandBlockTooltipAnchor().width() > 0);

    // Collapsed, the click expands it.
    REQUIRE(session->terminal().toggleFoldContaining(vtbackend::LineOffset(0)));
    CHECK(session->commandBlockTooltip().toStdString().ends_with(
        "\nClick to expand · Right-click for actions"));
    REQUIRE(session->terminal().toggleFoldContaining(vtbackend::LineOffset(0)));

    // Another row of the same block: the tooltip stays where the block was entered.
    session->sendGutterHoverEvent(gutterHit(1, GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 1);

    // The line numbers beside it are not the block column.
    session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::LineNumber));
    CHECK(spy.count() == 2);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());

    // Back on the block, then off the gutter altogether.
    session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 3);
    session->sendGutterHoverEvent(std::nullopt);
    CHECK(spy.count() == 4);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: a scroll withdraws the block tooltip", "[contour][session][gutter]")
{
    // The hovered block is found on pointer MOVEMENT, so a scroll slides another block under a stationary
    // pointer with nothing noticing; the tooltip goes, as the hyperlink's does.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE_FALSE(session->commandBlockTooltip().isEmpty());
    QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    session->onScrollOffsetChanged(vtbackend::ScrollOffset(1));
    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: without shell integration the block column shows no tooltip",
          "[contour][session][gutter]")
{
    // A row no OSC 133 mark claimed belongs to no block, so there is nothing to describe.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("plain output\r\n");
    QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 0);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: over the alternate screen the block column shows no tooltip",
          "[contour][session][gutter]")
{
    // A full-screen application's page has no command blocks, and the gutter beside it stays empty; the
    // primary screen's block at the same row is not what the pointer is beside.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    session->terminal().writeToScreen("\033[?1049h");
    REQUIRE(session->terminal().isAlternateScreen());
    QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 0);
    CHECK(session->commandBlockTooltip().isEmpty());
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: a scroll on the parser thread leaves the block tooltip to the GUI thread",
          "[contour][session][gutter][threading]")
{
    // Output that scrolls the page while the viewport is scrolled back moves the viewport along with it,
    // and the terminal reports that move from the thread writing the output. The tooltip's state belongs
    // to the GUI thread, where the hover writes it and QML reads it: the parser thread must not touch it,
    // only have the GUI thread withdraw it.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(std::string(30, '\n') + "\033[1;1H"); // history above, cursor home
    runFailingCommand(*session);
    REQUIRE(session->terminal().viewport().scrollUp(vtbackend::LineCount(1)));
    QCoreApplication::processEvents();
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE_FALSE(session->commandBlockTooltipAnchor().isEmpty());
    QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

    // One more line at the bottom of the page scrolls it, and the scrolled-back viewport follows.
    std::thread { [&] { session->terminal().writeToScreen("\033[24;1H\n"); } }.join();
    CHECK(spy.count() == 0);
    CHECK_FALSE(session->commandBlockTooltipAnchor().isEmpty());

    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    CHECK(session->commandBlockTooltipAnchor().isEmpty());
}

TEST_CASE("TerminalSession: a scroll on the parser thread leaves the hyperlink tooltip to the GUI thread",
          "[contour][session][hyperlink][threading]")
{
    // The block tooltip's case, for the hyperlink's: output that scrolls a scrolled-back viewport reports the
    // move from the thread writing the output, and the tooltip's state belongs to the GUI thread, where the
    // pointer's move writes it and QML reads it.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    // History above, and a link on the page's top row.
    session->terminal().writeToScreen(std::string(30, '\n')
                                      + "\033[1;1H\033]8;;https://example.com/\033\\link\033]8;;\033\\");
    REQUIRE(session->terminal().viewport().scrollUp(vtbackend::LineCount(1)));
    QCoreApplication::processEvents();
    // Scrolled back one line, the page's top row is the viewport's second.
    session->sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(1), .column = vtbackend::ColumnOffset(1) },
        vtbackend::PixelCoordinate {});
    REQUIRE_FALSE(session->hyperlinkTooltipText().isEmpty());
    QSignalSpy const spy(session.session.get(), &TerminalSession::hyperlinkHoverChanged);

    // One more line at the bottom of the page scrolls it, and the scrolled-back viewport follows.
    std::thread { [&] { session->terminal().writeToScreen("\033[24;1H\n"); } }.join();
    CHECK(spy.count() == 0);
    CHECK_FALSE(session->hyperlinkTooltipText().isEmpty());

    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    CHECK(session->hyperlinkTooltipText().isEmpty());
}

TEST_CASE("TerminalSession: a path that names no file shows no hand, Ctrl or not",
          "[contour][session][hyperlink][links]")
{
    // The move asks the index, which holds only what a probe confirmed, never the filesystem itself.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(std::format("open {} now", inWorkDirectory("notes.txt")));
    testApp.detectLinks(*session);
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");

    session->sendMouseMoveEvent(
        vtbackend::Modifiers { vtbackend::Modifier::Control },
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(7) },
        vtbackend::PixelCoordinate {});
    REQUIRE_FALSE(session.surface->cursorShapes.empty());
    CHECK(session.surface->cursorShapes.back() == contour::input::MouseCursorShape::IBeam);
}

#ifndef LIBTERMINAL_PASSIVE_RENDER_BUFFER_UPDATE
// A passive build fills on the parser thread, where detection is compiled out (Terminal::fillRenderFrame);
// these tests need a link detected.

TEST_CASE("TerminalSession: a detected path shows the pointing hand, Ctrl or not",
          "[contour][session][hyperlink][links]")
{
    // Detection confirmed the file before the pointer came; the move only looks the cell up.
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(std::format("open {} now", inWorkDirectory("notes.txt")));
    testApp.detectLinks(*session);

    auto const shapeAt = [&](vtbackend::Modifiers modifiers, int column) {
        session->sendMouseMoveEvent(modifiers,
                                    vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                              .column = vtbackend::ColumnOffset(column) },
                                    vtbackend::PixelCoordinate {});
        REQUIRE_FALSE(session.surface->cursorShapes.empty());
        return session.surface->cursorShapes.back();
    };
    using contour::input::MouseCursorShape;

    CHECK(shapeAt(vtbackend::Modifiers {}, 7) == MouseCursorShape::PointingHand);
    CHECK(shapeAt(vtbackend::Modifiers { vtbackend::Modifier::Control }, 8)
          == MouseCursorShape::PointingHand);
    CHECK(shapeAt(vtbackend::Modifiers {}, 1) == MouseCursorShape::IBeam); // "open" names no file here
}

TEST_CASE("TerminalSession: hovering a detected path shows where it resolved", "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(inWorkDirectory("notes.txt") + ":3");
    testApp.detectLinks(*session);
    session->sendMouseMoveEvent(
        {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(2) },
        {});
    CHECK(session->hyperlinkTooltipText().toStdString() == resolvedInWorkDirectory("notes.txt") + ":3:1");
}

TEST_CASE("TerminalSession: each detected link under the pointer shows its own tooltip",
          "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("one.txt"), "");
    testApp.fileSystem().addFile(inWorkDirectory("two.txt"), "");
    auto session = makeSessionWithSurface(testApp.app());
    auto const first = inWorkDirectory("one.txt") + ":3";
    session->terminal().writeToScreen(std::format("{} {}:4", first, inWorkDirectory("two.txt")));
    testApp.detectLinks(*session);
    auto const tooltipAt = [&](int column) {
        session->sendMouseMoveEvent({},
                                    vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                              .column = vtbackend::ColumnOffset(column) },
                                    {});
        return session->hyperlinkTooltipText().toStdString();
    };
    auto const second = static_cast<int>(first.size()) + 2;

    CHECK(tooltipAt(1) == resolvedInWorkDirectory("one.txt") + ":3:1");
    CHECK(tooltipAt(2) == resolvedInWorkDirectory("one.txt") + ":3:1"); // along the same link
    CHECK(tooltipAt(second) == resolvedInWorkDirectory("two.txt") + ":4:1");
    CHECK(tooltipAt(1) == resolvedInWorkDirectory("one.txt") + ":3:1"); // and back
}

TEST_CASE("TerminalSession: hovering a detected link printed as it opens shows no tooltip",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("see https://example.com/page now");
    testApp.detectLinks(*session);
    session->sendMouseMoveEvent(
        {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(8) },
        {});
    REQUIRE(session.surface->cursorShapes.back() == contour::input::MouseCursorShape::PointingHand);
    CHECK(session->hyperlinkTooltipText().isEmpty());
}

TEST_CASE("TerminalSession: the context menu pins a detected path once it is confirmed",
          "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(std::format("open {} now", inWorkDirectory("notes.txt")));
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(7) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    CHECK_FALSE(session->contextMenuState().linkUnderCursor.has_value()); // not yet looked at

    testApp.detectLinks(*session);
    auto const state = session->contextMenuState();
    REQUIRE(state.linkUnderCursor.has_value());
    CHECK(state.linkUnderCursor->kind == vtbackend::LinkKind::Path);
    CHECK(state.linkUnderCursor->target == resolvedInWorkDirectory("notes.txt"));
    CHECK(state.linkUnderCursor->pathKind == vtbackend::PathKind::File);
    CHECK(state.hyperlinkUnderCursor.empty());
}

TEST_CASE("TerminalSession: Ctrl+click on a path detection confirmed opens at once",
          "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(inWorkDirectory("notes.txt"));
    testApp.detectLinks(*session);
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    // Neither the probe thread nor a posted open: the index knows it.
    CHECK((*session)(contour::actions::FollowHyperlink {}));
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().isLocalFile());
}

TEST_CASE("TerminalSession: CopyHyperlink copies a confirmed path's absolute form under the pointer",
          "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("notes.txt"), "");
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().setCurrentWorkingDirectory("file://" + std::string { WorkDirectory });
    session->terminal().writeToScreen("see notes.txt:7");
    testApp.detectLinks(*session);
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(6) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->clear();
    CHECK((*session)(contour::actions::CopyHyperlink {}));
    CHECK(clipboard->text().toStdString() == resolvedInWorkDirectory("notes.txt"));
}
#endif

TEST_CASE("TerminalSession: Ctrl+click opens a detected URL, and a changed row what it holds now",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("https://old.example/");
    testApp.detectLinks(*session);
    session->terminal().writeToScreen("\rhttps://new.example/");
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    CHECK((*session)(contour::actions::FollowHyperlink {}));
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toString().toStdString() == "https://new.example/");
}

TEST_CASE("TerminalSession: a path printed a moment ago is probed, then opened", "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("fresh.txt"), "");
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(inWorkDirectory("fresh.txt"));
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    CHECK((*session)(contour::actions::FollowHyperlink {}));
    CHECK(testApp.launcher().openedUrls.empty()); // not before the probe has answered

    testApp.drainLinkProbes();
    CHECK(testApp.launcher().openedUrls.empty()); // the answer is opened back on the GUI thread
    QCoreApplication::processEvents();
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().isLocalFile());
    CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == inWorkDirectory("fresh.txt"));
}

TEST_CASE("TerminalSession: a path that names no file opens nothing once the probe answers",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(inWorkDirectory("gone.txt"));
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(3) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    CHECK((*session)(contour::actions::FollowHyperlink {})); // asked of the probe
    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    CHECK(testApp.launcher().openedUrls.empty());
}

TEST_CASE("TerminalSession: a follow the probe answers after the session is gone opens nothing",
          "[contour][session][links]")
{
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("fresh.txt"), "");
    {
        auto session = makeDisplaylessSession(testApp.app());
        session->terminal().writeToScreen(inWorkDirectory("fresh.txt"));
        session->terminal().sendMouseMoveEvent(
            vtbackend::Modifiers {},
            vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                      .column = vtbackend::ColumnOffset(3) },
            vtbackend::PixelCoordinate {},
            /*uiHandledHint=*/false);
        REQUIRE((*session)(contour::actions::FollowHyperlink {}));
    }
    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    CHECK(testApp.launcher().openedUrls.empty());
}

TEST_CASE("TerminalSession: a detected file:// URL to a program opens as the path it names, never runs",
          "[contour][session][links]")
{
    // §7.6: an OSC 8 file:// link to a program is run through `contour config`; printed text is not trusted
    // so. A detected one is the path it names, opened as a path is (here with the system's default
    // application), whichever gesture follows it.
    TestApp testApp;
    auto const tool = inWorkDirectory("bin/tool");
    testApp.fileSystem().addExecutable(core::pathFromUtf8(tool).value_or(std::filesystem::path {}));
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(
        "run "
        + localFileUrl(core::pathFromUtf8(tool).value_or(std::filesystem::path {})).toString().toStdString());
    testApp.detectLinks(*session);
    auto const cell =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(6) };
    session->terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {}, cell, vtbackend::PixelCoordinate {}, /*uiHandledHint=*/false);

    SECTION("Ctrl+click")
    {
        CHECK((*session)(contour::actions::FollowHyperlink {}));
    }

    SECTION("vi gx")
    {
        session->followLinkAt(cell);
    }

    SECTION("the context menu's Open Link")
    {
        auto const state = session->contextMenuState();
        REQUIRE(state.linkUnderCursor.has_value());
        CHECK(state.linkUnderCursor->kind == vtbackend::LinkKind::Path);
        CHECK((*session)(contour::actions::FollowHyperlink { .uri = {}, .link = state.linkUnderCursor }));
    }

    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    CHECK(testApp.launcher().detached.empty()); // no `contour config`
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == tool);
}

TEST_CASE("TerminalSession: a pinned detected link opens as pinned", "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    SECTION("a URL")
    {
        auto const link = vtbackend::LinkTarget { .kind = vtbackend::LinkKind::Url,
                                                  .target = "https://pinned.example/",
                                                  .location = std::nullopt,
                                                  .pathKind = vtbackend::PathKind::Missing,
                                                  .directory = {} };
        CHECK((*session)(contour::actions::FollowHyperlink { .uri = {}, .link = link }));
        REQUIRE(testApp.launcher().openedUrls.size() == 1);
        CHECK(testApp.launcher().openedUrls.back().toString().toStdString() == "https://pinned.example/");
    }

    SECTION("a confirmed path, as a local file, without looking again")
    {
        auto const link =
            vtbackend::LinkTarget { .kind = vtbackend::LinkKind::Path,
                                    .target = inWorkDirectory("a.cpp"),
                                    .location = vtbackend::LinkLocation { .line = 3, .column = 1 },
                                    .pathKind = vtbackend::PathKind::File,
                                    .directory = std::string { WorkDirectory } };
        CHECK((*session)(contour::actions::FollowHyperlink { .uri = {}, .link = link }));
        REQUIRE(testApp.launcher().openedUrls.size() == 1);
        CHECK(testApp.launcher().openedUrls.back().isLocalFile());
        CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == inWorkDirectory("a.cpp"));
    }
}

TEST_CASE("TerminalSession: vi gx opens the link under the vi cursor", "[contour][session][links][vi]")
{
    // Not detected yet: gx looks the cell up as Ctrl+click does, scanning its logical line on the spot.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("go https://vi.example/ now");
    (*session)(contour::actions::ViNormalMode {});
    REQUIRE(session->terminal().inputHandler().mode() == vtbackend::ViMode::Normal);

    for (auto const ch: std::u32string_view { U"gg03lgx" })
        session->sendCharEvent(ch,
                               vtbackend::KeyIdentity { .unshiftedKey = ch },
                               vtbackend::Modifiers {},
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());
    QCoreApplication::processEvents();
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toString().toStdString() == "https://vi.example/");
}

TEST_CASE("TerminalSession: vi gx where no link is opens nothing", "[contour][session][links][vi]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("go https://vi.example/ now");
    (*session)(contour::actions::ViNormalMode {});
    mockPtyOf(*session).stdinBuffer().clear();

    for (auto const ch: std::u32string_view { U"gg0gx" })
        session->sendCharEvent(ch,
                               vtbackend::KeyIdentity { .unshiftedKey = ch },
                               vtbackend::Modifiers {},
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());
    QCoreApplication::processEvents();
    CHECK(testApp.launcher().openedUrls.empty());
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // and the keys stay with vi
}

TEST_CASE("TerminalSession: CopyHyperlink copies a detected link's target", "[contour][session][links]")
{
    TestApp testApp;
    // With a display, which the clipboard is written through.
    auto session = makeSessionWithSurface(testApp.app());
    auto* clipboard = QGuiApplication::clipboard();
    REQUIRE(clipboard != nullptr);
    clipboard->clear();

    SECTION("pinned: a path's resolved form")
    {
        auto const link = vtbackend::LinkTarget { .kind = vtbackend::LinkKind::Path,
                                                  .target = inWorkDirectory("a.cpp"),
                                                  .location = std::nullopt,
                                                  .pathKind = vtbackend::PathKind::File,
                                                  .directory = {} };
        CHECK((*session)(contour::actions::CopyHyperlink { .uri = {}, .link = link }));
        CHECK(clipboard->text().toStdString() == inWorkDirectory("a.cpp"));
    }

    SECTION("under the pointer: a URL, but no path the probe has not confirmed")
    {
        session->terminal().writeToScreen(
            std::format("see https://x.example/ and {}", inWorkDirectory("new.txt")));
        auto const pointAt = [&](int column) {
            session->terminal().sendMouseMoveEvent(
                vtbackend::Modifiers {},
                vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                          .column = vtbackend::ColumnOffset(column) },
                vtbackend::PixelCoordinate {},
                /*uiHandledHint=*/false);
        };
        pointAt(8);
        CHECK((*session)(contour::actions::CopyHyperlink {}));
        CHECK(clipboard->text().toStdString() == "https://x.example/");
        pointAt(30);
        CHECK_FALSE((*session)(contour::actions::CopyHyperlink {}));
        CHECK(clipboard->text().toStdString() == "https://x.example/");
    }
}

TEST_CASE("TerminalSession: the context menu pins the detected URL under the pointer",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen(
        std::format("see https://x.example/ and {}", inWorkDirectory("new.txt")));
    auto const stateAt = [&](int column) {
        session->terminal().sendMouseMoveEvent(
            vtbackend::Modifiers {},
            vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                      .column = vtbackend::ColumnOffset(column) },
            vtbackend::PixelCoordinate {},
            /*uiHandledHint=*/false);
        return session->contextMenuState();
    };

    auto const overUrl = stateAt(8);
    REQUIRE(overUrl.linkUnderCursor.has_value());
    CHECK(overUrl.linkUnderCursor->target == "https://x.example/");
    CHECK(overUrl.hyperlinkUnderCursor.empty());

    CHECK_FALSE(stateAt(30).linkUnderCursor.has_value()); // a path, not confirmed yet
    // "see", a bare word, with no directory to resolve it in.
    CHECK_FALSE(stateAt(1).linkUnderCursor.has_value());
}

namespace
{
/// The row file_opener tests print: a file with a location, and a directory.
[[nodiscard]] std::string fileAndDirectoryRow()
{
    return std::format("{}:12:5 {}", inWorkDirectory("a.cpp"), inWorkDirectory("src"));
}

/// The column of the directory in fileAndDirectoryRow(), past its first characters.
[[nodiscard]] int directoryColumn()
{
    return static_cast<int>(std::format("{}:12:5 ", inWorkDirectory("a.cpp")).size()) + 2;
}

/// A session whose profile opens files with @p command, detached, over fileAndDirectoryRow(), its links
/// detected. One session per file_opener value: a profile is fixed for the session's life.
[[nodiscard]] std::unique_ptr<contour::session::TerminalSession> sessionOverDetectedFile(TestApp& testApp,
                                                                                         std::string command)
{
    testApp.fileSystem().addFile(inWorkDirectory("a.cpp"), "");
    testApp.fileSystem().addDirectory(inWorkDirectory("src"));
    auto const name = std::string { "file-opener" };
    registerProfile(testApp.app(), name, [&](contour::config::TerminalProfile& p) {
        p.fileOpener =
            contour::config::FileOpenerConfig { .command = std::move(command),
                                                .placement = contour::actions::OutputPlacement::Detached };
    });
    auto session = makeSessionWithProfile(testApp.app(), name);
    session->terminal().writeToScreen(fileAndDirectoryRow());
    testApp.detectLinks(*session);
    return session;
}

/// Ctrl+click at @p column of the first row, and its probe's answer, opened back on this thread.
bool followAt(TestApp& testApp, contour::session::TerminalSession& session, int column)
{
    session.terminal().sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                  .column = vtbackend::ColumnOffset(column) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    auto const followed = session(contour::actions::FollowHyperlink {});
    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    return followed;
}

/// @return @p arguments, as the launcher received them.
[[nodiscard]] QStringList qArguments(std::initializer_list<std::string> arguments)
{
    auto list = QStringList {};
    for (auto const& argument: arguments)
        list.append(QString::fromStdString(argument));
    return list;
}
} // namespace

TEST_CASE("TerminalSession: file_opener opens a detected file at its location, detached",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = sessionOverDetectedFile(testApp, "editor --goto {path}:{line}:{column}");
    CHECK(followAt(testApp, *session, 3));
    REQUIRE(testApp.launcher().detached.size() == 1);
    CHECK(testApp.launcher().detached.back().program == "editor");
    CHECK(testApp.launcher().detached.back().arguments
          == qArguments({ "--goto", resolvedInWorkDirectory("a.cpp") + ":12:5" }));
    CHECK(testApp.launcher().openedUrls.empty());
}

TEST_CASE("TerminalSession: a directory opens with the system default, file_opener or not",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = sessionOverDetectedFile(testApp, "editor {path}");
    CHECK(followAt(testApp, *session, directoryColumn()));
    CHECK(testApp.launcher().detached.empty());
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == inWorkDirectory("src"));
}

TEST_CASE("TerminalSession: without file_opener a file opens with the system default",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = sessionOverDetectedFile(testApp, "");
    CHECK(followAt(testApp, *session, 3));
    CHECK(testApp.launcher().detached.empty());
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == inWorkDirectory("a.cpp"));
}

TEST_CASE("TerminalSession: an invalid file_opener opens files with the system default",
          "[contour][session][links]")
{
    // Reported when the configuration was read, and kept as written for the settings page: disabled here.
    TestApp testApp;
    auto session = sessionOverDetectedFile(testApp, "editor {file}");
    CHECK(followAt(testApp, *session, 3));
    CHECK(testApp.launcher().detached.empty());
    REQUIRE(testApp.launcher().openedUrls.size() == 1);
    CHECK(testApp.launcher().openedUrls.back().toLocalFile().toStdString() == inWorkDirectory("a.cpp"));
}

TEST_CASE("TerminalSession: a file_opener that does not start says so", "[contour][session][links]")
{
    TestApp testApp;
    testApp.launcher().detachedError = contour::platform::SpawnError::NotFound;
    auto session = sessionOverDetectedFile(testApp, "no-such-editor {path}");
    QSignalSpy const notices(session.get(), &contour::session::TerminalSession::showNotification);
    auto capture = core::log::ScopedCapture { "error" };
    CHECK(followAt(testApp, *session, 3));
    CHECK(capture.contains("Could not open the file:"));
    REQUIRE(notices.count() == 1);
    CHECK(notices.at(0).at(1).toString().contains("Could not open the file"));
}

TEST_CASE("TerminalSession: file_opener in a split or a tab runs in a pane where the path resolved",
          "[contour][session][links]")
{
    auto const placement =
        GENERATE(contour::actions::OutputPlacement::Split, contour::actions::OutputPlacement::Tab);
    auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = owned.get();
    TestApp testApp { std::move(owned) };
    auto const win = contour::test::ScopedController { testApp.manager() };
    testApp.fileSystem().addFile(inWorkDirectory("a.cpp"), "");
    auto& config = testApp.app().config();
    config.profile(testApp.app().profileName())->fileOpener =
        contour::config::FileOpenerConfig { .command = "editor +{line} {path}", .placement = placement };
    auto* const session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    session->terminal().setCurrentWorkingDirectory("file://" + std::string { WorkDirectory });
    session->terminal().writeToScreen("a.cpp:12:5");

    CHECK(followAt(testApp, *session, 1));
    CHECK(win->count() == (placement == contour::actions::OutputPlacement::Tab ? 2 : 1));
    auto const& command = factory->requestedCommandOverrides.back();
    REQUIRE(command.has_value());
    CHECK(command->program == "editor");
    CHECK(command->arguments == std::vector<std::string> { "+12", resolvedInWorkDirectory("a.cpp") });
    CHECK(command->workingDirectory.lexically_normal()
          == core::pathFromUtf8(WorkDirectory).value_or(std::filesystem::path {}).lexically_normal());
    CHECK(testApp.launcher().detached.empty());
}

TEST_CASE("TerminalSession: a file_opener pane works where the file is, not where the shell is now",
          "[contour][session][links]")
{
    // The path names a file in another directory than the one the shell reports: the pane starts in the
    // file's, which beats the one a new pane would inherit from the acting pane.
#ifdef _WIN32
    constexpr auto Elsewhere = std::string_view { "C:/elsewhere" };
#else
    constexpr auto Elsewhere = std::string_view { "/elsewhere" };
#endif
    auto const placement =
        GENERATE(contour::actions::OutputPlacement::Split, contour::actions::OutputPlacement::Tab);
    auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = owned.get();
    TestApp testApp { std::move(owned) };
    auto const win = contour::test::ScopedController { testApp.manager() };
    auto const file = std::format("{}/a.cpp", Elsewhere);
    testApp.fileSystem().addFile(file, "");
    testApp.app().config().profile(testApp.app().profileName())->fileOpener =
        contour::config::FileOpenerConfig { .command = "editor {path}", .placement = placement };
    auto* const session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    session->terminal().setCurrentWorkingDirectory("file://" + std::string { WorkDirectory });
    session->terminal().writeToScreen(file + ":3");

    CHECK(followAt(testApp, *session, 3));
    REQUIRE_FALSE(factory->requestedCwds.empty());
    REQUIRE(factory->requestedCwds.back().has_value());
    CHECK(*factory->requestedCwds.back()
          == core::pathToUtf8(
              core::pathFromUtf8(Elsewhere).value_or(std::filesystem::path {}).lexically_normal()));
}

TEST_CASE("TerminalSession: a pane whose command names no directory works where the acting pane does",
          "[contour][session]")
{
    auto const placement =
        GENERATE(contour::session::CommandPanePlacement::Split, contour::session::CommandPanePlacement::Tab);
    auto owned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* const factory = owned.get();
    TestApp testApp { std::move(owned) };
    auto const win = contour::test::ScopedController { testApp.manager() };
    auto* const session = testApp.manager().createSession(win.id);
    REQUIRE(session != nullptr);
    auto const inherited = session->workingDirectory();

    auto command = vtpty::Process::ExecInfo {};
    command.program = "pager";
    REQUIRE(testApp.manager().openCommandPane(session, placement, command));
    REQUIRE_FALSE(factory->requestedCwds.empty());
    CHECK(factory->requestedCwds.back() == std::optional { inherited });
}

TEST_CASE("TerminalSession: hint mode opens a path with file_opener too",
          "[contour][session][links][hintmode]")
{
    TestApp testApp;
    auto session = sessionOverDetectedFile(testApp, "editor {path}:{line}");
    // Hint mode resolves an absolute path where no working directory is known: it names one place.
    REQUIRE((*session)(contour::actions::HintMode { .patterns = "filepath",
                                                    .hintAction = vtbackend::HintAction::Open,
                                                    .scope = vtbackend::HintScope::Visible }));
    // The first match on the page gets the first label.
    session->sendCharEvent(U'a',
                           vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                           vtbackend::Modifiers {},
                           vtbackend::KeyboardEventType::Press,
                           std::chrono::steady_clock::now());
    QCoreApplication::processEvents();
    REQUIRE(testApp.launcher().detached.size() == 1);
    CHECK(testApp.launcher().detached.back().program == "editor");
    CHECK(testApp.launcher().detached.back().arguments
          == qArguments({ resolvedInWorkDirectory("a.cpp") + ":12" }));
}

TEST_CASE("TerminalSession: hint mode's Open is refused where Ctrl+click is",
          "[contour][session][links][hintmode]")
{
    // One follow path for every kind of link: a file:// URL in a pane replicating another machine names a
    // file there, and so does a path a command printed on another machine.
    TestApp testApp;
    testApp.fileSystem().addFile(inWorkDirectory("doc.txt"), "x");
    auto session = makeDisplaylessSession(testApp.app());
    auto const openFirstHint = [&](std::string patterns) {
        auto const hintMode = contour::actions::HintMode { .patterns = std::move(patterns),
                                                           .hintAction = vtbackend::HintAction::Open,
                                                           .scope = vtbackend::HintScope::Visible };
        REQUIRE((*session)(hintMode));
        session->sendCharEvent(U'a',
                               vtbackend::KeyIdentity { .unshiftedKey = U'a' },
                               vtbackend::Modifiers {},
                               vtbackend::KeyboardEventType::Press,
                               std::chrono::steady_clock::now());
        testApp.drainLinkProbes();
        QCoreApplication::processEvents();
    };

    SECTION("a file:// URL in a pane replicating another machine")
    {
        session->terminal().bindReplica(vtbackend::ReplicaRole::OtherMachine, vtbackend::HostIdentity {});
        session->terminal().writeToScreen(
            "see "
            + localFileUrl(std::filesystem::path { inWorkDirectory("doc.txt") }).toString().toStdString());
        openFirstHint("url");
    }

    SECTION("a path a command printed on another machine")
    {
        auto const generic = std::string { WorkDirectory };
        auto const remote =
            "file://otherhost.invalid" + std::string(generic.starts_with('/') ? "" : "/") + generic;
        session->terminal().writeToScreen("\033]7;" + remote + "\033\\"
                                          + "\033]133;A\033\\$ ls\r\n\033]133;C\033\\"
                                          + "open doc.txt now\r\n\033]133;D;0\033\\");
        session->terminal().setCurrentWorkingDirectory("file://" + generic);
        openFirstHint("filepath");
    }

    CHECK(testApp.launcher().openedUrls.empty());
    CHECK(testApp.launcher().detached.empty());
}

TEST_CASE("TerminalSession: Ctrl flips the pointer over a link in place", "[contour][session][links]")
{
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    session->terminal().setLinkDecorationMode(vtbackend::LinkDecorationMode::Modifier);
    session->sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) },
        vtbackend::PixelCoordinate {});
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);

    auto const now = std::chrono::steady_clock::now();
    session->sendKeyEvent(vtbackend::Key::LeftControl,
                          vtbackend::KeyboardModifiers {},
                          vtbackend::KeyboardEventType::Press,
                          now);
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);
    session->sendKeyEvent(vtbackend::Key::LeftControl,
                          vtbackend::KeyboardModifiers { vtbackend::Modifier::Control },
                          vtbackend::KeyboardEventType::Release,
                          now);
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);
}

TEST_CASE("TerminalSession: a move on the same cell that changes the chord re-decides the pointer",
          "[contour][session][links]")
{
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    session->terminal().setLinkDecorationMode(vtbackend::LinkDecorationMode::Modifier);
    auto const onLink =
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) };
    session->sendMouseMoveEvent(vtbackend::Modifiers {}, onLink, vtbackend::PixelCoordinate {});
    REQUIRE(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);

    // Ctrl went down while another window had the focus: no key event reached this session, only the move.
    session->sendMouseMoveEvent(
        vtbackend::Modifiers { vtbackend::Modifier::Control }, onLink, vtbackend::PixelCoordinate {});
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);

    session->sendMouseMoveEvent(vtbackend::Modifiers {}, onLink, vtbackend::PixelCoordinate {});
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);
}

TEST_CASE("TerminalSession: a config reload moves the follow modifiers with the binding",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    REQUIRE(session->terminal().settings().linkFollowModifiers
            == std::optional { vtbackend::Modifiers { vtbackend::Modifier::Control } });

    auto reloaded = testApp.app().config();
    for (auto& mapping: reloaded.inputMappings.value().mouseMappings)
        if (std::ranges::any_of(mapping.binding, [](contour::actions::Action const& action) {
                return std::holds_alternative<contour::actions::FollowHyperlink>(action);
            }))
            mapping.modifiers = vtbackend::Modifiers { vtbackend::Modifier::Alt };
    session->applyReloadedConfig(reloaded, reloaded.defaultProfileName.value());

    CHECK(session->terminal().settings().linkFollowModifiers
          == std::optional { vtbackend::Modifiers { vtbackend::Modifier::Alt } });
    (void) session->terminal().updateHeldModifiers(vtbackend::Modifier::Alt);
    CHECK(session->terminal().followModifiers() == vtbackend::FollowModifiers::Held);
}

TEST_CASE("TerminalSession: losing focus with Ctrl down lets go of it", "[contour][session][links]")
{
    using contour::input::MouseCursorShape;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    session->terminal().setLinkDecorationMode(vtbackend::LinkDecorationMode::Modifier);
    session->sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) },
        vtbackend::PixelCoordinate {});
    session->sendKeyEvent(vtbackend::Key::LeftControl,
                          vtbackend::KeyboardModifiers { vtbackend::Modifier::Control },
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    REQUIRE(session.surface->cursorShapes.back() == MouseCursorShape::PointingHand);

    // Ctrl+Alt+Tab: the release goes to the window that took the focus.
    session->sendFocusOutEvent();
    CHECK(session.surface->cursorShapes.back() == MouseCursorShape::IBeam);
    CHECK(session->terminal().followModifiers() == vtbackend::FollowModifiers::Released);
}

TEST_CASE("TerminalSession: a modifier press while the pointer is off the grid leaves the shape alone",
          "[contour][session][links]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("\033]8;;https://a.b/\033\\link\033]8;;\033\\");
    session->terminal().setLinkDecorationMode(vtbackend::LinkDecorationMode::Modifier);
    session->sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) },
        vtbackend::PixelCoordinate {});
    session->onPointerLeft();
    session.surface->cursorShapes.clear();

    session->sendKeyEvent(vtbackend::Key::LeftControl,
                          vtbackend::KeyboardModifiers { vtbackend::Modifier::Control },
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());
    CHECK(session.surface->cursorShapes.empty());
}

TEST_CASE("TerminalSession: Ctrl over output the parser thread is writing reads the screen under the lock",
          "[contour][session][hyperlink][threading]")
{
    // The pointer shows whether it rests on something to open -- a link, detected or OSC 8, or a collapsed
    // fold's label: the grid, the link index and the fold cache, which the parser thread changes as output
    // arrives. That is read inside the lock the move takes. Read after it, ThreadSanitizer
    // (`ctest --preset=clang-tsan`) reports the fold cache raced between this thread and the writer;
    // elsewhere this stands as a no-crash smoke test.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());

    // The writer stands in for the parser thread: writeToScreen() takes the terminal lock around the parse.
    // The pointer moves for as long as it writes its rounds rather than for a count of its own, so that a
    // lock that is not fair cannot let every move run before the first block is written.
    auto const ctrl = vtbackend::Modifiers { vtbackend::Modifier::Control };
    auto move = 0;
    std::ignore = vtbackend::test::runWhileWriting(
        [&] {
            session->terminal().writeToScreen(
                "\033]133;A\033\\$ make\r\n\033]133;C\033\\src/main.cpp\r\n\033]133;D;0\033\\");
        },
        [&] {
            session->sendMouseMoveEvent(
                ctrl,
                vtbackend::CellLocation { .line = vtbackend::LineOffset(move % 20),
                                          .column = vtbackend::ColumnOffset(move % 3) },
                vtbackend::PixelCoordinate {});
            ++move;
        });
    CHECK(session->terminal().commandBlocks().size() > 0);
}

TEST_CASE("TerminalSession: a display that goes away takes the tooltips with it",
          "[contour][session][gutter]")
{
    // A tab switch detaches the display and gives it to another session. Nothing else ends the hover, so
    // switching back brought the tooltip back at the cell it had been entered at, wherever the pointer was.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());

    SECTION("the block's")
    {
        runFailingCommand(*session);
        session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
        REQUIRE_FALSE(session->commandBlockTooltipAnchor().isEmpty());

        session->detachDisplay(*session.surface);
        CHECK(session->commandBlockTooltipAnchor().isEmpty());
        CHECK(session->commandBlockTooltip().isEmpty());
    }

    SECTION("the hyperlink's")
    {
        session->terminal().writeToScreen("\033]8;;https://example.com/\033\\link\033]8;;\033\\");
        session->sendMouseMoveEvent(vtbackend::Modifiers {},
                                    vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                              .column = vtbackend::ColumnOffset(1) },
                                    vtbackend::PixelCoordinate {});
        REQUIRE_FALSE(session->hyperlinkTooltipText().isEmpty());

        session->detachDisplay(*session.surface);
        CHECK(session->hyperlinkTooltipText().isEmpty());
    }
}

TEST_CASE("TerminalSession: the block tooltip follows the hovered block to its finish",
          "[contour][session][gutter]")
{
    // The text is read as a block is entered, so a command that finished under a stationary pointer kept
    // saying "Running" until the pointer left its block.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());

    SECTION("the hovered block finishes")
    {
        // The start is a change too: let the GUI thread take what it posted before the pointer arrives.
        session->terminal().writeToScreen(vtbackend::test::commandStarted("make"));
        QCoreApplication::processEvents();
        session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
        REQUIRE(session->commandBlockTooltip().startsWith("Running"));
        QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

        session->terminal().writeToScreen(vtbackend::test::commandEnd(2));
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(session->commandBlockTooltip().startsWith("Exit code 2"));
    }

    SECTION("another block finishes")
    {
        runFailingCommand(*session);
        session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
        REQUIRE_FALSE(session->commandBlockTooltip().isEmpty());
        QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

        session->terminal().writeToScreen(vtbackend::test::commandRun("ls", { "out" }));
        QCoreApplication::processEvents();
        CHECK(spy.count() == 0);
    }

    SECTION("a newer prompt closes the hovered block without reporting its end")
    {
        // An `ssh` the user is still inside, or a shell that sends no ;D: the block is finished all the same.
        session->terminal().writeToScreen(vtbackend::test::commandStarted("ssh host"));
        QCoreApplication::processEvents();
        session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
        REQUIRE(session->commandBlockTooltip().startsWith("Running"));
        QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

        session->terminal().writeToScreen("\033]133;A\033\\");
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(session->commandBlockTooltip().startsWith("Finished, exit code unknown"));
    }

    SECTION("how the hovered block's command died arrives after its end")
    {
        // OSC 133 owns the blocks, so an OSC 3008 end= only says how the command ended -- and may follow ;D.
        auto& terminal = session->terminal();
        terminal.writeToScreen(vtbackend::test::typedAtPrompt("serve"));
        terminal.writeToScreen("\033]3008;start=cmd-1;type=command\033\\");
        terminal.writeToScreen("\033]133;C\033\\stopping\r\n" + vtbackend::test::commandEnd(1));
        QCoreApplication::processEvents();
        session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
        REQUIRE(session->commandBlockTooltip().startsWith("Exit code 1\n"));
        QSignalSpy const spy(session.session.get(), &TerminalSession::commandBlockHoverChanged);

        terminal.writeToScreen("\033]3008;end=cmd-1;exit=interrupt;signal=SIGTERM\033\\");
        QCoreApplication::processEvents();
        CHECK(spy.count() == 1);
        CHECK(session->commandBlockTooltip().startsWith("Exit code 1 (SIGTERM)"));
    }
}

namespace
{
/// The system clipboard for one test: it holds @p initial from the start, and is cleared however the test
/// ends -- a failed REQUIRE included -- so that no test sees what another left there.
class ClipboardGuard
{
  public:
    /// @param initial What the clipboard holds to begin with; empty leaves it as it is.
    explicit ClipboardGuard(QString const& initial = {}): _clipboard { QGuiApplication::clipboard() }
    {
        REQUIRE(_clipboard != nullptr);
        if (!initial.isEmpty())
            _clipboard->setText(initial);
    }
    ClipboardGuard(ClipboardGuard const&) = delete;
    ClipboardGuard& operator=(ClipboardGuard const&) = delete;
    ClipboardGuard(ClipboardGuard&&) = delete;
    ClipboardGuard& operator=(ClipboardGuard&&) = delete;
    ~ClipboardGuard() { _clipboard->clear(); }

    /// @return What the clipboard holds now.
    [[nodiscard]] std::string text() const { return _clipboard->text().toStdString(); }

  private:
    QClipboard* _clipboard;
};

/// A window over a mock PTY factory with one session whose last command has finished: what an
/// OpenCommandOutput test acts from, with the notices it shows recorded.
struct PagerFixture
{
    /// @param pager What command_blocks.pager is set to before the session is made; std::nullopt keeps
    ///              the default.
    /// @param typed The command the finished block ran.
    /// @param output What it printed, as written.
    PagerFixture(std::optional<std::string> const& pager, std::string_view typed, std::string_view output):
        PagerFixture(std::make_unique<contour::test::MockPtySessionFactory>(), pager, typed, output)
    {
    }

    /// @return The text of every notice the acting session showed, in order.
    [[nodiscard]] std::vector<std::string> notices() const
    {
        auto texts = std::vector<std::string> {};
        for (auto const& arguments: noticeSpy)
            texts.push_back(arguments.at(1).toString().toStdString());
        return texts;
    }

    contour::test::MockPtySessionFactory* factory; ///< Owned by the app.
    TestApp app;
    contour::test::ScopedController win;
    contour::session::TerminalSession* acting;
    QSignalSpy noticeSpy;

  private:
    PagerFixture(std::unique_ptr<contour::test::MockPtySessionFactory> owned,
                 std::optional<std::string> const& pager,
                 std::string_view typed,
                 std::string_view output):
        factory { owned.get() },
        app { std::move(owned) },
        win { app.manager() },
        acting { actingSession(app, win, pager, typed, output) },
        noticeSpy { acting, &contour::session::TerminalSession::showNotification }
    {
    }

    /// Makes the session the actions run in, once the configuration it copies is as the test wants it.
    [[nodiscard]] static contour::session::TerminalSession* actingSession(
        TestApp& app,
        contour::test::ScopedController const& win,
        std::optional<std::string> const& pager,
        std::string_view typed,
        std::string_view output)
    {
        if (pager)
            app.app().config().commandBlocks.value().pager = *pager;
        auto* session = app.manager().createSession(win.id);
        REQUIRE(session != nullptr);
        writeFinishedBlock(*session, typed, output);
        return session;
    }
};
} // namespace

TEST_CASE("TerminalSession: SelectCommandBlock selects a part of a block",
          "[contour][session][actions][semanticblocks]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    writeFinishedBlock(*session, "ls", "file1\r\nfile2\r\n");

    CHECK((*session)(contour::actions::SelectCommandBlock {}));
    CHECK(session->terminal().extractSelectionText() == "file1\nfile2\n");

    CHECK((*session)(contour::actions::SelectCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
    CHECK(session->terminal().extractSelectionText() == "$ ls\n");

    // The live prompt has no output yet: nothing to select, and the action says so.
    CHECK_FALSE(
        (*session)(contour::actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor }));

    // A pinned block wins over the target: a context-menu row acts on the block it was opened on, wherever
    // the cursor is now.
    auto const* const finished =
        session->terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Last);
    REQUIRE(finished != nullptr);
    session->terminal().clearSelection();
    CHECK((*session)(contour::actions::SelectCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor,
                                                            .block = finished->id }));
    CHECK(session->terminal().extractSelectionText() == "file1\nfile2\n");
}

namespace
{

/// A cell on the first output row of the block writeListingBlock() writes.
constexpr auto OverListingOutput =
    vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(2) };

/// The output part of the block writeListingBlock() writes, as a selection reads it.
constexpr auto ListingOutput = std::string_view { "alpha beta\ngamma\n" };

/// Writes one finished command, `ls` printing two lines, and the next prompt below it.
void writeListingBlock(contour::session::TerminalSession& session)
{
    writeFinishedBlock(session, "ls", "alpha beta\r\ngamma\r\n");
}

/// Presses and releases @p button @p count times on OverListingOutput, as fast as a multi-click.
void clickOverListing(contour::session::TerminalSession& session,
                      int count,
                      Modifiers modifiers,
                      vtbackend::MouseButton button = vtbackend::MouseButton::Left)
{
    auto const pixels = vtbackend::PixelCoordinate {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, count))
    {
        session.sendMousePressEvent(modifiers, button, OverListingOutput, pixels);
        session.sendMouseReleaseEvent(modifiers, button, pixels);
    }
}

} // namespace

TEST_CASE("TerminalSession: Ctrl+triple-click selects the output under the pointer",
          "[contour][session][input][semanticblocks]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    writeListingBlock(*session);
    auto const ctrl = Modifiers { vtbackend::Modifier::Control };

    SECTION("the third Ctrl+click selects the block's output")
    {
        clickOverListing(*session, 3, ctrl);
        CHECK(session->terminal().extractSelectionText() == ListingOutput);
    }

    SECTION("without Ctrl the third click is still the terminal's own selection")
    {
        clickOverListing(*session, 3, Modifiers {});
        CHECK(session->terminal().selectionAvailable());
        CHECK(session->terminal().extractSelectionText() != ListingOutput);
    }

    SECTION("the clicks before the third are the terminal's own")
    {
        clickOverListing(*session, 2, ctrl);
        CHECK(session->terminal().extractSelectionText() == "alpha");
    }

    SECTION("a pause longer than the multi-click interval starts the count again")
    {
        // The app's clocks are the fixture's: the pause takes no time, and the third click is a first.
        clickOverListing(*session, 2, ctrl);
        testApp.steadyClock().advance(std::chrono::seconds(10));
        clickOverListing(*session, 1, ctrl);
        CHECK(session->terminal().extractSelectionText() != ListingOutput);
    }

    SECTION("a third Ctrl+click of another button is not claimed")
    {
        clickOverListing(*session, 3, ctrl, vtbackend::MouseButton::Middle);
        CHECK_FALSE(session->terminal().selectionAvailable());
    }

    SECTION("an application that asked for the mouse keeps its clicks")
    {
        session->terminal().writeToScreen("\033[?1000h");
        clickOverListing(*session, 3, ctrl);
        CHECK_FALSE(session->terminal().selectionAvailable());
        CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
    }
}

TEST_CASE("TerminalSession: a Ctrl+triple-click over a row of no block leaves the second click's word",
          "[contour][session][input][semanticblocks]")
{
    // The third click is claimed before the block is looked for: finding none, nothing is selected in its
    // place, and the word the second click selected stays.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().writeToScreen("plain words\r\n");
    auto const overPlain = vtbackend::CellLocation { vtbackend::LineOffset(0), vtbackend::ColumnOffset(1) };
    auto const ctrl = Modifiers { vtbackend::Modifier::Control };
    auto const pixels = vtbackend::PixelCoordinate {};

    for ([[maybe_unused]] auto const _: std::views::iota(0, 3))
    {
        session->sendMousePressEvent(ctrl, vtbackend::MouseButton::Left, overPlain, pixels);
        session->sendMouseReleaseEvent(ctrl, vtbackend::MouseButton::Left, pixels);
    }

    CHECK(session->terminal().extractSelectionText() == "plain");
}

TEST_CASE("TerminalSession: the user's own click-count binding comes before the built-in one",
          "[contour][session][input][semanticblocks]")
{
    TestApp testApp;
    testApp.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, action: SendChars, chars: x }
    - { mods: [Control], mouse: Left, clicks: 3, action: SelectCommandBlock, target: pointer, part: all }
)")
                                               .inputMappings;
    auto session = makeDisplaylessSession(testApp.app());
    writeListingBlock(*session);

    clickOverListing(*session, 3, Modifiers { vtbackend::Modifier::Control });

    // The user's triple-click selects the whole block, not the built-in output; and the plain Ctrl+click
    // binding ran for the two presses the triple-click did not claim, and only for those.
    CHECK(session->terminal().extractSelectionText() == "$ ls\nalpha beta\ngamma\n");
    CHECK(mockPtyOf(*session).stdinBuffer() == "xx");
}

TEST_CASE("TerminalSession: with key mappings off, a click count claims only what would still run",
          "[contour][session][input][semanticblocks]")
{
    // Double-clicks rather than triple-clicks: a claimed second click skips the word selection and leaves
    // none, where the third would leave the second's word -- the same text the terminal's own third click
    // selects, so a wrongful claim could not be told from none.
    TestApp testApp;
    testApp.app().config().inputMappings = contour::test::loadConfigFromYaml(R"(
default_profile: main
profiles:
    main:
        shell: /bin/sh
input_mapping:
    - { mods: [Control], mouse: Left, clicks: 2, action: ToggleAllKeyMaps }
    - { mods: [Alt], mouse: Left, clicks: 2, action: SelectAll }
)")
                                               .inputMappings;
    auto session = makeDisplaylessSession(testApp.app());
    writeListingBlock(*session);
    auto const disabledNotice = [&]() {
        return session->terminal().indicatorStatusLineDisplay().renderMainPageText().contains(
            "key bindings disabled");
    };

    REQUIRE((*session)(contour::actions::ToggleAllKeyMaps {}));
    REQUIRE(disabledNotice());

    SECTION("a refused binding leaves the click to the terminal's own selection")
    {
        // Claimed, the press would skip the selection, and executeAllActions() would then refuse the
        // binding: the double-click would have done nothing at all.
        clickOverListing(*session, 2, Modifiers { vtbackend::Modifier::Alt });
        CHECK(session->terminal().extractSelectionText() == "alpha");
        CHECK(disabledNotice());
    }

    SECTION("the binding that turns them back on still claims its click")
    {
        // executeAllActions() still runs this one while the others are refused: no word is selected, and
        // the bindings are back on.
        clickOverListing(*session, 2, Modifiers { vtbackend::Modifier::Control });
        CHECK_FALSE(session->terminal().selectionAvailable());
        CHECK_FALSE(disabledNotice());
    }
}

TEST_CASE("TerminalSession: the block copy actions copy one part of a block",
          "[contour][session][clipboard][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    writeFinishedBlock(*session, "make test", "ok\r\n", ";cmdline_url=make%20test");

    SECTION("the output of the last command")
    {
        CHECK((*session)(contour::actions::CopyCommandBlock {}));
        CHECK(clipboard.text() == "ok");
    }

    SECTION("its prompt")
    {
        CHECK((*session)(contour::actions::CopyCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
        CHECK(clipboard.text() == "$ make test");
    }

    SECTION("its command line")
    {
        CHECK((*session)(contour::actions::CopyCommandLine {}));
        CHECK(clipboard.text() == "make test");
    }

    SECTION("a part that is not there leaves the clipboard alone")
    {
        CHECK_FALSE((*session)(
            contour::actions::CopyCommandBlock { .target = vtbackend::CommandBlockTarget::Cursor }));
        CHECK(clipboard.text() == "untouched");
    }

    SECTION("a pinned block wins over the target")
    {
        // The cursor stands on the live prompt, which has neither output nor a command line yet: only the
        // pin can name the finished block, as a context-menu row does.
        auto const* const finished =
            session->terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Last);
        REQUIRE(finished != nullptr);
        auto const atCursor = vtbackend::CommandBlockTarget::Cursor;

        CHECK((*session)(contour::actions::CopyCommandBlock { .target = atCursor, .block = finished->id }));
        CHECK(clipboard.text() == "ok");

        CHECK_FALSE((*session)(contour::actions::CopyCommandLine { .target = atCursor }));
        CHECK((*session)(contour::actions::CopyCommandLine { .target = atCursor, .block = finished->id }));
        CHECK(clipboard.text() == "make test");
    }
}

TEST_CASE("TerminalSession: CopyCommandLine copies a line that is safe to paste",
          "[contour][session][clipboard][semanticblocks]")
{
    // A command line arrives from whatever wrote to the pty. One carrying ESC [ 201 ~ would end a
    // bracketed paste early and run the rest as typed input in the shell it is pasted into, so the copy
    // is sanitised for insertion: the ESC goes, the harmless rest stays.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard {};

    writeFinishedBlock(*session, "", "", ";cmdline_url=ls%1b%5b201~%3bid");

    CHECK((*session)(contour::actions::CopyCommandLine {}));
    CHECK(clipboard.text() == "ls[201~;id");
}

TEST_CASE("TerminalSession: CopyCommandLine preserves multiline commands only when enabled",
          "[contour][session][clipboard][semanticblocks]")
{
    TestApp testApp;
    auto expected = std::string { "# comment  Write-Output @'   text  '@" };
    SECTION("the default keeps single-line copying")
    {
        CHECK_FALSE(testApp.app()
                        .config()
                        .profile(testApp.app().profileName())
                        ->copyCommandPreserveMultiline.value());
    }
    SECTION("the profile can enable multiline copying")
    {
        testApp.app().config().profile(testApp.app().profileName())->copyCommandPreserveMultiline = true;
        expected = "# comment\nWrite-Output @'\n\ttext\n'@";
    }
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard {};

    writeFinishedBlock(
        *session,
        "# comment",
        "",
        ";cmdline_url=%23%20comment%0D%0AWrite-Output%20%40%27%0D%0A%09text%0D%0A%27%40%0D%0A");

    CHECK((*session)(contour::actions::CopyCommandLine {}));
    CHECK(clipboard.text() == expected);
}

TEST_CASE("TerminalSession: the last command's output is copied up to where the command ended",
          "[contour][session][clipboard][semanticblocks]")
{
    // `printf foo` leaves no newline, so the next prompt shares the row its output ends on. The copy takes
    // the columns before the end mark, and none of the prompt after it.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard {};

    writeFinishedBlock(*session, "printf foo", "foo");

    CHECK((*session)(contour::actions::CopyLastCommandOutput {}));
    CHECK(clipboard.text() == "foo");
    CHECK((*session)(contour::actions::CopyLastCommandBlock {}));
    CHECK(clipboard.text() == "$ printf foo\nfoo");
}

TEST_CASE("TerminalSession: an empty part or command line is nothing to copy",
          "[contour][session][clipboard][semanticblocks]")
{
    // Copying "" would only destroy what the clipboard held, so a part of blank rows and a command line
    // the sanitiser leaves nothing of are refused, as a part that is not there is.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    writeFinishedBlock(*session, "", "\r\n\r\n", ";cmdline_url=%1b%07");

    // The blocks are there, with something to hold: blank output rows and a raw command line.
    auto const output = session->terminal().commandBlockText(vtbackend::CommandBlockTarget::Last,
                                                             vtbackend::CommandBlockPart::Output,
                                                             vtbackend::CaptureRendition::PlainText);
    REQUIRE(output.has_value());
    REQUIRE(output->empty());
    auto const* const finished =
        session->terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Last);
    REQUIRE(finished != nullptr);
    REQUIRE_FALSE(finished->commandLine.empty());

    CHECK_FALSE((*session)(contour::actions::CopyCommandBlock {}));
    CHECK_FALSE(
        (*session)(contour::actions::CopyLastCommandOutput {})); // the same text, through the same path
    CHECK_FALSE((*session)(contour::actions::CopyCommandLine {}));
    CHECK(clipboard.text() == "untouched");
}

TEST_CASE("TerminalSession: CopySelection may fall back to the last command's output",
          "[contour][session][clipboard][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };

    writeFinishedBlock(*session, "make", "built\r\n");
    auto const copyOrLastOutput =
        contour::actions::CopySelection { .fallback = contour::actions::CopyFallback::LastCommandOutput };

    SECTION("with nothing selected it copies what the last command printed")
    {
        CHECK((*session)(copyOrLastOutput));
        CHECK(clipboard.text() == "built");
    }

    SECTION("a selection wins over the fallback")
    {
        CHECK(
            (*session)(contour::actions::SelectCommandBlock { .part = vtbackend::CommandBlockPart::Prompt }));
        CHECK((*session)(copyOrLastOutput));
        CHECK(clipboard.text() == "$ make\n");
    }

    SECTION("without the fallback an empty selection is copied, as it always was")
    {
        CHECK((*session)(contour::actions::CopySelection {}));
        CHECK(clipboard.text().empty());
    }
}

TEST_CASE("TerminalSession: CopySelection's fallback copies nothing over the clipboard",
          "[contour][session][clipboard][semanticblocks]")
{
    // The fallback reads the last command's output through the block copy path, so blank output, or no
    // command at all, is nothing to copy: the clipboard keeps what it held.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const clipboard = ClipboardGuard { QStringLiteral("untouched") };
    auto const copyOrLastOutput =
        contour::actions::CopySelection { .fallback = contour::actions::CopyFallback::LastCommandOutput };

    SECTION("no command has run")
    {
        CHECK_FALSE((*session)(copyOrLastOutput));
    }

    SECTION("the last command printed only blank lines")
    {
        writeFinishedBlock(*session, "true", "\r\n\r\n");
        REQUIRE(session->terminal().targetedCommandBlock(vtbackend::CommandBlockTarget::Last) != nullptr);
        CHECK_FALSE((*session)(copyOrLastOutput));
    }

    CHECK(clipboard.text() == "untouched");
}

TEST_CASE("TerminalSession: ClearToPrompt leaves the prompt at the top of an empty scrollback",
          "[contour][session][actions][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    for (auto const i: std::views::iota(0, 40))
        session->terminal().writeToScreen(std::format("line {}\r\n", i));

    SECTION("with a prompt, everything above it goes")
    {
        session->terminal().writeToScreen(vtbackend::test::PromptStart);
        REQUIRE(session->terminal().primaryScreen().historyLineCount() > vtbackend::LineCount(0));
        session->refreshScrollbarMarks(); // what output asks for, to publish its travel
        REQUIRE(session->historyLineCount() > 0);

        CHECK((*session)(contour::actions::ClearToPrompt {}));

        CHECK(session->terminal().primaryScreen().historyLineCount() == vtbackend::LineCount(0));
        CHECK(session->terminal().primaryScreen().grid().lineTextTrimmed(vtbackend::LineOffset(0)) == "$");
        // What the scrollbar is sized from is published, never computed: the action has to announce it.
        CHECK(session->historyLineCount() == 0);
    }

    SECTION("without one, nothing is cleared")
    {
        auto const history = session->terminal().primaryScreen().historyLineCount();
        CHECK_FALSE((*session)(contour::actions::ClearToPrompt {}));
        CHECK(session->terminal().primaryScreen().historyLineCount() == history);
    }
}

TEST_CASE("TerminalSession: OpenCommandOutput shows a command's output in a pager",
          "[contour][session][actions][semanticblocks]")
{
    auto fixture = PagerFixture { "less -R", "make", "\033[32mbuilt\033[m\r\n" };
    auto& app = fixture.app;
    auto* const factory = fixture.factory;
    auto const& win = fixture.win;
    auto* const acting = fixture.acting;

    auto& fileSystem = app.fileSystem();
    auto const directory = std::filesystem::path(TestApp::OutputDirectory);
    auto const filesLeft = [&]() {
        return fileSystem.listDirectory(directory)
            .value_or(std::vector<core::platform::FileSystem::DirectoryEntry> {})
            .size();
    };

    SECTION("in a split running the pager over a private file, which goes with the pane")
    {
        REQUIRE((*acting)(contour::actions::OpenCommandOutput {}));
        CHECK(win->count() == 1);

        // The configured string is the program, read by the spawn as it reads a shell setting; the file
        // comes after it.
        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(command->program == "less -R");
        REQUIRE(command->arguments.size() == 1);

        auto const file = std::filesystem::path(command->arguments[0]);
        CHECK(file.parent_path() == directory);
        auto const content = fileSystem.readFile(file).value_or("");
        CHECK(content.contains("built"));
        CHECK(content.contains("\033[")); // format: sgr is the default
        CHECK(fileSystem.permissions(file).value_or(std::filesystem::perms::none)
              == (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write));
        CHECK(fileSystem.permissions(directory).value_or(std::filesystem::perms::none)
              == std::filesystem::perms::owner_all);

        auto* pager =
            app.manager().sessionForId(vtworkspace::SessionId { acting->modelSessionId().value + 1 });
        REQUIRE(pager != nullptr);
        emit pager->sessionClosed(*pager);
        CHECK_FALSE(fileSystem.exists(file));
        CHECK(fixture.notices().empty());
    }

    SECTION("a pager that ends at once closes its pane and releases its file")
    {
        // A `q` within the early-exit threshold, or a `more` over one screen of output, is how a pager is
        // used, not a shell that failed to start: no notice, and no pane left waiting for a key.
        REQUIRE((*acting)(contour::actions::OpenCommandOutput {}));
        auto const file = std::filesystem::path(factory->requestedCommandOverrides.back()->arguments.back());
        auto* pager =
            app.manager().sessionForId(vtworkspace::SessionId { acting->modelSessionId().value + 1 });
        REQUIRE(pager != nullptr);
        CHECK(pager->role() == contour::session::SessionRole::CommandOutput);
        auto closed = false;
        QObject::connect(pager, &contour::session::TerminalSession::sessionClosed, [&] { closed = true; });

        mockPtyOf(*pager).close();
        pager->onClosed();

        CHECK(closed);
        CHECK_FALSE(fileSystem.exists(file));
    }

    SECTION("a layout saved with a pager open does not record its private file")
    {
        QTemporaryDir const configDir;
        REQUIRE(configDir.isValid());
        auto const configDirectory = std::filesystem::path(configDir.path().toStdU16String());
        app.app().config().configFile = configDirectory / "contour.yml";

        REQUIRE((*acting)(
            contour::actions::OpenCommandOutput { .placement = contour::actions::OutputPlacement::Tab }));
        REQUIRE(win->count() == 2);
        REQUIRE(app.manager().saveWindowLayout(win.id, "with-pager"));

        auto const in = std::ifstream(configDirectory / "layouts.yml", std::ios::binary);
        auto contents = std::ostringstream {};
        contents << in.rdbuf();
        CHECK(contents.str().contains("with-pager:"));
        CHECK_FALSE(contents.str().contains(TestApp::OutputDirectory));
        CHECK_FALSE(contents.str().contains("less"));
    }

    SECTION("in a tab, as plain text")
    {
        REQUIRE((*acting)(
            contour::actions::OpenCommandOutput { .placement = contour::actions::OutputPlacement::Tab,
                                                  .format = contour::actions::OutputFormat::Plain }));
        CHECK(win->count() == 2);

        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(fileSystem.readFile(command->arguments.back()).value_or("") == "built");
    }

    SECTION("the binding's program wins over the configured pager")
    {
        REQUIRE((*acting)(contour::actions::OpenCommandOutput { .program = "most -s" }));

        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(command->program == "most -s");
        CHECK(command->arguments.size() == 1);
    }

    SECTION("a Windows path keeps its backslashes")
    {
        // Taken apart by POSIX quoting rules, the backslashes would be escapes, and the program
        // C:Toolsless.exe. The spawn reads the string by CreateProcess's rules on Windows instead.
        auto const pager = std::string { R"(C:\Tools\less.exe -R)" };
        REQUIRE((*acting)(contour::actions::OpenCommandOutput { .program = pager }));
        auto const& command = factory->requestedCommandOverrides.back();
        REQUIRE(command.has_value());
        CHECK(command->program == pager);

        REQUIRE((*acting)(contour::actions::OpenCommandOutput {
            .program = pager, .placement = contour::actions::OutputPlacement::Detached }));
        REQUIRE(app.launcher().piped.size() == 1);
        CHECK(app.launcher().piped[0].commandLine == pager);
    }

    SECTION("detached: fed to the program as plain text, with no pane, from a file released once it started")
    {
        auto const panesBefore = factory->requestedCommandOverrides.size();
        REQUIRE((*acting)(contour::actions::OpenCommandOutput {
            .program = "wl-copy --type text/plain",
            .placement = contour::actions::OutputPlacement::Detached }));

        REQUIRE(app.launcher().piped.size() == 1);
        auto const& piped = app.launcher().piped[0];
        CHECK(piped.commandLine == "wl-copy --type text/plain");
        CHECK(piped.inputFile.parent_path() == directory);
        CHECK(piped.input == "built");
        CHECK_FALSE(fileSystem.exists(piped.inputFile));
        CHECK(factory->requestedCommandOverrides.size() == panesBefore);
    }

    SECTION("a program that cannot be started is reported")
    {
        app.launcher().pipedError = contour::platform::SpawnError::NotFound;

        CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput {
            .program = "nope", .placement = contour::actions::OutputPlacement::Detached }));
        CHECK(fixture.notices().size() == 1);
        CHECK(filesLeft() == 0);
    }

    SECTION("an attached session opens no pane, and pipes the output all the same")
    {
        // Its panes run on the host, where a file written here does not exist.
        factory->reportedHosting = contour::session::SessionHosting::Remote;
        auto const panesBefore = factory->requestedCommandOverrides.size();

        CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput {}));
        CHECK_FALSE((*acting)(
            contour::actions::OpenCommandOutput { .placement = contour::actions::OutputPlacement::Tab }));
        CHECK(factory->requestedCommandOverrides.size() == panesBefore);
        CHECK(win->count() == 1);
        // Refused before anything was written: not even the private directory was made.
        CHECK_FALSE(fileSystem.exists(directory));
        REQUIRE(fixture.notices().size() == 2);
        CHECK(fixture.notices()[0].contains("placement: detached"));

        CHECK((*acting)(contour::actions::OpenCommandOutput {
            .program = "wl-copy", .placement = contour::actions::OutputPlacement::Detached }));
        REQUIRE(app.launcher().piped.size() == 1);
        CHECK(app.launcher().piped[0].input == "built");
    }

    SECTION("nothing to show opens nothing")
    {
        auto const panesBefore = factory->requestedCommandOverrides.size();
        CHECK_FALSE((*acting)(
            contour::actions::OpenCommandOutput { .target = vtbackend::CommandBlockTarget::Cursor }));
        CHECK(factory->requestedCommandOverrides.size() == panesBefore);
        CHECK(fixture.notices().empty());
    }
}

TEST_CASE("TerminalSession: OpenCommandOutput opens nothing for blank output",
          "[contour][session][actions][semanticblocks]")
{
    // A command that prints nothing leaves blank rows at most, so its output's text is empty: a pager over an
    // empty file shows nothing, and an empty pipe hands the program nothing. Both are refused quietly, as an
    // empty part is.
    auto fixture = PagerFixture { std::nullopt, "true", "\r\n\r\n" };
    auto& app = fixture.app;
    auto* const factory = fixture.factory;
    auto* const acting = fixture.acting;
    auto const output = acting->terminal().commandBlockText(vtbackend::CommandBlockTarget::Last,
                                                            vtbackend::CommandBlockPart::Output,
                                                            vtbackend::CaptureRendition::WithSgr);
    REQUIRE(output.has_value());
    REQUIRE(output->empty());

    auto const panesBefore = factory->requestedCommandOverrides.size();

    CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput {}));
    CHECK_FALSE((*acting)(contour::actions::OpenCommandOutput {
        .program = "wl-copy", .placement = contour::actions::OutputPlacement::Detached }));

    CHECK(factory->requestedCommandOverrides.size() == panesBefore);
    CHECK(app.launcher().piped.empty());
    CHECK_FALSE(app.fileSystem().exists(std::filesystem::path(TestApp::OutputDirectory)));
    CHECK(fixture.notices().empty());
}

TEST_CASE("TerminalSession: OpenCommandOutput without a pager says so",
          "[contour][session][actions][semanticblocks]")
{
    auto fixture = PagerFixture { "", "make", "built\r\n" };
    auto const panesBefore = fixture.factory->requestedCommandOverrides.size();

    CHECK_FALSE((*fixture.acting)(contour::actions::OpenCommandOutput {}));
    CHECK(fixture.factory->requestedCommandOverrides.size() == panesBefore);
    auto const notices = fixture.notices();
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].contains("command_blocks.pager"));
}

namespace
{
/// Presses Ctrl+Shift+G in @p session the way a Qt key event delivers it: as the character 'G'.
void pressCtrlShiftG(contour::session::TerminalSession& session)
{
    session.sendCharEvent(U'G',
                          vtbackend::KeyIdentity { .unshiftedKey = U'g' },
                          Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift },
                          KeyboardEventType::Press,
                          std::chrono::steady_clock::time_point {});
}

/// A finished `make` that printed "built", then a live prompt.
void finishedMake(contour::session::TerminalSession& session)
{
    writeFinishedBlock(session, "make", "built\r\n");
}
} // namespace

TEST_CASE("TerminalSession: Ctrl+Shift+G opens the last output in the pager",
          "[contour][session][input][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    finishedMake(*acting);
    auto const panesBefore = factory->requestedCommandOverrides.size();

    pressCtrlShiftG(*acting);

    CHECK(factory->requestedCommandOverrides.size() == panesBefore + 1);
}

TEST_CASE("TerminalSession: a user binding of Ctrl+Shift+G wins over the built-in one",
          "[contour][session][input][semanticblocks]")
{
    auto factoryOwned = std::make_unique<contour::test::MockPtySessionFactory>();
    auto* factory = factoryOwned.get();
    TestApp app { std::move(factoryOwned) };
    contour::test::ScopedController const win { app.manager() };

    // Before the session exists: a session takes its own copy of the configuration when it is made.
    app.app().config().inputMappings.value().charMappings.push_back(contour::config::CharInputMapping {
        .modes { vtbackend::MatchModes {} },
        .modifiers { vtbackend::Modifiers { vtbackend::Modifier::Control, vtbackend::Modifier::Shift } },
        .input = U'G',
        .binding = { { contour::actions::SelectAll {} } } });

    auto* acting = app.manager().createSession(win.id);
    REQUIRE(acting != nullptr);
    finishedMake(*acting);
    auto const panesBefore = factory->requestedCommandOverrides.size();

    pressCtrlShiftG(*acting);

    CHECK(factory->requestedCommandOverrides.size() == panesBefore);
    CHECK(acting->terminal().selectionAvailable());
}

TEST_CASE("TerminalSession: the context menu pins the command block that was right-clicked",
          "[contour][session][contextmenu][semanticblocks]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writeFinishedBlock(*session, "make", "built\r\n", ";cmdline_url=make");
    auto const pixels = vtbackend::PixelCoordinate {};

    SECTION("over a finished command's output")
    {
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(0) },
            pixels);
        auto const state = session->contextMenuState();

        auto const* make = session->terminal().commandBlockAt(vtbackend::LineOffset(0));
        REQUIRE(make != nullptr);
        REQUIRE(state.blockUnderCursor.has_value());
        CHECK(state.blockUnderCursor->id == make->id);
        CHECK(state.blockUnderCursor->output == contour::command::OutputPresence::Present);
        CHECK(state.blockUnderCursor->commandLine == contour::command::CommandLinePresence::Present);

        // The pinned row acts on that block even though the pointer has since moved away.
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(2), vtbackend::ColumnOffset(0) },
            pixels);
        CHECK((*session)(contour::actions::CopyCommandBlock {
            .target = vtbackend::CommandBlockTarget::Pointer, .block = state.blockUnderCursor->id }));
        CHECK(QGuiApplication::clipboard()->text().toStdString() == "built");
    }

    SECTION("over the live prompt, whose command is not known yet")
    {
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(2), vtbackend::ColumnOffset(0) },
            pixels);
        auto const state = session->contextMenuState();
        REQUIRE(state.blockUnderCursor.has_value());
        // Nothing has run at it: no output to act on, and no command to copy.
        CHECK(state.blockUnderCursor->output == contour::command::OutputPresence::Absent);
        CHECK(state.blockUnderCursor->commandLine == contour::command::CommandLinePresence::Absent);
    }

    SECTION("below every block")
    {
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(10), vtbackend::ColumnOffset(0) },
            pixels);
        CHECK_FALSE(session->contextMenuState().blockUnderCursor.has_value());
    }

    SECTION("over an alternate-screen application, which has no blocks")
    {
        session->terminal().writeToScreen("\033[?1049h");
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(0) },
            pixels);
        CHECK_FALSE(session->contextMenuState().blockUnderCursor.has_value());
    }

    QGuiApplication::clipboard()->clear();
}

TEST_CASE("TerminalSession: the context menu offers a line's fold and block only while the pointer is on it",
          "[contour][session][contextmenu][semanticblocks]")
{
    // The rows act on the line the pointer is on, and there is none while the pointer is on no cell: not the
    // top-left one before any pointer event, nor the one it left. A menu opened from the keyboard then
    // offers no line's fold or block.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writeFinishedBlock(*session, "make", "built\r\nlinked\r\n");
    auto const* make = session->terminal().commandBlockAt(vtbackend::LineOffset(0));
    REQUIRE(make != nullptr);
    REQUIRE(session->terminal().foldContaining(vtbackend::LineOffset(0)).has_value());
    auto const onTheOutput = vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(0) };

    SECTION("before any pointer event")
    {
        auto const state = session->contextMenuState();
        CHECK_FALSE(state.foldLine.has_value());
        CHECK_FALSE(state.blockUnderCursor.has_value());
    }

    SECTION("with the pointer on the block's output")
    {
        session->sendMouseMoveEvent(Modifiers {}, onTheOutput, vtbackend::PixelCoordinate {});
        auto const state = session->contextMenuState();
        CHECK(state.foldLine == vtbackend::LineOffset(1));
        REQUIRE(state.blockUnderCursor.has_value());
        CHECK(state.blockUnderCursor->id == make->id);
    }

    SECTION("once the pointer has left the grid")
    {
        session->sendMouseMoveEvent(Modifiers {}, onTheOutput, vtbackend::PixelCoordinate {});
        session->onPointerLeft();
        auto const state = session->contextMenuState();
        CHECK_FALSE(state.foldLine.has_value());
        CHECK_FALSE(state.blockUnderCursor.has_value());
    }
}

TEST_CASE("TerminalSession: a right-click on an output tail the next prompt shares pins the prompt's block",
          "[contour][session][contextmenu][semanticblocks]")
{
    // `printf hi` leaves the cursor after its output, so the next prompt is printed onto the same row. The
    // gutter shows that row as the prompt's block, and the menu acts on the block the gutter shows.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writeFinishedBlock(*session, "printf hi", "hi");

    session->sendMouseMoveEvent(
        Modifiers {},
        vtbackend::CellLocation { vtbackend::LineOffset(1), vtbackend::ColumnOffset(0) },
        vtbackend::PixelCoordinate {});
    auto const state = session->contextMenuState();

    auto const* finished = session->terminal().commandBlocks().lastFinished();
    auto const* prompt = session->terminal().commandBlocks().current();
    REQUIRE(finished != nullptr);
    REQUIRE(prompt != nullptr);
    REQUIRE(state.blockUnderCursor.has_value());
    CHECK(state.blockUnderCursor->id == prompt->id);
    CHECK(state.blockUnderCursor->id != finished->id);
    // The prompt has no output yet, so the menu grays the output rows rather than copy nothing.
    CHECK(state.blockUnderCursor->output == contour::command::OutputPresence::Absent);
}

// ============================================================================================
// Finish notifications
// ============================================================================================

namespace
{

using contour::config::FinishNotificationConfig;
using contour::config::NotifyAction;
using contour::config::NotifyClearOn;
using contour::config::NotifyOutcome;
using contour::config::NotifyWhen;
using contour::platform::finishNotificationIdentifier;

using namespace std::chrono_literals;

/// Runs one shell-integrated command through @p session's parser -- prompt, typed input, output and exit
/// status, as an OSC 133 shell reports them -- and delivers what that posted to the GUI thread.
void runAndDeliverShellCommand(contour::session::TerminalSession& session,
                               std::string_view commandLine,
                               int exitCode)
{
    session.terminal().writeToScreen(vtbackend::test::commandRun(commandLine, { "output" }, exitCode));
    QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall);
}

/// A headless pty factory that gives each session it makes a recording notifier, for the sessions a
/// manager makes itself.
/// @param recorders Receives each session's notifier, in the order the sessions are made.
/// @return The factory.
std::unique_ptr<contour::test::MockPtySessionFactory> makeRecordingFactory(
    std::vector<RecordingNotifier*>& recorders)
{
    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->makeNotifier = [&recorders]() {
        auto notifier = std::make_unique<RecordingNotifier>();
        recorders.push_back(notifier.get());
        return notifier;
    };
    return factory;
}

/// A display-less session whose profile notifies under a given policy, raising through a recording
/// notifier: what every finish-notice case below starts from. The manager's focus is given back before the
/// session it may point at goes.
struct FinishNoticeHarness
{
    TestApp testApp;
    RecordingNotifier* recorder; ///< The session's notifier.
    std::unique_ptr<contour::session::TerminalSession> session;

    /// @param policy The profile's `notify_on_command_finish`.
    explicit FinishNoticeHarness(FinishNotificationConfig const& policy):
        FinishNoticeHarness(policy, std::make_unique<RecordingNotifier>())
    {
    }

    ~FinishNoticeHarness() { testApp.manager().setFocusedSession(nullptr); }

    FinishNoticeHarness(FinishNoticeHarness const&) = delete;
    FinishNoticeHarness& operator=(FinishNoticeHarness const&) = delete;
    FinishNoticeHarness(FinishNoticeHarness&&) = delete;
    FinishNoticeHarness& operator=(FinishNoticeHarness&&) = delete;

    /// @return The identifier every finish notice of the session carries.
    [[nodiscard]] std::string identifier() const { return finishNotificationIdentifier(session->id()); }

    /// Delivers what the session posted to the GUI thread.
    void deliver() const { QCoreApplication::sendPostedEvents(session.get(), QEvent::MetaCall); }

    /// Writes @p bytes as the shell would, and delivers what that posted.
    /// @param bytes What the shell writes.
    void write(std::string_view bytes) const
    {
        session->terminal().writeToScreen(bytes);
        deliver();
    }

    /// Runs one shell-integrated command, and delivers what that posted.
    /// @param commandLine The command line typed at the prompt.
    /// @param exitCode The exit code its end reports.
    void run(std::string_view commandLine, int exitCode) const
    {
        runAndDeliverShellCommand(*session, commandLine, exitCode);
    }

    /// Focuses the session and takes focus away again. Platform-independent evidence: withdrawing goes
    /// through the notifier everywhere.
    /// @return How many notifications that withdrew.
    [[nodiscard]] size_t withdrawnByFocus()
    {
        auto const before = recorder->closed.size();
        testApp.manager().setFocusedSession(session.get());
        testApp.manager().setFocusedSession(nullptr);
        return recorder->closed.size() - before;
    }

  private:
    FinishNoticeHarness(FinishNotificationConfig const& policy, std::unique_ptr<RecordingNotifier> notifier):
        recorder { notifier.get() },
        session { makeSessionWith(
            testApp.app(),
            std::make_unique<vtpty::MockPty>(TestPageSize),
            std::move(notifier),
            registerProfile(testApp.app(), "finish-notify", [&](contour::config::TerminalProfile& profile) {
                profile.notifyOnCommandFinish = policy;
            })) }
    {
    }
};

} // namespace

TEST_CASE("TerminalSession: a finished command raises one finish notification",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s } };

    h.run("make", 2);

    // Through the session's notifier on every platform: off Linux, the one it is given by default hands
    // it to the window's tray.
    REQUIRE(h.recorder->raised.size() == 1);
    CHECK(h.recorder->raised[0].identifier == h.identifier());
    CHECK(h.recorder->raised[0].title == "✗ make failed (exit 2)");
    CHECK(h.recorder->raised[0].body.starts_with("took "));

    // The notice is outstanding, so focusing the pane withdraws exactly it.
    REQUIRE(h.withdrawnByFocus() == 1);
    CHECK(h.recorder->closed.back() == h.identifier());
}

TEST_CASE("TerminalSession: a command shorter than min_duration raises nothing",
          "[contour][session][notification]")
{
    // The default policy: ten seconds, which a command run straight through the parser never reaches.
    FinishNoticeHarness h { {} };

    h.run("make", 0);

    CHECK(h.recorder->raised.empty());
    CHECK(h.withdrawnByFocus() == 0);
}

TEST_CASE("TerminalSession: a command that ran for min_duration raises a notification",
          "[contour][session][notification]")
{
    // The duration is measured on the clocks the session's creator gave it, so the test's clock decides
    // it: ten seconds pass between the output start and the exit status.
    FinishNoticeHarness h { {} };

    h.write(vtbackend::test::commandStarted("make"));
    h.testApp.steadyClock().advance(10s);
    h.write(vtbackend::test::commandEnd(0));

    REQUIRE(h.recorder->raised.size() == 1);
    CHECK(h.recorder->raised[0].body.starts_with("took 10s"));
    CHECK(h.withdrawnByFocus() == 1);
}

TEST_CASE("TerminalSession: without shell integration nothing is ever notified",
          "[contour][session][notification]")
{
    // A shell without OSC 133 reports no command, so nothing finishes. tcsh's unconditional `D`,
    // arriving with no command behind it, is no finish either.
    FinishNoticeHarness h { { .when = NotifyWhen::Always, .minDuration = 0s } };

    h.write("$ make\r\nbuilding\r\n");
    h.write(vtbackend::test::commandEnd(1));

    CHECK(h.recorder->raised.empty());
    CHECK(h.withdrawnByFocus() == 0);
}

TEST_CASE("TerminalSession: a newer finish replaces the older notification",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s } };

    h.run("make", 0);
    h.run("make", 0);

    REQUIRE(h.recorder->raised.size() == 2);
    CHECK(h.recorder->raised[0].identifier == h.recorder->raised[1].identifier);

    // One notice outstanding, not two: the second took the first one's place.
    CHECK(h.withdrawnByFocus() == 1);
}

TEST_CASE("TerminalSession: the focused pane is not notified under `unfocused`",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s } };

    h.testApp.manager().setFocusedSession(h.session.get());
    h.run("make", 0);
    CHECK(h.recorder->raised.empty());

    h.testApp.manager().setFocusedSession(nullptr);
    CHECK(h.withdrawnByFocus() == 0);
}

TEST_CASE("TerminalSession: `outcome: failure` notifies only for a command that failed",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s, .outcome = NotifyOutcome::Failure } };

    h.run("make", 0);
    CHECK(h.withdrawnByFocus() == 0);

    h.run("make", 2);
    CHECK(h.withdrawnByFocus() == 1);
}

TEST_CASE("TerminalSession: `clear_on: never` survives a focus-in", "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s, .clearOn = NotifyClearOn::Never } };

    h.run("make", 0);

    CHECK(h.withdrawnByFocus() == 0);
}

TEST_CASE("TerminalSession: the desktop retiring a finish notification forgets it",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s } };

    h.run("make", 0);

    SECTION("a click retires it")
    {
        h.recorder->fireActivated(h.identifier());
        CHECK(h.withdrawnByFocus() == 0);
    }

    SECTION("a close retires it")
    {
        h.recorder->fireClosed(h.identifier(), /*reason*/ 2);
        CHECK(h.withdrawnByFocus() == 0);
    }

    SECTION("a close the backend only assumed leaves it")
    {
        // A timer running out is no evidence the notice left the desktop: it may still be withdrawn, and
        // clicked from the desktop's history.
        h.recorder->fireClosed(h.identifier(), /*reason*/ 1, vtbackend::CloseReport::Untracked);
        CHECK(h.withdrawnByFocus() == 1);
    }

    SECTION("another notification's events leave it alone")
    {
        h.recorder->fireActivated("someone-else");
        h.recorder->fireClosed("someone-else", /*reason*/ 2);
        CHECK(h.withdrawnByFocus() == 1);
    }
}

TEST_CASE("TerminalSession: closing a session takes its finish notice with it",
          "[contour][session][notification]")
{
    // Once the pane is gone, neither of the notice's clear_on events can happen any more.
    auto const clearOn = GENERATE(NotifyClearOn::Focus, NotifyClearOn::Next);
    FinishNoticeHarness h { { .minDuration = 0s, .clearOn = clearOn } };
    h.run("make", 2);
    REQUIRE(h.recorder->closed.empty());

    h.session->terminate();
    h.session->onClosed();

    CHECK(std::ranges::count(h.recorder->closed, h.identifier()) == 1);
    CHECK(h.withdrawnByFocus() == 0);
}

TEST_CASE("TerminalSession: closing a session leaves a clear_on: never notice to the desktop",
          "[contour][session][notification]")
{
    FinishNoticeHarness const h { { .minDuration = 0s, .clearOn = NotifyClearOn::Never } };
    h.run("make", 2);

    h.session->terminate();
    h.session->onClosed();

    CHECK(h.recorder->closed.empty());
}

TEST_CASE("TerminalSession: a click on a finish notice brings its session's tab forward",
          "[contour][session][notification][manager]")
{
    // Through the notifier each session is given, on every platform: the seam, whatever the platform puts
    // behind it.
    auto recorders = std::vector<RecordingNotifier*> {};
    TestApp testApp { makeRecordingFactory(recorders) };
    auto* const profile = testApp.app().config().profile(testApp.app().profileName());
    REQUIRE(profile != nullptr);
    profile->notifyOnCommandFinish = FinishNotificationConfig { .minDuration = 0s };

    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    REQUIRE(background != nullptr);
    REQUIRE(recorders.size() == 2);
    auto* const recorder = recorders.front();

    runAndDeliverShellCommand(*background, "make", 2);
    REQUIRE(recorder->raised.size() == 1);
    CHECK(recorder->raised[0].title == "✗ make failed (exit 2)");

    recorder->fireActivated(finishNotificationIdentifier(background->id()));
    CHECK(window->activeSession() == background);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: a `clear_on: focus` notice goes once its window brings the focused pane back",
          "[contour][session][notification][manager]")
{
    // The pane already holds focus, so no focus-in will ever come: a notice raised for it under `always`,
    // or while a window manager kept it focused through a minimise, waits for its window instead.
    auto recorders = std::vector<RecordingNotifier*> {};
    TestApp testApp { makeRecordingFactory(recorders) };
    auto& manager = testApp.manager();
    auto* const profile = testApp.app().config().profile(testApp.app().profileName());
    REQUIRE(profile != nullptr);
    profile->notifyOnCommandFinish =
        FinishNotificationConfig { .when = NotifyWhen::Always, .minDuration = 0s };

    contour::test::ScopedControllerWithOsWindow const window { manager };
    window->bindWindow(window.osWindow.get());
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    REQUIRE(recorders.size() == 1);
    auto* const recorder = recorders.front();
    manager.setFocusedWindow(window.id);
    REQUIRE(manager.sessionVisibility(*session) == contour::platform::SessionVisibility::Focused);

    SECTION("its window is activated again")
    {
        runAndDeliverShellCommand(*session, "make", 2);
        REQUIRE(recorder->raised.size() == 1);
        REQUIRE(recorder->closed.empty());

        manager.setFocusedWindow(window.id);
        CHECK(recorder->closed.size() == 1);
    }

    SECTION("its window is restored")
    {
        window.osWindow->setWindowStates(Qt::WindowMinimized);
        runAndDeliverShellCommand(*session, "make", 2);
        REQUIRE(recorder->raised.size() == 1);
        REQUIRE(recorder->closed.empty());

        window.osWindow->setWindowStates(Qt::WindowNoState);
        CHECK(recorder->closed.size() == 1);
    }

    SECTION("minimising its window is no look at it")
    {
        runAndDeliverShellCommand(*session, "make", 2);
        REQUIRE(recorder->raised.size() == 1);

        window.osWindow->setWindowStates(Qt::WindowMinimized);
        CHECK(recorder->closed.empty());
    }

    contour::test::closeAllTabs(*window);
}

#ifndef __linux__
TEST_CASE("TerminalSession: off Linux a background tab's notices go to the tray of the window hosting it",
          "[contour][session][notification][manager]")
{
    // No desktop to raise them on, and a background tab has no chrome on screen to show them: the
    // session's own notifier hands them to its window.
    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->makeNotifier = nullptr; // the platform's own: off Linux, the hosting window's tray
    TestApp testApp { std::move(factory) };
    auto* const profile = testApp.app().config().profile(testApp.app().profileName());
    REQUIRE(profile != nullptr);
    profile->notifyOnCommandFinish = FinishNotificationConfig { .minDuration = 0s };

    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    REQUIRE(background != nullptr);
    QSignalSpy const messages(window.controller, &contour::window::WindowController::notificationRequested);

    SECTION("a command's finish")
    {
        runAndDeliverShellCommand(*background, "make", 2);

        REQUIRE(messages.count() == 1);
        CHECK(messages.at(0).at(0).toString().toStdString() == "✗ make failed (exit 2)");
        CHECK(messages.at(0).at(1).toString().startsWith(QStringLiteral("took ")));

        // As a click on a desktop's notice does: the session's tab is brought forward.
        window->trayMessageClicked();
        CHECK(window->activeSession() == background);
    }

    SECTION("an application's notification")
    {
        background->terminal().writeToScreen("\033]9;Build done\033\\");
        QCoreApplication::sendPostedEvents(background, QEvent::MetaCall);

        REQUIRE(messages.count() == 1);
        CHECK(messages.at(0).at(0).toString() == QStringLiteral("Application Message: ConEmu"));
        CHECK(messages.at(0).at(1).toString() == QStringLiteral("Build done"));
    }

    contour::test::closeAllTabs(*window);
}
#endif

TEST_CASE("TerminalSession: an application cannot close or replace a finish notification",
          "[contour][session][notification]")
{
    // OSC 99 identifies an application's notifications through the session's own notifier, so the finish
    // notice's identifier must be one no `i=` can spell.
    FinishNoticeHarness h { { .minDuration = 0s } };
    auto const identifier = h.identifier();

    h.run("make", 2);
    auto const raisedBefore = h.recorder->raised.size();

    h.write(std::format("\033]99;i={}:p=close;\033\\", identifier));
    h.write(std::format("\033]99;i={};hijacked\033\\", identifier));

    CHECK(std::ranges::count(h.recorder->closed, identifier) == 0);
    CHECK(std::ranges::none_of(
        h.recorder->raised | std::views::drop(raisedBefore),
        [&](vtbackend::DesktopNotification const& raised) { return raised.identifier == identifier; }));

    // The session's notice is still up, so focusing the pane withdraws it.
    CHECK(h.withdrawnByFocus() == 1);
}

TEST_CASE("TerminalSession: a notification's close is answered under the terminal's lock",
          "[contour][session][notification][threading]")
{
    // The parser thread lists a notification as it arrives, under the terminal's lock. The desktop's word
    // that it closed comes on the GUI thread, and strikes it off that list and answers the application:
    // under the same lock, or the two threads change the list at once.
    FinishNoticeHarness h { {} };
    auto const isLive = [&h]() {
        return std::ranges::contains(h.session->terminal().desktopNotificationManager().activeNotifications(),
                                     std::string { "build" });
    };
    h.write("\033]99;i=build:c=1;Build done\033\\");
    REQUIRE(h.recorder->raised.size() == 1);
    REQUIRE(isLive());

    CHECK(contour::test::runWhileHeld(
              h.session->terminal(), [&]() { h.recorder->fireClosed("build", /*reason*/ 2); }, 100ms)
          == contour::test::UnderLock::Waited);
    CHECK_FALSE(isLive());
}

TEST_CASE("TerminalSession: a notification's click is answered under the terminal's lock",
          "[contour][session][notification][threading]")
{
    // The answer is phrased by state the parser thread changes under that lock -- 7-bit or 8-bit controls.
    FinishNoticeHarness h { {} };
    h.write("\033]99;i=build:a=report;Build done\033\\");
    REQUIRE(h.recorder->raised.size() == 1);

    CHECK(contour::test::runWhileHeld(
              h.session->terminal(), [&]() { h.recorder->fireActivated("build"); }, 100ms)
          == contour::test::UnderLock::Waited);
}

TEST_CASE("TerminalSession: an application's notification is raised by where its pane is",
          "[contour][session][notification]")
{
    // OSC 777, OSC 9 and OSC 99 are decided by the finish notice's rule: OSC 99 states when it wants to be
    // raised with `o=`, and OSC 777 and OSC 9, which cannot, are raised unless the pane has focus.
    FinishNoticeHarness h { {} };

    SECTION("OSC 777 and OSC 9 are raised from a pane without focus")
    {
        h.write("\033]777;notify;Build;done\033\\");
        h.write("\033]9;Build done\033\\");
        CHECK(h.recorder->raised.size() == 2);
    }

    SECTION("OSC 777 is not raised from the focused pane")
    {
        h.testApp.manager().setFocusedSession(h.session.get());
        h.write("\033]777;notify;Build;done\033\\");
        CHECK(h.recorder->raised.empty());
    }

    SECTION("OSC 9 is not raised from the focused pane")
    {
        h.testApp.manager().setFocusedSession(h.session.get());
        h.write("\033]9;Build done\033\\");
        CHECK(h.recorder->raised.empty());
    }

    SECTION("OSC 99 says for itself, and only what was raised is live")
    {
        h.testApp.manager().setFocusedSession(h.session.get());
        h.write("\033]99;i=always:o=always;Build done\033\\");
        h.write("\033]99;i=unfocused:o=unfocused;Build done\033\\");
        CHECK(h.recorder->raised.size() == 1);

        auto& pty = contour::test::mockPtyOf(*h.session);
        pty.stdinBuffer().clear();
        h.write("\033]99;i=query:p=alive;\033\\");
        h.session->terminal().flushInput();
        CHECK(pty.stdinBuffer() == "\033]99;i=query:p=alive;always\033\\");
    }
}

TEST_CASE(
    "TerminalSession: an application's notification from a background tab reaches the session's notifier",
    "[contour][session][notification][manager]")
{
    // A background tab has no chrome on screen to pass it on, so it goes where a finish notice goes: to the
    // notifier the session was given, whatever the platform puts behind it.
    auto recorders = std::vector<RecordingNotifier*> {};
    TestApp testApp { makeRecordingFactory(recorders) };
    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    REQUIRE(background != nullptr);
    REQUIRE(testApp.manager().sessionVisibility(*background) == contour::platform::SessionVisibility::Hidden);
    REQUIRE(recorders.size() == 2);
    auto const* const recorder = recorders.front();

    background->terminal().writeToScreen("\033]9;Build done\033\\");
    QCoreApplication::sendPostedEvents(background, QEvent::MetaCall);

    REQUIRE(recorder->raised.size() == 1);
    CHECK(recorder->raised[0].body == "Build done");

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: an OSC 99 notification asked for when out of sight is not raised on screen",
          "[contour][session][notification][manager]")
{
    // kitty's `o=invisible`: raised only while the pane is both unfocused and off screen. A pane in its
    // window's shown tab is in sight, focus or not.
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    testApp.manager().clearFocusedWindow(window.id); // the user is in another application
    REQUIRE(testApp.manager().sessionVisibility(*session)
            == contour::platform::SessionVisibility::VisibleUnfocused);

    // Announced exactly when raised, on every platform.
    auto announcer = std::make_unique<RecordingAnnouncer>();
    auto const* const announced = announcer.get();
    session->setAnnouncer(std::move(announcer));

    session->terminal().writeToScreen("\033]99;i=later:o=invisible;Build done\033\\");
    QCoreApplication::sendPostedEvents(session, QEvent::MetaCall);
    CHECK(announced->said.empty());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: a notification asking for focus brings its session forward",
          "[contour][session][notification][manager]")
{
    // What a click on an OSC 99 notification asks for (`a=focus`, the default): the session's tab selected
    // and its window raised, not merely a window it may not even have a pane on screen in.
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    REQUIRE(background != nullptr);
    REQUIRE(window->activeSession() != background);

    background->focusTerminalWindow();
    QCoreApplication::sendPostedEvents(background, QEvent::MetaCall);

    CHECK(window->activeSession() == background);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: a finish on the parser thread is decided on the GUI thread",
          "[contour][session][notification][threading]")
{
    // The finish arrives on the thread writing the output, with the terminal's state mutex held. Whether
    // to notify is asked of the session manager, and the outstanding notice is the GUI thread's: the
    // writer only posts the decision, which runs when the GUI thread delivers it.
    FinishNoticeHarness h { { .minDuration = 0s } };

    std::thread { [&] {
        h.session->terminal().writeToScreen(vtbackend::test::commandRun("make", { "output" }, 2));
    } }.join();
    CHECK(h.recorder->raised.empty());
    CHECK(h.withdrawnByFocus() == 0);

    h.deliver();
    CHECK(h.withdrawnByFocus() == 1);
}

TEST_CASE("TerminalSession: a finish the policy rejects is decided without the tab's title",
          "[contour][session][notification][threading]")
{
    // A split tab's title is its active pane's, read under that pane's terminal lock, which its parser
    // holds across a whole write. A command in the other pane that the floor rejects -- nearly every
    // command -- must not wait for it.
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = testApp.manager();
    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical=*/true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != nullptr);
    REQUIRE(right != left);

    // Below the default ten-second floor.
    left->terminal().writeToScreen(vtbackend::test::commandRun("make", { "output" }, 0));

    CHECK(contour::test::runWhileHeld(right->terminal(),
                                      [&]() { QCoreApplication::sendPostedEvents(left, QEvent::MetaCall); })
          == contour::test::UnderLock::Finished);

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: a re-attach's snapshot of a finished command raises no notification",
          "[contour][session][notification][daemon]")
{
    // A daemon client adopts the host's records: the snapshot taken at attach is history, and only a
    // live delta's first reported finish is news.
    FinishNoticeHarness h { { .minDuration = 0s } };

    auto record = vtbackend::CommandBlockRecord {};
    record.id = vtbackend::CommandBlockId { 7 };
    record.state = vtbackend::CommandBlockState::Finished;
    record.end = vtbackend::CommandBlockEnd::Reported;
    record.commandLine = vtbackend::CommandLineText { "make" };
    record.exitCode = 2;
    record.commandStartedAt = h.testApp.wallClock().now();
    record.duration = 1min;
    auto const adopt = [&](vtbackend::AdoptMode mode) {
        core::locked(h.session->terminal(), [&]() { h.session->terminal().adoptCommandBlock(record, mode); });
        h.deliver();
    };

    SECTION("the attach snapshot is history")
    {
        adopt(vtbackend::AdoptMode::Snapshot);
        CHECK(h.recorder->raised.empty());
        CHECK(h.withdrawnByFocus() == 0);
    }

    SECTION("a live delta is news")
    {
        adopt(vtbackend::AdoptMode::Live);
        CHECK(h.withdrawnByFocus() == 1);
    }
}

TEST_CASE("TerminalSession: `clear_on: next` withdraws the notification when the next command starts",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s, .clearOn = NotifyClearOn::Next } };

    h.run("make", 0);
    REQUIRE(h.recorder->closed.empty());

    SECTION("a new prompt alone is not a start")
    {
        h.write(vtbackend::test::PromptStart);
        CHECK(h.recorder->closed.empty());
    }

    SECTION("a change to the notified block itself is not a start")
    {
        // The next prompt records on the notified block where the command left the shell.
        h.write("\033]7;file:///tmp\033\\" + std::string { vtbackend::test::PromptStart });
        CHECK(h.recorder->closed.empty());
    }

    SECTION("the next command's output start withdraws it")
    {
        h.write(vtbackend::test::commandStarted("make test"));
        REQUIRE(h.recorder->closed.size() == 1);
        CHECK(h.recorder->closed[0] == h.identifier());
    }

    SECTION("a next command that comes and goes within one read withdraws it once")
    {
        h.write(vtbackend::test::commandRun("true", {}, 0));
        // Withdrawn once; `true` then raised its own notice in the old one's place.
        CHECK(h.recorder->closed.size() == 1);
    }

    SECTION("a next command that a newer prompt closes within one read withdraws it")
    {
        // A shell that reports no end for it, or an `ssh` the user is still inside: the newer prompt
        // finishes it all the same, and announces no finish.
        h.write(vtbackend::test::commandStarted("ssh host") + std::string { vtbackend::test::PromptStart });
        REQUIRE(h.recorder->closed.size() == 1);
        CHECK(h.recorder->closed[0] == h.identifier());
    }

    SECTION("a next command that ends before a reset within one read withdraws it")
    {
        // The reset forgets every record, so by the time the GUI thread looks, only the finish it
        // announced still tells that a command ran.
        h.write(vtbackend::test::commandRun("true", {}, 0) + "\033c");
        CHECK(h.recorder->closed.size() == 1);
    }

    SECTION("a reset as the next command withdraws it")
    {
        // `reset` forgets every record before the GUI thread looks, and its end then finds nothing running,
        // so no finish is announced: only the notified block's absence tells that a later command started.
        h.write(vtbackend::test::commandStarted("reset") + "\033c" + vtbackend::test::commandEnd(0)
                + std::string { vtbackend::test::PromptStart });
        REQUIRE(h.recorder->closed.size() == 1);
        CHECK(h.recorder->closed[0] == h.identifier());
    }

    SECTION("focus does not withdraw it")
    {
        CHECK(h.withdrawnByFocus() == 0);
    }
}

TEST_CASE("TerminalSession: a flood of command cycles on one row leaves the parser no re-heading to do",
          "[contour][session][notification]")
{
    // `\e]133;A\e\\\e]133;C\e\\\e]133;D;0\e\\` over and over with no newline: each prompt takes the one row
    // of the block before, and only a walk of the whole history tells that block has no row left. Every cycle
    // changes records, and a `next` notice is up -- yet what the session needs travels with each change, so
    // no event asks the store for anything, and the walk is left to whoever reads the store next, once.
    FinishNoticeHarness const h { { .minDuration = 0s, .clearOn = NotifyClearOn::Next } };
    auto& terminal = h.session->terminal();
    auto history = std::string {};
    for (auto const line: std::views::iota(0, 600))
        history += std::format("history {}\r\n", line);
    h.write(history);
    h.run("make", 0);
    auto const rows = unbox<uint64_t>(terminal.primaryScreen().historyLineCount())
                      + unbox<uint64_t>(terminal.pageSize().lines);
    REQUIRE(rows > 600);
    std::ignore = terminal.commandBlocks(); // settled before the flood
    auto const settled = terminal.commandBlockHeadWork();

    auto flood = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 200))
        flood += "\033]133;A\033\\\033]133;C\033\\\033]133;D;0\033\\";
    terminal.writeToScreen(flood);

    // Less than a single walk for all 200 cycles: none of their events walked.
    CHECK(terminal.commandBlockHeadWork() - settled < rows);
    h.deliver();
    CHECK_FALSE(h.recorder->closed.empty()); // and the next command did withdraw the notice
}

namespace
{

/// Counts the queued calls delivered to the object it filters.
class QueuedCallCounter final: public QObject
{
  public:
    int count = 0; ///< The calls delivered so far.

    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (event->type() == QEvent::MetaCall)
            ++count;
        return QObject::eventFilter(watched, event);
    }
};

} // namespace

TEST_CASE("TerminalSession: a flood of finished commands queues the GUI thread a bounded number of calls",
          "[contour][session][notification]")
{
    // A `cat` of a log replays its OSC 133 cycles, each a change and a finish. Queued a call apiece, a GUI
    // thread slower than the parser holds a call, and a copy of the summary, per cycle; gathered, the queue
    // holds one call for all of them, and the summaries are bounded by the records.
    FinishNoticeHarness const h { { .minDuration = 0s } };
    QueuedCallCounter counter;
    h.session->installEventFilter(&counter);

    auto flood = std::string {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, 200))
        flood += vtbackend::test::commandRun("true", {}, 0);
    flood += vtbackend::test::commandRun("make", {}, 2);
    h.session->terminal().writeToScreen(flood);
    h.deliver();
    h.session->removeEventFilter(&counter);

    CHECK(counter.count <= 3);
    // Every finish is still announced, in order.
    REQUIRE(h.recorder->raised.size() == 201);
    CHECK(h.recorder->raised.front().title == "✓ true finished");
    CHECK(h.recorder->raised.back().title == "✗ make failed (exit 2)");
}

TEST_CASE("TerminalSession: a next command starting on the parser thread is looked for on the GUI thread",
          "[contour][session][notification][threading]")
{
    // The start arrives on the thread writing the output, with the terminal's state mutex held. The notice
    // is the GUI thread's, and so is the look at the record store that tells a later command started: the
    // writer only posts it.
    FinishNoticeHarness h { { .minDuration = 0s, .clearOn = NotifyClearOn::Next } };
    h.run("make", 0);

    std::thread { [&] {
        h.session->terminal().writeToScreen(vtbackend::test::commandStarted("ls"));
    } }.join();
    CHECK(h.recorder->closed.empty());

    h.deliver();
    CHECK(h.recorder->closed.size() == 1);
}

TEST_CASE("TerminalSession: a next command's start is looked for without the terminal's lock",
          "[contour][session][notification][threading]")
{
    // While a `next` notice is up, every record change asks whether a later command started. The parser
    // holds the terminal across a whole write, so asking under the lock on the GUI thread would wait for
    // it, once per change of a flood of them.
    FinishNoticeHarness h { { .minDuration = 0s, .clearOn = NotifyClearOn::Next } };
    h.run("make", 0);
    h.session->terminal().writeToScreen(vtbackend::test::commandStarted("ls"));

    CHECK(contour::test::runWhileHeld(h.session->terminal(), [&]() { h.deliver(); })
          == contour::test::UnderLock::Finished);
    CHECK(h.recorder->closed.size() == 1);
}

TEST_CASE("TerminalSession: a finish rings the window bell only where the policy's visibility allows",
          "[contour][session][notification][manager]")
{
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    auto& manager = testApp.manager();

    // Every session below is created under the app's default profile, which a session copies at birth.
    auto* const profile = testApp.app().config().profile(testApp.app().profileName());
    REQUIRE(profile != nullptr);
    profile->notifyOnCommandFinish = FinishNotificationConfig { .when = NotifyWhen::Hidden,
                                                                .minDuration = 0s,
                                                                .action = NotifyAction::Bell };

    contour::test::ScopedController const window { manager };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    auto* const left = window->activeSession();
    REQUIRE(background != nullptr);
    REQUIRE(left != nullptr);
    manager.splitActivePane(/*vertical=*/true, left);
    auto* const right = window->activeSession();
    REQUIRE(right != nullptr);
    REQUIRE(right != left);

    QSignalSpy const bells(window.controller, &contour::window::WindowController::bellRequested);
    QSignalSpy const alerts(window.controller, &contour::window::WindowController::alertRequested);

    SECTION("an unfocused split pane is on screen, so `hidden` stays quiet")
    {
        runAndDeliverShellCommand(*left, "make", 0);
        CHECK(bells.isEmpty());
    }

    SECTION("a pane in a background tab is hidden, so it rings through its window")
    {
        runAndDeliverShellCommand(*background, "make", 0);
        REQUIRE(bells.count() == 1);
        CHECK(bells.at(0).at(0).toString() == QStringLiteral("qrc:/contour/bell.wav"));
        CHECK(bells.at(0).at(1).toDouble() == 1.0);
        CHECK(alerts.count() == 1); // the default profile's bell alerts
    }

    SECTION("zooming the focused pane hides its sibling")
    {
        manager.toggleActivePaneZoom(right);
        runAndDeliverShellCommand(*left, "make", 0);
        CHECK(bells.count() == 1);
    }

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: the default scrollbar is on the right, and hides over a full-screen program",
          "[contour][session][scrollbar-marks]")
{
    // What SessionChrome.qml binds the bar's edge and visibility to (isScrollbarRight, isScrollbarVisible),
    // for a session on the default profile.
    TestApp testApp;
    auto held = makeSessionWithSurface(testApp.app());

    CHECK(held->getIsScrollbarRight());
    CHECK(held->getIsScrollbarVisible());

    // hide_in_alt_screen keeps its default, so vim or htop keep their last column.
    held->terminal().writeToScreen("\033[?1049h");
    CHECK_FALSE(held->getIsScrollbarVisible());

    held->terminal().writeToScreen("\033[?1049l");
    CHECK(held->getIsScrollbarVisible());
}

namespace
{

using vtbackend::test::failOnDeadlock;

/// Turns local echo on (SRM reset), so that whatever the terminal sends is echoed, which parses. Clears what
/// doing so wrote to the PTY.
/// @param session The session.
void echoLocally(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033[12l");
    session.terminal().flushInput();
    mockPtyOf(session).stdinBuffer().clear();
}

/// Turns local echo on and in-band resize reports with it (DECSET 2048), so that a resize writes a report and
/// the report is echoed, which parses. Clears what doing so wrote to the PTY.
/// @param session The session.
void echoResizeReports(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033[?2048h");
    echoLocally(session);
}

/// Whether what @p session wrote to its PTY opens with the in-band resize report of the page it has now.
/// @param session The session.
/// @return Whether the report leads.
[[nodiscard]] bool reportedItsPage(contour::session::TerminalSession& session)
{
    auto const page = session.terminal().pageSize();
    return mockPtyOf(session).stdinBuffer().starts_with(
        std::format("\033[48;{};{};", page.lines.value, page.columns.value));
}

} // namespace

TEST_CASE("TerminalSession: Normal mode with resize reports echoed locally does not deadlock",
          "[contour][session][vi]")
{
    // Entering Normal mode puts up the indicator status line, and the resize that follows reports the page.
    // The action holds the terminal lock throughout, and with SRM reset the report is echoed, which parses
    // and takes that lock again: the report goes out once the lock is released.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    session->terminal().setStatusDisplay(vtbackend::StatusDisplayType::None);
    echoResizeReports(*session);

    failOnDeadlock(std::chrono::seconds(10), [&] { (*session)(contour::actions::ViNormalMode {}); });

    // The report names the page the status line left.
    CHECK(session->terminal().inputHandler().mode() == vtbackend::ViMode::Normal);
    CHECK(reportedItsPage(*session));
}

TEST_CASE("TerminalSession: attaching a display with resize reports echoed locally does not deadlock",
          "[contour][session][view]")
{
    // Attaching resizes the page to the display's pixels, and the resize reports the page: as for Normal
    // mode, the report goes out once the lock the attach holds is released.
    TestApp testApp;
    auto pane = SessionWithSurface { makeDisplaylessSession(testApp.app()),
                                     std::make_unique<contour::test::FakeDisplaySurface>() };
    pane.surface->attachedSession = pane.session.get();
    echoResizeReports(*pane);

    failOnDeadlock(std::chrono::seconds(10), [&] { pane->attachDisplay(*pane.surface); });

    CHECK(pane->display() == pane.surface.get());
    CHECK(mockPtyOf(*pane).stdinBuffer().starts_with("\033[48;"));
}

TEST_CASE("TerminalSession: what an opener opens is opened once the terminal lock is free",
          "[contour][session][hyperlink][vi][threading]")
{
    // The desktop's URL handler is another process, and an executable link runs one: neither may hold up the
    // parser thread. So each opener reads what to open under the terminal lock and opens it once that lock is
    // released. At each launch, another thread takes the lock as the parser thread would.
    namespace actions = contour::actions;
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto& terminal = session->terminal();
    auto& launcher = testApp.launcher();

    auto atLaunch = vtbackend::test::LockProbe {};
    launcher.onLaunch = [&] {
        atLaunch.probe(terminal);
    };

    // A link under the pointer.
    seedHoveredHyperlink(*session, "https://example.com/page");
    CHECK((*session)(actions::FollowHyperlink {}));
    CHECK(atLaunch.lockWasFree());

    // A path under the pointer, naming a file that exists here.
    testApp.fileSystem().addFile(inWorkDirectory("doc.txt"), "x");
    terminal.setCurrentWorkingDirectory("file://" + std::string { WorkDirectory });
    terminal.writeToScreen("\r\nopen doc.txt now");
    terminal.sendMouseMoveEvent(
        Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(1), .column = vtbackend::ColumnOffset(6) },
        vtbackend::PixelCoordinate {},
        /*uiHandledHint=*/false);
    REQUIRE(vtbackend::test::printedPathUnderPointer(terminal) == "doc.txt");
    CHECK((*session)(actions::FollowHyperlink {}));
    // Opened once the probe has answered, back on this thread.
    testApp.drainLinkProbes();
    QCoreApplication::processEvents();
    CHECK(atLaunch.lockWasFree());

    // The selection.
    terminal.selectAll();
    CHECK((*session)(actions::OpenSelection {}));
    CHECK(atLaunch.lockWasFree());

    // `oo` in Normal mode: the line the vi cursor is on.
    CHECK((*session)(actions::ViNormalMode {}));
    typeKeys(*session, "oo");
    CHECK(atLaunch.lockWasFree());

    CHECK(launcher.openedUrls.size() == 4);
}

TEST_CASE("TerminalSession: the modes a key binding matches on are read without racing the parser thread",
          "[contour][session][input][threading]")
{
    // A binding can ask for the screen, the cursor keys and keypad modes (DECCKM, DECNKM), a selection and a
    // search, and the parser thread changes each of them under the terminal lock: output switches the screen
    // and the modes, scrolls a selection away, and ends a hint session along with its search. A key reads
    // them without that lock, so as not to wait for a parse. The case to run under ThreadSanitizer, which
    // reports each read against its write unless the modes are read race-free.
    //
    // The keyboard is locked (KAM), so the keys go no further than the bindings: sending them would take the
    // input lock the parser thread takes too, which orders the two threads' accesses as it happens.
    struct Row
    {
        std::string_view mode;
        std::function<void(vtbackend::Terminal&)> change;
    };
    auto const rows = std::array {
        Row { .mode = "DECCKM",
              .change = [](auto& terminal) { terminal.writeToScreen("\033[?1h\033[?1l"); } },
        Row { .mode = "DECNKM", .change = [](auto& terminal) { terminal.writeToScreen("\033=\033>"); } },
        Row { .mode = "alternate screen",
              .change = [](auto& terminal) { terminal.writeToScreen("\033[?1049h\033[?1049l"); } },
        Row { .mode = "selection",
              .change =
                  [](auto& terminal) {
                      auto const _ = std::scoped_lock { terminal };
                      terminal.selectAll();
                      terminal.clearSelection();
                  } },
        Row { .mode = "search",
              .change =
                  [](auto& terminal) {
                      auto const _ = std::scoped_lock { terminal };
                      (void) terminal.setNewSearchTerm(U"x", vtbackend::SearchOrigin::Typed);
                      terminal.clearSearch();
                  } },
    };

    TestApp testApp;
    for (auto const& row: rows)
    {
        INFO(row.mode);
        auto session = makeDisplaylessSession(testApp.app());
        auto& terminal = session->terminal();
        terminal.writeToScreen("\033[2h");
        REQUIRE_FALSE(terminal.allowInput());
        mockPtyOf(*session).stdinBuffer().clear();

        {
            auto const writer = vtbackend::test::writeOnOwnThread([&] { row.change(terminal); });
            auto constexpr KeyCount = 100;
            for ([[maybe_unused]] auto const key: std::views::iota(0, KeyCount))
                typeKeys(*session, "a");
        }

        CHECK(mockPtyOf(*session).stdinBuffer().empty());
    }
}

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

    failOnDeadlock(std::chrono::seconds(10),
                   [&] { session->executeShowHostWritableStatusLine(/*allow=*/true, /*remember=*/false); });

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
    session->executeShowHostWritableStatusLine(/*allow=*/true, /*remember=*/false);
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

} // namespace

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
        Row { .site = "cycleSearchCaseSensitivity",
              .prepare =
                  [](auto& s) {
                      enterVisualMode(s);
                      s.setSearchPattern(QStringLiteral("line"));
                  },
              .act = [](auto& s) { s.cycleSearchCaseSensitivity(); } },
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

namespace
{
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
} // namespace

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

    accessibility.set(contour::config::AccessibilityPreferences {
        .motion = MotionPreference::Full, .transparency = {}, .contrast = {} });
    CHECK(settings.screenTransitionDuration == 250ms);
    CHECK(settings.cursorMotionAnimationDuration == 80ms);
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
