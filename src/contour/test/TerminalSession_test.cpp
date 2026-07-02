// SPDX-License-Identifier: Apache-2.0
//
// Integration tests for TerminalSession that exercise the *real* class (not a Qt-free surrogate).
//
// Unlike the model-layer tests (TabListModel_test / vtmux SessionModel_test), these construct an
// actual TerminalSession around a MockPty and a test-configured ContourGuiApp. That is only possible
// because the contour frontend is built as the `contour_core` object library the test links against,
// and because crispy::app exposes parseParametersForTesting() to populate parameters() without
// launching the GUI event loop.
//
// The headline case is the regression behind the "close leaks background tabs" finding:
// TerminalSession::terminate() must close the PTY device even when NO display is attached (a
// background tab/split pane whose display was detached on the last tab switch). Before the fix
// terminate() early-returned for a display-less session, so the device stayed open and the session —
// plus its shell process — leaked.

#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>

#include <vtpty/MockPty.h>

#include <catch2/catch_test_macros.hpp>

#include <memory>

using namespace std::string_literals;

namespace
{

/// Builds a ContourGuiApp whose parameters() are populated with defaults (so profileName() resolves
/// to the default "main" profile) without running the GUI. The default-constructed config already
/// seeds a "main" profile and a "default" colorscheme, so no config file is needed.
class TestApp
{
  public:
    TestApp()
    {
        char const* argv[] = { "contour", "terminal" };
        // Parse the "terminal" subcommand so parameters() carries every contour.terminal.* default
        // (profile, dump-state-at-exit, ...) that TerminalSession reads during construction.
        REQUIRE(_app.parseParametersForTesting(2, argv));
    }

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _app; }

  private:
    contour::ContourGuiApp _app;
};

/// Creates a TerminalSession backed by a MockPty, with NO display attached (the default state).
/// The session owns the PTY; the caller owns the session.
[[nodiscard]] std::unique_ptr<contour::TerminalSession> makeDisplaylessSession(contour::ContourGuiApp& app)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    // Pass the app's real session manager so the sessionClosed->removeSession wiring matches
    // production; we never pump the Qt event loop here, so that slot does not actually fire and the
    // test stays deterministic.
    return std::make_unique<contour::TerminalSession>(&app.sessionsManager(), std::move(pty), app);
}

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

TEST_CASE("TerminalSession::workingDirectory falls back to \".\" for a non-process device",
          "[contour][session][cwd]")
{
    // workingDirectory() is the single cwd-inheritance accessor shared by every spawn path (new tab,
    // new window, split pane). For a device that is not a local process (here a MockPty; in production
    // an SSH session on non-Windows) it must return the "." fallback rather than crash on the failed
    // dynamic_cast — this is what split panes now use so they inherit a cwd on every platform.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    CHECK(session->workingDirectory() == ".");
}
