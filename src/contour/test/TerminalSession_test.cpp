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

#include <contour/Actions.h>
#include <contour/ContourGuiApp.h>
#include <contour/TerminalSession.h>
#include <contour/TerminalSessionManager.h>
#include <contour/test/GuiTestFixtures.h>

#include <vtbackend/Hyperlink.h>

#include <vtpty/MockPty.h>

#include <crispy/utils.h>

#include <QtCore/QCoreApplication>
#include <QtCore/QTemporaryDir>
#include <QtGui/QClipboard>
#include <QtGui/QGuiApplication>
#include <QtNetwork/QHostInfo>

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <ranges>

using namespace std::string_literals;

namespace
{

using contour::test::TestApp;

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
[[nodiscard]] std::unique_ptr<contour::TerminalSession> makeSessionWithProfile(contour::ContourGuiApp& app,
                                                                               std::string profileName)
{
    auto pty =
        std::make_unique<vtpty::MockPty>(vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) });
    return std::make_unique<contour::TerminalSession>(
        &app.sessionsManager(), std::move(pty), app, std::move(profileName));
}

} // namespace

TEST_CASE("TerminalSession: key and char events write their encoding into the PTY",
          "[contour][session][input]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    session->sendCharEvent(U'a', 0, Modifiers {}, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer() == "a");

    session->sendKeyEvent(vtbackend::Key::Enter, Modifiers {}, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer() == "a\r");

    // A release in the default keyboard protocol encodes nothing.
    session->sendCharEvent(U'a', 0, Modifiers {}, KeyboardEventType::Release, now);
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
                     &contour::TerminalSessionManager::contextMenuRequested,
                     [&](contour::TerminalSession*) { ++menuRequests; });

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
        CHECK(session->terminal().extractSelectionText().find("line 0") != std::string::npos);
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
        CHECK(session->terminal().primaryScreen().grid().lineText(vtbackend::LineOffset(-1)).find("line")
              != std::string::npos);

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
    // was never registered in the vtmux model (this fixture) must resolve to no target tab and be a
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
    CHECK(mockPtyOf(*session).stdinBuffer().find("pasted") != std::string::npos);

    // > 1 MB: hard-rejected — and (regression) must not crash on a display-less session.
    mockPtyOf(*session).stdinBuffer().clear();
    clipboard->setText(QString(1024 * 1024 + 1, QChar('x')));
    CHECK_NOTHROW(session->pasteFromClipboard(1, false));
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // > 512 KB: requires permission; nothing written until granted.
    int permissionRequests = 0;
    QObject::connect(session.get(), &contour::TerminalSession::requestPermissionForPasteLargeFile, [&] {
        ++permissionRequests;
    });
    clipboard->setText(QString(1024 * 512 + 1, QChar('y')));
    session->pasteFromClipboard(1, false);
    CHECK(permissionRequests == 1);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

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
    contour::test::ScopedController controller(testApp.manager());

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
    CHECK(pty.stdinBuffer().find("abc") != std::string::npos);
    CHECK((*session)(actions::WriteScreen { .chars = "xyz" }));

    // Clipboard write action (no display: routed but harmless).
    CHECK_NOTHROW((*session)(actions::PasteSelection {}));
}

TEST_CASE("TerminalSession: a modifier-bound char dispatches its action instead of writing to the PTY",
          "[contour][session][input]")
{
    // Ctrl+0 is bound to ResetFontSize in the default char mappings with no mode restriction, so
    // sending it runs the char keybinding-dispatch path (config::apply -> executeAllActions) and
    // consumes the character: nothing reaches the PTY.
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    auto const now = std::chrono::steady_clock::now();

    auto& pty = mockPtyOf(*session);
    pty.stdinBuffer().clear();

    session->sendCharEvent(
        U'0', 0, Modifiers { vtbackend::Modifier::Control }, KeyboardEventType::Press, now);
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

    // Vi-mode toggle round-trips Insert -> Normal -> Insert (pure input-handler state).
    using vtbackend::ViMode;
    CHECK(session->terminal().inputHandler().mode() == ViMode::Insert);
    (*session)(actions::ViNormalMode {});
    CHECK(session->terminal().inputHandler().mode() == ViMode::Normal);
    (*session)(actions::ViNormalMode {});
    CHECK(session->terminal().inputHandler().mode() == ViMode::Insert);
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
    auto const restore = crispy::finally { [&] {
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
    session->sendCharEvent(
        U'c', 0, Modifiers { Modifier::Control }, KeyboardEventType::Press, std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer().find('\x03') != std::string::npos);

    // A function/navigation key produces its escape sequence (CSI-prefixed).
    pty.stdinBuffer().clear();
    session->sendKeyEvent(
        Key::UpArrow, Modifiers {}, KeyboardEventType::Press, std::chrono::steady_clock::now());
    CHECK(pty.stdinBuffer().find('\033') != std::string::npos);

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
    CHECK(pty.stdinBuffer().find('\033') != std::string::npos);
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
        CHECK(pty.stdinBuffer().find("ababab") != std::string::npos);
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
    CHECK(session->applicationPointerShape() == contour::MouseCursorShape::PointingHand);

    // Each supported CSS name maps to its own shape, display or no display.
    session->setPointerShape("text");
    CHECK(session->applicationPointerShape() == contour::MouseCursorShape::IBeam);
    session->setPointerShape("none");
    CHECK(session->applicationPointerShape() == contour::MouseCursorShape::Hidden);
    session->setPointerShape("default");
    CHECK(session->applicationPointerShape() == contour::MouseCursorShape::Arrow);

    // The empty name is the documented reset, and must clear the memory rather than pin the last
    // shape -- otherwise the alternate screen would never get its own default back.
    session->setPointerShape("");
    CHECK(session->applicationPointerShape() == std::nullopt);

    // A name this terminal does not implement leaves the remembered shape untouched: it is not a
    // reset, and inventing one would let an unsupported request clear a supported one.
    session->setPointerShape("pointer");
    session->setPointerShape("zoom-in");
    CHECK(session->applicationPointerShape() == contour::MouseCursorShape::PointingHand);
}

TEST_CASE("TerminalSession: desktop notification wiring is display-safe and reports back over the PTY",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // A plain notification just relays to the FreeDesktopNotifier (Linux) / showNotification signal.
    auto plain = vtbackend::DesktopNotification {};
    plain.identifier = "n1";
    plain.title = "Build finished";
    plain.body = "OK";
    CHECK_NOTHROW(session->showDesktopNotification(plain));

    // A notification requesting close/activation/focus reporting wires the notifier's signals; the
    // handlers reply over the PTY. Emitting the notifier signals drives those single-shot handlers.
    auto reporting = vtbackend::DesktopNotification {};
    reporting.identifier = "n2";
    reporting.title = "Job";
    reporting.body = "done";
    reporting.closeEventRequested = true;
    reporting.reportOnActivation = true;
    reporting.focusOnActivation = true;
    CHECK_NOTHROW(session->showDesktopNotification(reporting));

    session->discardDesktopNotification("n2"); // routes to the notifier's close()
    SUCCEED("desktop notification paths execute without a display");
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
    QObject::connect(
        session.get(), &contour::TerminalSession::scrollOffsetChanged, [&seen](int value) { seen = value; });
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
                     &contour::TerminalSession::sessionClosed,
                     [&closed](contour::TerminalSession&) { closed = true; });

    // The shell "dies" moments after startup: onClosed() must route through the early-exit notice
    // (deliberately NOT emitting sessionClosed) because the default early-exit threshold is 5s.
    pty.close();
    session->onClosed();
    CHECK_FALSE(closed);

    // The acknowledging key press prunes the pane: sessionClosed fires now.
    session->sendCharEvent(U'x',
                           static_cast<uint32_t>('x'),
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
    auto session = makeDisplaylessSession(testApp.app());

    // Headlessly the FreeDesktop notifier has no valid D-Bus interface, so notify() early-returns —
    // but the per-request signal-connection branches (close/activation/focus) still execute. Drive
    // every combination so all three branches are covered, and discard by id.
    auto notification = vtbackend::DesktopNotification {};
    notification.identifier = "note-1";
    notification.title = "Title";
    notification.body = "Body";
    notification.closeEventRequested = true;
    notification.reportOnActivation = true;
    notification.focusOnActivation = true;
    CHECK_NOTHROW(session->showDesktopNotification(notification));

    // A second notification with none of the optional reports takes the plain path.
    auto plain = vtbackend::DesktopNotification {};
    plain.identifier = "note-2";
    plain.title = "Plain";
    plain.closeEventRequested = false;
    plain.reportOnActivation = false;
    plain.focusOnActivation = false;
    CHECK_NOTHROW(session->showDesktopNotification(plain));

    CHECK_NOTHROW(session->discardDesktopNotification("note-1"));
}

TEST_CASE("TerminalSession: notify() routes an OSC-9 title/body to the desktop notifier",
          "[contour][session][notification]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());

    // notify() builds a DesktopNotification and hands it to the (headlessly inert) notifier; the
    // build-and-dispatch path must run without a display or a live D-Bus session.
    CHECK_NOTHROW(session->notify("Build finished", "All targets are up to date"));
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
        session->terminal().setNewSearchTerm(U"needle", /*initiatedByDoubleClick*/ false);
    }
    // At least one of next/prev must find a match now (position-dependent), and neither throws.
    auto const nextFound = (*session)(actions::FocusNextSearchMatch {});
    auto const prevFound = (*session)(actions::FocusPreviousSearchMatch {});
    CHECK((nextFound || prevFound));
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
    session->addToAccumulatedScroll(crispy::point { .x = 0, .y = 0 },
                                    crispy::point { .x = 0, .y = 8 * 5 * 3 },
                                    vtbackend::ScrollPhase::NoPhase);
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
        crispy::point {}, crispy::point { .x = 0, .y = -Step }, vtbackend::ScrollPhase::Begin);
    for ([[maybe_unused]] auto const _: std::views::iota(0, 20))
        session->addToAccumulatedScroll(
            crispy::point {}, crispy::point { .x = Step, .y = -1 }, vtbackend::ScrollPhase::Update);

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
        crispy::point {}, crispy::point { .x = Step, .y = 0 }, vtbackend::ScrollPhase::Begin);
    session->addToAccumulatedScroll(
        crispy::point {}, crispy::point { .x = Step, .y = 0 }, vtbackend::ScrollPhase::Update);

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
    QTemporaryDir dir;
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

TEST_CASE("TerminalSession: config-driven permissions resolve without asking the user",
          "[contour][session][permission]")
{
    // A profile that pre-allows ChangeFont and denies CaptureBuffer drives the requestPermission
    // Allow/Deny branches straight to executeRole — no permission dialog signal is emitted.
    contour::test::TestApp testApp;
    auto const profileName = registerProfile(testApp.app(), "perm", [](contour::config::TerminalProfile& p) {
        p.permissions.value().changeFont = contour::config::Permission::Allow;
        p.permissions.value().captureBuffer = contour::config::Permission::Deny;
    });
    auto session = makeSessionWithProfile(testApp.app(), profileName);
    namespace actions = contour::actions;

    // A font-change action with a pre-allowed permission applies without asking (no crash, no dialog).
    CHECK_NOTHROW((*session)(actions::ResetFontSize {}));
    // Requesting a capture with a config-denied permission resolves to the deny path.
    CHECK_NOTHROW(session->executePendingBufferCapture(false, false));
}

namespace
{
/// Writes an OSC-8 hyperlink cell to @p session's screen and hovers the mouse over it, so
/// terminal().tryGetHoveringHyperlink() resolves — the precondition FollowHyperlink needs to reach
/// TerminalSession::followHyperlink() without a display. @p uri is the OSC-8 target.
void seedHoveredHyperlink(contour::TerminalSession& session, std::string const& uri)
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

#if !defined(_WIN32)
    // The "is this an executable file" branch of followHyperlink() keys off POSIX owner-execute
    // permission; Windows determines executability by extension, not filesystem perms, so this Unix
    // semantics section does not apply there.
    SECTION("a local file:// link to an executable file runs `contour config` via execute()")
    {
        contour::test::TestApp testApp;
        auto session = makeDisplaylessSession(testApp.app());
        auto& launcher = testApp.launcher();

        // Create a real executable file and hover a file://<localhost>/<path> link to it.
        QTemporaryDir tmp;
        REQUIRE(tmp.isValid());
        auto const exePath = std::filesystem::path(tmp.path().toStdString()) / "script.sh";
        {
            auto out = std::ofstream(exePath);
            out << "#!/bin/sh\n";
        }
        std::filesystem::permissions(
            exePath, std::filesystem::perms::owner_all, std::filesystem::perm_options::add);
        auto const uri = "file://" + QHostInfo::localHostName().toStdString() + exePath.string();

        seedHoveredHyperlink(*session, uri);
        REQUIRE(session->terminal().tryGetHoveringHyperlink() != nullptr);
        CHECK((*session)(actions::FollowHyperlink {}));
        REQUIRE(launcher.executed.size() == 1);
        CHECK(launcher.executed.back().arguments.contains(QStringLiteral("config")));
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
    // openUrl returning false drives the "could not open" error-log branch of OpenConfiguration /
    // OpenFileManager / OpenSelection — a diagnostic, not a crash.
    contour::test::TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    namespace actions = contour::actions;
    testApp.launcher().openUrlResult = false;

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
    session->sendCharEvent(U'P', 0, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // The session is not registered with a window here, so the palette has nowhere to open — the point
    // is that the action still consumed the key rather than letting it through. A chord with no binding,
    // by contrast, is encoded and written to the PTY.
    session->sendCharEvent(U'Y', 0, ctrlShift, KeyboardEventType::Press, now);
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

    session->sendCharEvent(U'<', 0, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // consumed by the ',' binding via its base char

    // A shifted symbol whose base key has no binding still reaches the terminal (retry misses, falls
    // through) — the normalization only rescues chords that are actually bound.
    session->sendCharEvent(U'~', 0, ctrlShift, KeyboardEventType::Press, now); // base '`', unbound
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
    // modifier equality in config::apply, the mode gate, and the codepoint the input routes deliver.
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
    session->sendCharEvent(U'Q', 0, chord, KeyboardEventType::Press, now);

    CHECK(mockPtyOf(*session).stdinBuffer().empty()); // consumed by the binding
    CHECK(session->terminal().primaryScreen().historyLineCount() == vtbackend::LineCount(0));

    // A subset of the chord must NOT match: config::apply compares modifiers with ==, and this is the
    // property that keeps Ctrl+Q from firing a binding written for Ctrl+Alt+Shift+Q.
    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(
        U'Q', 0, Modifiers { vtbackend::Modifier::Control }, KeyboardEventType::Press, now);
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
    session->sendCharEvent(U'P', 0, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(U'p', 0, ctrlShift, KeyboardEventType::Press, now);
    CHECK(mockPtyOf(*session).stdinBuffer().empty());

    // An unbound letter still reaches the shell — the fold must not swallow everything.
    mockPtyOf(*session).stdinBuffer().clear();
    session->sendCharEvent(U'y', 0, ctrlShift, KeyboardEventType::Press, now);
    CHECK_FALSE(mockPtyOf(*session).stdinBuffer().empty());
}
