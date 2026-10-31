// SPDX-License-Identifier: Apache-2.0
//
// Fixtures shared by the TerminalSession_test*.cpp files: session factories, recording doubles and the
// scenes several areas drive.

#pragma once

#include <contour/ContourGuiApp.hpp>
#include <contour/config/Actions.hpp>
#include <contour/platform/FinishNotification.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/PermissionFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/ScrollbarMarks.hpp>
#include <vtbackend/shell/StickyHeader.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>

#include <core/Utf8Path.hpp>

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

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QtTest/QSignalSpy>
#include <QtTest/QTest>

namespace contour::test::sessionSupport
{

using namespace std::string_literals;

using contour::session::PermissionDecision;

using contour::session::PermissionMemory;

using contour::test::answerShown;

using contour::test::TestApp;

constexpr auto TestPageSize = vtpty::PageSize { vtpty::LineCount(24), vtpty::ColumnCount(80) };

/// Creates a TerminalSession backed by @p pty, with NO display attached (the default state).
/// The session owns the PTY; the caller owns the session.
/// @param notifier the desktop notifier to inject; null (the default) takes the test notifier.
/// @param profileName the profile to run under; empty (the default) takes the app's default one.
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeSessionWith(
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
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeDisplaylessSession(
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
inline void writeFinishedBlock(contour::session::TerminalSession& session,
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

using vtbackend::KeyboardEventType;

using vtbackend::Modifiers;

using contour::test::mockPtyOf;

/// The lines a command prints in the folding tests: `out 0`, `out 1`, and so on.
/// @param count How many lines.
/// @return The lines, for vtbackend::test::commandRun.
[[nodiscard]] inline std::vector<std::string> numberedOutput(int count)
{
    return std::ranges::to<std::vector>(
        std::views::iota(0, count) | std::views::transform([](int i) { return std::format("out {}", i); }));
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
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeSessionWithProfile(
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
[[nodiscard]] inline SessionWithSurface makeSessionWithSurface(
    contour::ContourGuiApp& app, std::optional<std::string> profileName = std::nullopt)
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
[[nodiscard]] inline std::string inWorkDirectory(std::string_view name)
{
    return std::format("{}/{}", WorkDirectory, name);
}

/// @return The file @p name in WorkDirectory, as a path resolves to it: normalised, in this platform's
///         separators.
[[nodiscard]] inline std::string resolvedInWorkDirectory(std::string_view name)
{
    return core::pathToUtf8(
        core::pathFromUtf8(inWorkDirectory(name)).value_or(std::filesystem::path {}).lexically_normal());
}

/// Registers a profile that shows the scrollbar on the right with @p marks as its `scrollbar.marks`, and
/// keeps @p history lines of scrollback.
inline std::string registerMarkedScrollbarProfile(contour::ContourGuiApp& app,
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
inline void writeFiller(vtbackend::Terminal& terminal, int count)
{
    auto text = std::string {};
    for (auto const i: std::views::iota(0, count))
        text += std::format("filler {}\r\n", i);
    terminal.writeToScreen(text);
}

/// The kind of every tick @p session publishes, top first.
[[nodiscard]] inline std::vector<vtbackend::ScrollbarMarkKind> markKindsOf(
    contour::session::TerminalSession const& session)
{
    return std::ranges::to<std::vector>(session.scrollbarMarks()
                                        | std::views::transform([](QVariant const& mark) {
                                              return static_cast<vtbackend::ScrollbarMarkKind>(
                                                  mark.toMap().value(QStringLiteral("kind")).toInt());
                                          }));
}

/// How @p session's tick @p index is drawn: 0 solid, 1 hollow (vtbackend::ScrollbarTickStyle).
[[nodiscard]] inline int markStyle(contour::session::TerminalSession const& session, qsizetype index)
{
    return session.scrollbarMarks().at(index).toMap().value(QStringLiteral("style")).toInt();
}

/// The colour @p session publishes for ticks of @p kind.
[[nodiscard]] inline QColor markColor(contour::session::TerminalSession const& session,
                                      vtbackend::ScrollbarMarkKind kind)
{
    return session.scrollbarMarkColors().at(static_cast<qsizetype>(kind)).value<QColor>();
}

/// Rewrites the file @p session's configuration names to hold @p sections and nothing else beside
/// @p session's own profile, then reloads @p session from it: the path a saved configuration file takes.
/// @param session The session to reload.
/// @param sections Top-level YAML sections, each line ending in a newline.
/// @param profileEntries Entries of the profile, each line indented by eight spaces and ending in a newline.
inline void reloadSessionWith(contour::session::TerminalSession& session,
                              std::string_view sections,
                              std::string_view profileEntries = {})
{
    contour::test::writeConfigFile(
        session.config().configFile,
        contour::test::profileConfigYaml(session.profileName(), sections, profileEntries));
    REQUIRE(session(contour::actions::ReloadConfig {}));
}

/// Types @p keys into @p session, each one pressed and released, as a keyboard delivers them.
inline void typeKeys(contour::session::TerminalSession& session, std::string_view keys)
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

/// The line sessionCapturing() puts on the screen, the APC introducer a capture reply opens with, and
/// the empty terminating chunk that closes EVERY reply — including a refused one, which consists of
/// nothing else. Spelled out rather than derived from @c vtbackend::CaptureBufferCode, so a test pins
/// the bytes that go on the wire. @see docs/vt-extensions/buffer-capture.md
constexpr auto SeededLine = std::string_view { "capture me" };

constexpr auto CaptureReplyMarker = std::string_view { "\033^314;" };

/// Builds a session whose profile answers capture_buffer with @p permission, with a recording surface
/// attached — requestCaptureBuffer() early-returns without a display, so the gate is only reachable
/// with one. The screen is seeded and the PTY buffer cleared, so what a test reads back afterwards is
/// the capture and nothing else.
[[nodiscard]] inline SessionWithSurface sessionCapturing(contour::ContourGuiApp& app,
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
inline void captureWholePage(contour::session::TerminalSession& session)
{
    session.requestCaptureBuffer(session.terminal().pageSize().lines, vtbackend::CaptureLineMode::Physical);
}

/// Whether @p session's PTY holds a capture of the seeded screen. Both halves matter: capturing an
/// empty region still emits the closing marker, so the marker alone would not tell a capture that
/// carried the screen from one that carried nothing.
[[nodiscard]] inline bool capturedSeededScreen(contour::session::TerminalSession& session)
{
    auto const& written = mockPtyOf(session).stdinBuffer();
    return written.contains(CaptureReplyMarker) && written.contains(SeededLine);
}

/// @param path An absolute path.
/// @return @p path as a local file URL, '/'-separated on every platform, as QUrl wants it.
[[nodiscard]] inline QUrl localFileUrl(std::filesystem::path const& path)
{
    return QUrl::fromLocalFile(QString::fromStdU16String(path.generic_u16string()));
}

/// Writes an OSC-8 hyperlink cell to @p session's screen and hovers the mouse over it, so
/// terminal().tryGetHoveringHyperlink() resolves — the precondition FollowHyperlink needs to reach
/// TerminalSession::followHyperlink() without a display. @p uri is the OSC-8 target.
inline void seedHoveredHyperlink(contour::session::TerminalSession& session, std::string const& uri)
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

using contour::test::RecordingAnnouncer;

/// Runs a failing command through @p session's terminal as a shell with OSC 133 integration would:
/// prompt on row 0, its output on row 1, and the end mark with exit code 2. Then lets the GUI thread take
/// what the finish posted to it, as it would before the pointer could reach the block.
inline void runFailingCommand(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen(vtbackend::test::commandRun("make", { "boom" }, 2));
    QCoreApplication::processEvents();
}

/// A hit on @p segment beside grid line @p line, the page starting at screen row 0.
[[nodiscard]] inline contour::session::GutterHit gutterHit(int line, vtbackend::GutterSegmentKind segment)
{
    return { .gridLine = vtbackend::LineOffset(line),
             .screenRow = vtbackend::LineOffset(line),
             .segment = segment };
}

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

using contour::config::FinishNotificationConfig;

using contour::config::NotifyWhen;

using contour::platform::finishNotificationIdentifier;

using namespace std::chrono_literals;

/// Runs one shell-integrated command through @p session's parser -- prompt, typed input, output and exit
/// status, as an OSC 133 shell reports them -- and delivers what that posted to the GUI thread.
inline void runAndDeliverShellCommand(contour::session::TerminalSession& session,
                                      std::string_view commandLine,
                                      int exitCode)
{
    session.terminal().writeToScreen(vtbackend::test::commandRun(commandLine, { "output" }, exitCode));
    QCoreApplication::sendPostedEvents(&session, QEvent::MetaCall);
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
    /// @param bell The profile's `bell`; audible by default, whatever the edition's default is.
    explicit FinishNoticeHarness(FinishNotificationConfig const& policy,
                                 contour::config::Bell const& bell = contour::config::Bell {}):
        FinishNoticeHarness(policy, bell, std::make_unique<RecordingNotifier>())
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
    FinishNoticeHarness(FinishNotificationConfig const& policy,
                        contour::config::Bell const& bell,
                        std::unique_ptr<RecordingNotifier> notifier):
        recorder { notifier.get() },
        session { makeSessionWith(
            testApp.app(),
            std::make_unique<vtpty::MockPty>(TestPageSize),
            std::move(notifier),
            registerProfile(testApp.app(), "finish-notify", [&](contour::config::TerminalProfile& profile) {
                profile.notifyOnCommandFinish = policy;
                profile.bell = bell;
            })) }
    {
    }
};

using vtbackend::test::failOnDeadlock;

/// Turns local echo on (SRM reset), so that whatever the terminal sends is echoed, which parses. Clears what
/// doing so wrote to the PTY.
/// @param session The session.
inline void echoLocally(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033[12l");
    session.terminal().flushInput();
    mockPtyOf(session).stdinBuffer().clear();
}

/// Turns local echo on and in-band resize reports with it (DECSET 2048), so that a resize writes a report and
/// the report is echoed, which parses. Clears what doing so wrote to the PTY.
/// @param session The session.
inline void echoResizeReports(contour::session::TerminalSession& session)
{
    session.terminal().writeToScreen("\033[?2048h");
    echoLocally(session);
}

/// Whether what @p session wrote to its PTY opens with the in-band resize report of the page it has now.
/// @param session The session.
/// @return Whether the report leads.
[[nodiscard]] inline bool reportedItsPage(contour::session::TerminalSession& session)
{
    auto const page = session.terminal().pageSize();
    return mockPtyOf(session).stdinBuffer().starts_with(
        std::format("\033[48;{};{};", page.lines.value, page.columns.value));
}

} // namespace contour::test::sessionSupport
