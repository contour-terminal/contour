// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession tests, continued: what a session asks of its view, the gutter, the sticky header,
// an ended pane and the anchoring of a hovered block.

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
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/PaneState.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
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

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

using namespace contour::test::sessionSupport;

namespace
{

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

/// The footer an interruption raises: the session has been told a restart stopped @p commandLine.
/// @param session The session, which stands for a daemon-hosted pane whose mirror reported it.
/// @param commandLine The stopped command line.
void interrupt(contour::session::TerminalSession& session, std::string commandLine)
{
    session.noteInterruption(vtbackend::CommandLineText { std::move(commandLine) });
}

/// The ids of the three finished commands of runTwoBlocksAndHoverTheFirst().
struct TwoBlocksHovered
{
    vtbackend::CommandBlockId first {};
    vtbackend::CommandBlockId second {};
    vtbackend::CommandBlockId third {};
};

/// Runs three finished commands, each over two rows, then moves the grid's pointer onto row 0, which hovers
/// the first.
/// @param session The session to drive.
/// @return The two blocks' ids, in the order they ran.
[[nodiscard]] TwoBlocksHovered runTwoBlocksAndHoverTheFirst(contour::session::TerminalSession& session)
{
    runFailingCommand(session);
    runFailingCommand(session);
    runFailingCommand(session);
    auto& terminal = session.terminal();
    auto const ids = core::locked(terminal, [&] {
        return TwoBlocksHovered { .first = terminal.commandBlockAt(vtbackend::LineOffset(0))->id,
                                  .second = terminal.commandBlockAt(vtbackend::LineOffset(2))->id,
                                  .third = terminal.commandBlockAt(vtbackend::LineOffset(4))->id };
    });
    session.sendMouseMoveEvent(
        vtbackend::Modifiers {},
        vtbackend::CellLocation { .line = vtbackend::LineOffset(0), .column = vtbackend::ColumnOffset(1) },
        vtbackend::PixelCoordinate {});
    return ids;
}

} // namespace

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
    CHECK((*session)(actions::OpenConfiguration { .target = actions::ConfigurationTarget::Editor }));
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
    // End-to-end counterpart to the parse-level test in the Config tests. That one proves a binding
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

TEST_CASE("TerminalSession: asking announces the title", "[contour][session][permission]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto announcer = std::make_unique<RecordingAnnouncer>();
    auto* recorder = announcer.get();
    session->setAnnouncer(std::move(announcer));

    auto font = vtbackend::FontDef {};
    font.size = 17.0;
    session->setFontDef(font);

    REQUIRE(recorder->said.size() == 1);
    auto const expected = contour::session::fontChangePrompt(session->resolvedWindowTitle(), font).title;
    CHECK(recorder->said.front().message.toStdString() == expected);
    CHECK(expected.starts_with("Allow "));
    CHECK(recorder->said.front().politeness == QAccessible::AnnouncementPoliteness::Polite);

    // Answered, nothing more is said.
    answerShown(*session, PermissionDecision::Deny, PermissionMemory::Once);
    QCoreApplication::processEvents();
    CHECK(recorder->said.size() == 1);

    // Each question shown is said once. Three asked; the first answered, the second settled before it was
    // shown: the third is said once, though the queue moved on twice before the event loop turned.
    session->setFontDef(font);
    session->requestCaptureBuffer(vtbackend::LineCount(1), vtbackend::CaptureLineMode::Physical);
    session->requestShowHostWritableStatusLine();
    REQUIRE(recorder->said.size() == 2);
    answerShown(*session, PermissionDecision::Deny, PermissionMemory::Once);
    session->executePendingBufferCapture(PermissionDecision::Deny, PermissionMemory::Once);
    QCoreApplication::processEvents();
    CHECK(recorder->said.size() == 3);
    CHECK(recorder->said.back().message.toStdString().ends_with("to show its own status line?"));
}

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
    // History to scroll through, published as the frame timer would: without any, no bar shows at all.
    writeFiller(held->terminal(), 60);
    held->refreshScrollbarMarks();
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

    // Find Next wraps (R-h), so a step finds nothing only where nothing matches: a pattern that matches
    // nothing, set while the user stands on the match the step above revealed.
    {
        auto const _ = std::scoped_lock { held->terminal() };
        REQUIRE(held->terminal().setNewSearchTerm(U"haystack", vtbackend::SearchOrigin::Typed).has_value());
    }
    CHECK((*held)(contour::actions::FocusNextSearchMatch {}));

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
    // Closes the last gap between the YAML and the grid: the Config tests prove the two bounds survive
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

    CHECK(session->terminal().settings().gutter
          == contour::config::gutterSettingsFor(session->config(), session->gutterChrome().form));
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
                                               .evictedPlaceholder = vtbackend::EvictedPlaceholder::Hidden,
                                               .presentation = vtbackend::StickyHeaderPresentation::InGrid });
}

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
                     &contour::session::TerminalSessionManager::blockMenuRequested,
                     [&](contour::session::TerminalSession* /*acting*/, vtbackend::CommandBlockId block) {
                         ++menuRequests;
                         menuBlock = block;
                     });
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
    auto asked = 0;
    contour::test::countAsks(
        *session, contour::session::PermissionAsk { contour::session::GuardedRole::RerunCommand }, asked);

    SECTION("asked first, shown what it runs, and run once allowed")
    {
        CHECK((*session)(RerunCommand {}));
        CHECK(asked == 1);
        // The prompt shows what runs, as the shell reported it.
        auto const lines = session->pendingPermission().value(QStringLiteral("lines")).toList();
        REQUIRE_FALSE(lines.isEmpty());
        CHECK(lines.front().toMap().value(QStringLiteral("text")).toString() == "make");
        CHECK(written.empty());

        session->applyPendingRerun(PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(written == "make\r");
        CHECK(session->pendingPermission().isEmpty());
    }

    SECTION("refused, nothing is typed")
    {
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(PermissionDecision::Deny, PermissionMemory::Once);
        CHECK(written.empty());
    }

    SECTION("allowed for the session, the next one runs without asking")
    {
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(PermissionDecision::Allow, PermissionMemory::Session);
        written.clear();
        CHECK((*session)(RerunCommand {}));
        CHECK(asked == 1);
        CHECK(written == "make\r");
    }

    SECTION("through a bracketed paste where the shell asked for one")
    {
        terminal.writeToScreen("\033[?2004h");
        REQUIRE((*session)(RerunCommand {}));
        session->applyPendingRerun(PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(written == "\033[200~make\033[201~\r");
    }

    SECTION("not while a command runs: typed, it would be that command's input")
    {
        CHECK(terminal.shellIsPrompting());
        terminal.writeToScreen(vtbackend::test::commandStarted("sleep 9"));
        CHECK_FALSE(terminal.shellIsPrompting());
        CHECK_FALSE((*session)(RerunCommand {}));
        CHECK(asked == 0);
    }

    SECTION("the prompt gone by the time the answer comes, nothing is typed")
    {
        REQUIRE((*session)(RerunCommand {}));
        terminal.writeToScreen("sleep 9\r\n\033]133;C\033\\");
        session->applyPendingRerun(PermissionDecision::Allow, PermissionMemory::Once);
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
        CHECK(asked == 0);
    }
}

TEST_CASE("TerminalSession: a restored shell's footer names the stopped command and waits for a prompt "
          "to offer Run Again",
          "[contour][session][paneend][interrupted]")
{
    using contour::session::FooterAction;
    using contour::window::PaneFooterKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto changes = QSignalSpy(&*session, &contour::session::TerminalSession::paneFooterChanged);
    CHECK(session->getPaneFooter() == static_cast<int>(PaneFooterKind::None));
    CHECK(session->getFooterMessage().isEmpty());

    interrupt(*session, "make test");
    CHECK(changes.count() == 1);
    CHECK(session->getPaneFooter() == static_cast<int>(PaneFooterKind::Interrupted));
    CHECK(session->getFooterMessage()
          == QString::fromUtf8("\u201cmake test\u201d was stopped when the session ended."));
    CHECK(session->getFooterDismissLabel() == "Dismiss");
    CHECK(session->getFooterTakesReturn() == 0);
    // The fresh shell has shown no prompt: there is nothing to type the command at, so no Run Again.
    CHECK(session->footerAction() == FooterAction::None);
    CHECK(session->getFooterActionLabel().isEmpty());

    // The prompt arrives, and the parser's screen update has the offer re-decided on the GUI thread.
    session->terminal().writeToScreen(vtbackend::test::PromptStart);
    session->screenUpdated();
    QCoreApplication::processEvents();
    CHECK(changes.count() == 2);
    CHECK(session->footerAction() == FooterAction::RunAgain);
    CHECK(session->getFooterActionLabel() == "Run Again");

    SECTION("the same news twice changes nothing")
    {
        interrupt(*session, "make test");
        CHECK(changes.count() == 2);
    }

    SECTION("an empty line is none")
    {
        session->noteInterruption(vtbackend::CommandLineText {});
        CHECK(session->getPaneFooter() == static_cast<int>(PaneFooterKind::None));
    }
}

TEST_CASE("TerminalSession: a stopped command cut at the recorded maximum is never offered to Run Again",
          "[contour][session][paneend][interrupted]")
{
    using contour::session::FooterAction;
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(vtbackend::test::PromptStart);
    interrupt(*session,
              vtbackend::truncatedCommandLine(std::string(vtbackend::MaxRecordedCommandLineBytes + 1, 'x')));
    session->screenUpdated();
    QCoreApplication::processEvents();
    // Named in the footer, but it is not the command that ran.
    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));
    CHECK(session->footerAction() == FooterAction::None);
    CHECK(session->requestRerun(vtbackend::CommandLineText {
              vtbackend::truncatedCommandLine(std::string(vtbackend::MaxRecordedCommandLineBytes + 1, 'x')) })
          == contour::session::RerunAsk::Refused);
}

TEST_CASE("TerminalSession: Return at a restored shell goes to the shell",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(vtbackend::test::PromptStart);
    interrupt(*session, "make test");
    session->screenUpdated();
    QCoreApplication::processEvents();
    REQUIRE(session->getFooterActionLabel() == "Run Again");
    auto& written = mockPtyOf(*session).stdinBuffer();
    written.clear();
    auto asked = 0;
    contour::test::countAsks(
        *session, contour::session::PermissionAsk { contour::session::GuardedRole::RerunCommand }, asked);

    session->sendKeyEvent(vtbackend::Key::Enter,
                          vtbackend::Modifiers {},
                          vtbackend::KeyboardEventType::Press,
                          std::chrono::steady_clock::now());

    CHECK(written == "\r");
    CHECK(asked == 0);
    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));
}

TEST_CASE("TerminalSession: Run Again asks first, and types the command once allowed",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(vtbackend::test::PromptStart);
    interrupt(*session, "make test");
    session->screenUpdated();
    QCoreApplication::processEvents();
    auto& written = mockPtyOf(*session).stdinBuffer();
    written.clear();
    auto asked = 0;
    contour::test::countAsks(
        *session, contour::session::PermissionAsk { contour::session::GuardedRole::RerunCommand }, asked);

    session->runFooterAction();
    CHECK(asked == 1);
    auto const lines = session->pendingPermission().value(QStringLiteral("lines")).toList();
    REQUIRE_FALSE(lines.isEmpty());
    CHECK(lines.front().toMap().value(QStringLiteral("text")).toString() == "make test");
    CHECK(written.empty());
    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));

    SECTION("allowed: typed and entered, and the footer goes")
    {
        session->applyPendingRerun(PermissionDecision::Allow, PermissionMemory::Once);
        CHECK(written == "make test\r");
        CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::None));
    }

    SECTION("denied: nothing is typed, and the footer stays for another try")
    {
        session->applyPendingRerun(PermissionDecision::Deny, PermissionMemory::Once);
        CHECK(written.empty());
        CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));
    }
}

TEST_CASE("TerminalSession: a hostile stopped command is shown sanitized and asked about",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(vtbackend::test::PromptStart);
    auto& written = mockPtyOf(*session).stdinBuffer();
    written.clear();
    auto asked = 0;
    contour::test::countAsks(
        *session, contour::session::PermissionAsk { contour::session::GuardedRole::RerunCommand }, asked);

    SECTION("a line with a bidi override is shown sanitized and never typed")
    {
        // U+202E, RIGHT-TO-LEFT OVERRIDE, built byte by byte so no source literal holds a misleading
        // character.
        auto const bidiOverride = std::string { '\xE2', '\x80', '\xAE' };
        interrupt(*session, "echo " + bidiOverride + "evil; rm -rf ~");
        session->screenUpdated();
        QCoreApplication::processEvents();
        auto const message = session->getFooterMessage();
        CHECK_FALSE(message.contains(QChar(0x202e)));
        CHECK(message.contains(QStringLiteral("rm -rf ~")));

        // Typed through the paste path it would lose the override, and be another command: refused.
        session->runFooterAction();
        CHECK(asked == 0);
        CHECK(written.empty());
    }

    SECTION("a line that can be typed is shown to the user before it runs, defaulting to Deny")
    {
        interrupt(*session, "make; rm -rf ~");
        session->screenUpdated();
        QCoreApplication::processEvents();
        CHECK(session->getFooterMessage().contains(QStringLiteral("rm -rf ~")));

        session->runFooterAction();
        REQUIRE(asked == 1);
        CHECK(written.empty());
        auto const lines = session->pendingPermission().value(QStringLiteral("lines")).toList();
        REQUIRE_FALSE(lines.isEmpty());
        CHECK(lines.front().toMap().value(QStringLiteral("text")).toString() == "make; rm -rf ~");
        CHECK(session->pendingPermission().value(QStringLiteral("defaultDecision")).toInt()
              == static_cast<int>(PermissionDecision::Deny));

        session->applyPendingRerun(PermissionDecision::Deny, PermissionMemory::Once);
        CHECK(written.empty());
    }

    SECTION("a line feed in the line is never typed")
    {
        interrupt(*session, "echo a\necho b");
        session->screenUpdated();
        QCoreApplication::processEvents();
        CHECK_FALSE(session->getFooterMessage().contains(QChar('\n')));
        session->runFooterAction();
        CHECK(asked == 0);
        CHECK(written.empty());
    }
}

TEST_CASE("TerminalSession: Dismiss takes the restored shell's footer away in this client",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    interrupt(*session, "make test");
    REQUIRE(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));
    auto closed = 0;
    QObject::connect(&*session,
                     &contour::session::TerminalSession::sessionClosed,
                     [&closed](contour::session::TerminalSession&) { ++closed; });

    session->dismissFooter();

    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::None));
    CHECK(closed == 0); // the pane and its shell stay
}

TEST_CASE("TerminalSession: the daemon saying the next command started clears the footer",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    interrupt(*session, "make test");
    session->noteInterruption(std::nullopt);
    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::None));
    // And it is not offered again for a line already told: a later identical report is news.
    interrupt(*session, "make test");
    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));
}

TEST_CASE("TerminalSession: an ended pane's footer wins over an interruption",
          "[contour][session][paneend][interrupted]")
{
    TestApp testApp;
    auto session = makeDisplaylessSession(testApp.app());
    interrupt(*session, "make test");
    REQUIRE(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Interrupted));

    mockPtyOf(*session).close();
    session->onClosed();

    CHECK(session->getPaneFooter() == static_cast<int>(contour::window::PaneFooterKind::Ended));
    CHECK(session->getFooterMessage() == QStringLiteral("Process exited."));
    CHECK(session->getFooterActionLabel() == "Restart");
    CHECK(session->getFooterDismissLabel() == "Close");
    CHECK(session->getFooterTakesReturn() == 1);
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
    REQUIRE_FALSE(session.linkTargetText().isEmpty());
    REQUIRE(terminal.isMouseHoveringHyperlink());

    SECTION("the link the pointer came from ends, and the one beneath the header is never hovered")
    {
        scene.hover(overHeader);
        CHECK(session.linkTargetText().isEmpty());
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
        CHECK_FALSE(session.linkTargetText().isEmpty());
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK(scene.surface().cursorShapes.back() == MouseCursorShape::PointingHand);
    }

    SECTION("without a header row 0 is the grid's")
    {
        REQUIRE(terminal.viewport().scrollToBottom());
        REQUIRE_FALSE(vtbackend::test::drawFrame(terminal).stickyHeader.has_value());
        scene.hover(overHeader);
        CHECK_FALSE(session.linkTargetText().isEmpty());
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
    REQUIRE_FALSE(session->linkTargetText().isEmpty());

    SECTION("into the gutter")
    {
        CHECK(session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::LineNumber))
              == ConsumedByGutter::Yes);
        CHECK_FALSE(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.tryGetHoveringHyperlink() == nullptr); // what the frame underlines
        CHECK(session->linkTargetText().isEmpty());
    }

    SECTION("out of the window")
    {
        session->onPointerLeft();
        CHECK_FALSE(terminal.isMouseHoveringHyperlink());
        CHECK(terminal.tryGetHoveringHyperlink() == nullptr);
        CHECK(session->linkTargetText().isEmpty());
    }

    SECTION("back onto the cell it left from: the link is hovered again, tooltip and all")
    {
        session->onPointerLeft();
        session->sendMouseMoveEvent(vtbackend::Modifiers {}, onLink, vtbackend::PixelCoordinate {});
        CHECK(terminal.isMouseHoveringHyperlink());
        CHECK_FALSE(session->linkTargetText().isEmpty());
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

TEST_CASE("TerminalSession: hovering a block's rows anchors its actions", "[contour][session][gutter]")
{
    using contour::session::ConsumedByGutter;
    using contour::session::TerminalSession;
    using vtbackend::GutterSegmentKind;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);

    SECTION("beside it, in the block column")
    {
        REQUIRE(session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn))
                == ConsumedByGutter::Yes);
        CHECK(spy.count() == 1);
        auto const anchor = session->blockActionsAnchor();
        CHECK(anchor.width() > 0);
        CHECK(anchor.height() > 0);

        // Another row of the same block: the button stays beside its first row.
        session->sendGutterHoverEvent(gutterHit(1, GutterSegmentKind::BlockColumn));
        CHECK(spy.count() == 1);
        CHECK(session->blockActionsAnchor() == anchor);

        // The line numbers beside it are not the block column.
        session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::LineNumber));
        CHECK(spy.count() == 2);
        CHECK(session->blockActionsAnchor().isEmpty());

        // Back on the block, then off the gutter altogether.
        session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn));
        CHECK(spy.count() == 3);
        session->sendGutterHoverEvent(std::nullopt);
        CHECK(spy.count() == 4);
        CHECK(session->blockActionsAnchor().isEmpty());
    }

    SECTION("over its output")
    {
        auto const pointAt = [&](int line) {
            session->sendMouseMoveEvent(Modifiers {},
                                        vtbackend::CellLocation { .line = vtbackend::LineOffset(line),
                                                                  .column = vtbackend::ColumnOffset(2) },
                                        vtbackend::PixelCoordinate {});
        };
        pointAt(1);
        CHECK(spy.count() == 1);
        auto const anchor = session->blockActionsAnchor();
        REQUIRE_FALSE(anchor.isEmpty());

        // The block the anchor belongs to is named, for a chrome that marks it when it is also the focused
        // one.
        auto const* const under = session->terminal().commandBlockAt(vtbackend::LineOffset(1));
        REQUIRE(under != nullptr);
        CHECK(session->hoveredCommandBlock() == static_cast<qulonglong>(under->id.value));

        // The same anchor from the strip beside the block's first row: the button sits where the row is.
        session->sendGutterHoverEvent(gutterHit(0, GutterSegmentKind::BlockColumn));
        CHECK(session->blockActionsAnchor().top() == anchor.top());

        // Another row of the block leaves it where it was.
        pointAt(2);
        CHECK(session->blockActionsAnchor().top() == anchor.top());

        // The button opens the menu of that block, and nothing opens without one.
        auto asked = std::vector<vtbackend::CommandBlockId> {};
        QObject::connect(&testApp.app().sessionsManager(),
                         &contour::session::TerminalSessionManager::blockMenuRequested,
                         [&](contour::session::TerminalSession* /*acting*/, vtbackend::CommandBlockId block) {
                             asked.push_back(block);
                         });
        session->openBlockActions();
        auto const* const hovered = session->terminal().commandBlockAt(vtbackend::LineOffset(1));
        REQUIRE(hovered != nullptr);
        CHECK(asked == std::vector { hovered->id });

        // A row below every block hovers none; leaving the terminal hovers none either.
        pointAt(20);
        CHECK(session->blockActionsAnchor().isEmpty());
        CHECK(session->hoveredCommandBlock() == 0);
        pointAt(1);
        REQUIRE_FALSE(session->blockActionsAnchor().isEmpty());
        session->onPointerLeft();
        CHECK(session->blockActionsAnchor().isEmpty());
        session->openBlockActions();
        CHECK(asked.size() == 1);
    }
}

TEST_CASE("TerminalSession: the window reserves the cells and the strip", "[contour][session][gutter]")
{
    // The session's gutter chrome is its chrome row's, and with that row's rail the strip beside the cells is
    // reserved by the page fit and found by the hit-test: a pointer over its middle names the block column.
    using contour::session::TerminalSession;

    TestApp testApp;
    // Line numbers on, so the cells come first and the strip lies between them and column 0.
    testApp.app().config().gutter.value().lineNumbers = vtbackend::LineNumberMode::Absolute;
    auto session = makeSessionWithSurface(testApp.app());
    CHECK(session->gutterChrome() == contour::session::gutterChromeFor(testApp.app().chromeStyle()));
    REQUIRE(session->gutterChrome().form == vtbackend::BlockColumnForm::Rail);

    auto& surface = *session.surface;
    surface.gridMetricsValue.cellSize = surface.cellSizeValue;
    auto const extent = contour::session::configuredGutterExtent(
        session->config(), surface.cellSizeValue, surface.devicePixelRatioValue, session->gutterChrome());
    REQUIRE(extent.rail > 0);
    REQUIRE(extent.columns > 0);
    surface.gridMetricsValue.pageMargin.left = extent.leadIn + extent.columns + extent.rail;

    runFailingCommand(*session);
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);
    auto const overTheStrip =
        QPointF { surface.gridMetricsValue.pageMargin.left - (extent.rail / 2.0), 4.0 }; // beside row 0
    auto event = QHoverEvent(QEvent::HoverMove, overTheStrip, overTheStrip, overTheStrip);
    contour::session::sendMouseMoveEvent(&event, *session);
    CHECK(spy.count() == 1);
    // The button sits in the strip the pointer is over, beside the block's first row.
    CHECK(session->blockActionsAnchor().width()
          == Catch::Approx(extent.rail / surface.devicePixelRatioValue));
    CHECK(session->blockActionsAnchor().contains(overTheStrip));

    // Over the line numbers, left of the strip, the block is not hovered.
    auto const overTheNumbers = QPointF { extent.leadIn + (extent.columns / 2.0), 4.0 };
    auto numbers = QHoverEvent(QEvent::HoverMove, overTheNumbers, overTheNumbers, overTheNumbers);
    contour::session::sendMouseMoveEvent(&numbers, *session);
    CHECK(spy.count() == 2);
    CHECK(session->blockActionsAnchor().isEmpty());
}

TEST_CASE("TerminalSession: a running block's time is redrawn while it shows, and only then",
          "[contour][session][gutter]")
{
    // The frame build reports a running time on display under the lock; the session arms its once-a-second
    // redraw on its own thread, and stops it once the time is gone.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto& terminal = session->terminal();
    REQUIRE_FALSE(session->refreshesRunningStatus());

    terminal.writeToScreen(vtbackend::test::commandStarted("sleep 9"));
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK(terminal.hasVisibleRunningStatus());
    CHECK(session->refreshesRunningStatus());

    // I1: a pane nobody sees -- its display detached on a tab switch -- stops ticking, though its last frame
    // still shows the time; re-attached, it ticks again without waiting for a change.
    session->detachDisplay(*session.surface);
    CHECK_FALSE(session->refreshesRunningStatus());
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK(terminal.hasVisibleRunningStatus());
    CHECK_FALSE(session->refreshesRunningStatus());
    session->attachDisplay(*session.surface);
    CHECK(session->refreshesRunningStatus());

    // I1 again: a minimised window keeps its display attached but is sent no frames, so its last frame's time
    // would tick for good. Hidden stops the redraw, and the window-state change that brings it back restarts
    // it.
    session.surface->exposureValue = contour::session::DisplayExposure::Hidden;
    session->displayExposureChanged();
    CHECK_FALSE(session->refreshesRunningStatus());
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK(terminal.hasVisibleRunningStatus());
    CHECK_FALSE(session->refreshesRunningStatus());

    session.surface->exposureValue = contour::session::DisplayExposure::Exposed;
    session->displayExposureChanged();
    CHECK(session->refreshesRunningStatus());

    terminal.writeToScreen(vtbackend::test::commandEnd(0));
    terminal.writeToScreen(std::string { vtbackend::test::PromptStart });
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK_FALSE(terminal.hasVisibleRunningStatus());
    CHECK_FALSE(session->refreshesRunningStatus());
}

TEST_CASE("TerminalSession: a minimised window's time is not redrawn when the frame asks for it",
          "[contour][session][gutter]")
{
    // Whatever armed it first -- a frame built while the window still showed -- the timer's tick and the
    // frame build's request both look at the exposure, not only at the last frame's flag.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto& terminal = session->terminal();
    session.surface->exposureValue = contour::session::DisplayExposure::Hidden;

    terminal.writeToScreen(vtbackend::test::commandStarted("sleep 9"));
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    CHECK(terminal.hasVisibleRunningStatus());
    CHECK_FALSE(session->refreshesRunningStatus());
}

TEST_CASE("TerminalSession: a restored window's running time ticks again once it is exposed",
          "[contour][session][gutter]")
{
    // The real order of a restore: the window-state change arrives while the surface is still not exposed,
    // and the expose that follows is the only thing that says the frames are wanted again.
    TestApp testApp { std::make_unique<contour::test::MockPtySessionFactory>() };
    contour::test::ScopedControllerWithOsWindow const window { testApp.manager() };
    window.osWindow->create();
    window->bindWindow(window.osWindow.get());
    window->createNewTab();
    auto* const session = window->activeSession();
    REQUIRE(session != nullptr);
    auto surface = contour::test::FakeDisplaySurface {};
    surface.attachedSession = session;
    session->attachDisplay(surface);
    auto& terminal = session->terminal();

    terminal.writeToScreen(vtbackend::test::commandStarted("sleep 9"));
    (void) vtbackend::test::drawFrame(terminal);
    QCoreApplication::processEvents();
    REQUIRE(session->refreshesRunningStatus());

    // Minimised: the state change finds the surface hidden and the tick stops.
    surface.exposureValue = contour::session::DisplayExposure::Hidden;
    testApp.manager().windowStateChanged(window.id);
    REQUIRE_FALSE(session->refreshesRunningStatus());

    // Restored: the state change still finds it hidden ...
    testApp.manager().windowStateChanged(window.id);
    CHECK_FALSE(session->refreshesRunningStatus());

    // ... and the expose, arriving after, brings the tick back.
    surface.exposureValue = contour::session::DisplayExposure::Exposed;
    auto expose = QExposeEvent { QRegion { 0, 0, 640, 400 } };
    QCoreApplication::sendEvent(window.osWindow.get(), &expose);
    CHECK(session->refreshesRunningStatus());

    session->detachDisplay(surface);
}

TEST_CASE("TerminalSession: a click beside the fold chip still opens it", "[contour][session][gutter]")
{
    // The chip's hit is at least size.foldChipHit wide (R-k), in the display's cells: at 8x device pixels a
    // cell 8 pixels wide is one point, so the 15-column "1 line hidden ⌄" is widened to 24 columns, kept off
    // the command's text -- columns 4..24 after "$ ls" -- and a click there opens it.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto& surface = *session.surface;
    surface.devicePixelRatioValue = 8.0;
    surface.gridMetricsValue.cellSize = surface.cellSizeValue;
    auto& terminal = session->terminal();
    terminal.writeToScreen(vtbackend::test::commandRun("ls", { "a" }));
    terminal.writeToScreen(std::string { vtbackend::test::PromptStart });

    auto const pressAt = [&](int column) {
        REQUIRE(core::locked(terminal, [&] { return terminal.collapseLastFold(); }));
        session->sendMousePressEvent(Modifiers {},
                                     vtbackend::MouseButton::Left,
                                     vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                               .column = vtbackend::ColumnOffset(column) },
                                     vtbackend::PixelCoordinate {});
        session->sendMouseReleaseEvent(
            Modifiers {}, vtbackend::MouseButton::Left, vtbackend::PixelCoordinate {});
        auto const expanded = core::locked(terminal, [&] { return terminal.foldState().empty(); });
        if (!expanded)
            core::locked(terminal, [&] { terminal.expandAllFolds(); });
        return expanded;
    };
    CHECK(pressAt(5));  // on the chip
    CHECK(pressAt(20)); // one column right of its last cell
    CHECK(pressAt(24)); // the widened hit's last column
    CHECK_FALSE(pressAt(25));
    CHECK_FALSE(pressAt(2)); // the command's own text
}

TEST_CASE("TerminalSession: a scroll names the block that slid under the pointer",
          "[contour][session][gutter]")
{
    // The hovered block is found on pointer MOVEMENT, so a scroll slides another block under a stationary
    // pointer with nothing noticing; the block is decided again at the row the pointer stands on.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE_FALSE(session->blockActionsAnchor().isEmpty());

    // Nothing is scrollable here, so the page moves under the pointer by output: a command's block is pushed
    // up and the row at the pointer now belongs to no block.
    session->terminal().writeToScreen(std::string(40, '\n'));
    session->onScrollOffsetChanged(vtbackend::ScrollOffset(0));
    QCoreApplication::processEvents();
    // Whatever the page now has at row 0 -- the block's trailing rows, or none -- is what the terminal
    // hovers.
    auto const under = core::locked(session->terminal(), [&] {
        auto const* const block = session->terminal().commandBlockAt(
            session->terminal().viewport().translateScreenToGridLine(vtbackend::LineOffset(0)));
        return block != nullptr ? std::optional { block->id } : std::nullopt;
    });
    CHECK(session->terminal().hoveredBlock() == under);
    CHECK(session->blockActionsAnchor().isEmpty() == !under.has_value());
}

TEST_CASE("TerminalSession: a scroll re-decides the terminal's hovered block from the grid",
          "[contour][session][gutter]")
{
    // Deleting the re-decision leaves the hover cleared, which the anchor alone cannot tell from "no block
    // here": the terminal's own hovered block is what the lit rail and the ring read.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const blocks = runTwoBlocksAndHoverTheFirst(*session);
    REQUIRE(session->terminal().hoveredBlock() == blocks.first);

    // The page moves two rows up under the stationary pointer: the second block is at row 0 now.
    session->terminal().writeToScreen("\033[999;1H\n\n");
    session->onScrollOffsetChanged(vtbackend::ScrollOffset(0));
    QCoreApplication::processEvents();

    CHECK(session->terminal().hoveredBlock() == blocks.second);
    CHECK_FALSE(session->blockActionsAnchor().isEmpty());
}

TEST_CASE("TerminalSession: after a scroll with the pointer on the rail only one block is hovered",
          "[contour][session][gutter]")
{
    // The scroll re-decides from the grid; a pointer that then moves along the rail names the block beside
    // it, and the one the grid had named must not stay lit beside it.
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const blocks = runTwoBlocksAndHoverTheFirst(*session);
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE(session->terminal().hoveredBlock() == blocks.first);

    session->terminal().writeToScreen("\033[999;1H\n\n");
    session->onScrollOffsetChanged(vtbackend::ScrollOffset(0));
    QCoreApplication::processEvents();
    REQUIRE(session->terminal().hoveredBlock() == blocks.second);

    // Down the rail to the third block, now at row 2: the rail names it, and the second does not stay lit.
    session->sendGutterHoverEvent(gutterHit(2, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(session->terminal().hoveredBlock() == blocks.third);
}

TEST_CASE("TerminalSession: a scroll that moves no block under the pointer withdraws nothing",
          "[contour][session][gutter]")
{
    // The hover is set again in one locked step, not cleared and then set: a frame built between the two
    // would show no hover, and the anchor would be announced gone and back at every step of a wheel scroll.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    auto const blocks = runTwoBlocksAndHoverTheFirst(*session);
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE(session->terminal().hoveredBlock() == blocks.first);
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);

    session->onScrollOffsetChanged(vtbackend::ScrollOffset(0)); // the same block stays at row 0
    QCoreApplication::processEvents();

    CHECK(session->terminal().hoveredBlock() == blocks.first);
    CHECK(spy.count() == 0);
}

TEST_CASE("TerminalSession: a scroll over the line numbers lights no block", "[contour][session][gutter]")
{
    // Beside the block column there is nothing to light, and a scroll decides the hover again only for a
    // pointer that stands on the grid or on the block column.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    runFailingCommand(*session);
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    REQUIRE(session->terminal().hoveredBlock().has_value());
    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::LineNumber));
    REQUIRE_FALSE(session->terminal().hoveredBlock().has_value());

    session->onScrollOffsetChanged(vtbackend::ScrollOffset(0));
    QCoreApplication::processEvents();

    CHECK_FALSE(session->terminal().hoveredBlock().has_value());
    CHECK(session->blockActionsAnchor().isEmpty());
}

TEST_CASE("TerminalSession: a session without a manager opens no menu", "[contour][session][blockmenu]")
{
    TestApp testApp;
    auto const session = contour::test::makeTestSession(
        nullptr, std::make_unique<vtpty::MockPty>(TestPageSize), testApp.app(), {}, nullptr);
    runFailingCommand(*session);
    REQUIRE((*session)(contour::actions::FocusPreviousCommand {}));

    // Both menus have no window to open in; neither takes the manager it does not have.
    CHECK((*session)(contour::actions::OpenBlockMenu {}));
    CHECK_FALSE((*session)(contour::actions::OpenContextMenu {}));
}

TEST_CASE("TerminalSession: without shell integration there is no block to anchor actions to",
          "[contour][session][gutter]")
{
    // A row no OSC 133 mark claimed belongs to no block, so there is nothing to describe.
    using contour::session::TerminalSession;

    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen("plain output\r\n");
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);

    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 0);
    CHECK(session->blockActionsAnchor().isEmpty());
}

TEST_CASE("TerminalSession: over the alternate screen there is no block to anchor actions to",
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
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);

    session->sendGutterHoverEvent(gutterHit(0, vtbackend::GutterSegmentKind::BlockColumn));
    CHECK(spy.count() == 0);
    CHECK(session->blockActionsAnchor().isEmpty());
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
    REQUIRE_FALSE(session->blockActionsAnchor().isEmpty());
    QSignalSpy const spy(session.session.get(), &TerminalSession::blockActionsAnchorChanged);

    // One more line at the bottom of the page scrolls it, and the scrolled-back viewport follows.
    std::thread { [&] { session->terminal().writeToScreen("\033[24;1H\n"); } }.join();
    CHECK(spy.count() == 0);
    CHECK_FALSE(session->blockActionsAnchor().isEmpty());

    // The GUI thread then decides the block at the row the pointer stands on afresh.
    QCoreApplication::processEvents();
    auto const under = core::locked(session->terminal(), [&] {
        auto const* const block = session->terminal().commandBlockAt(
            session->terminal().viewport().translateScreenToGridLine(vtbackend::LineOffset(0)));
        return block != nullptr ? std::optional { block->id } : std::nullopt;
    });
    CHECK(session->terminal().hoveredBlock() == under);
    CHECK(session->blockActionsAnchor().isEmpty() == !under.has_value());
}

TEST_CASE("TerminalSession: a scroll withdraws the link target, on the GUI thread",
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
    REQUIRE_FALSE(session->linkTargetText().isEmpty());
    QSignalSpy const spy(session.session.get(), &TerminalSession::linkTargetChanged);

    // One more line at the bottom of the page scrolls it, and the scrolled-back viewport follows.
    std::thread { [&] { session->terminal().writeToScreen("\033[24;1H\n"); } }.join();
    CHECK(spy.count() == 0);
    CHECK_FALSE(session->linkTargetText().isEmpty());

    QCoreApplication::processEvents();
    CHECK(spy.count() == 1);
    CHECK(session->linkTargetText().isEmpty());
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
    CHECK(session->linkTargetText().toStdString() == resolvedInWorkDirectory("notes.txt") + ":3:1");
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
        return session->linkTargetText().toStdString();
    };
    auto const second = static_cast<int>(first.size()) + 2;

    CHECK(tooltipAt(1) == resolvedInWorkDirectory("one.txt") + ":3:1");
    CHECK(tooltipAt(2) == resolvedInWorkDirectory("one.txt") + ":3:1"); // along the same link
    CHECK(tooltipAt(second) == resolvedInWorkDirectory("two.txt") + ":4:1");
    CHECK(tooltipAt(1) == resolvedInWorkDirectory("one.txt") + ":3:1"); // and back
}

TEST_CASE("TerminalSession: a detected link printed as it opens names its target in the pill",
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
    // The pill names what is printed all the same: the canvas's pill names a printed URL (R-f).
    CHECK(session->linkTargetText() == QStringLiteral("example.com \u203A page"));
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
