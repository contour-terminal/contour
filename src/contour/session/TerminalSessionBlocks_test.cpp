// SPDX-License-Identifier: Apache-2.0
//
// TerminalSession tests, continued: detected paths and links, semantic blocks, copying and opening
// command output, and the command-finish notifications.

#include <contour/config/Actions.hpp>
#include <contour/input/MouseMapping.hpp>
#include <contour/platform/UserNotificationNotifier.hpp>
#include <contour/session/FontControl.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/SessionInput.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/session/TerminalSessionTestSupport.hpp>
#include <contour/test/AppFixture.hpp>
#include <contour/test/FakeDisplaySurface.hpp>
#include <contour/test/GuiTestFixtures.hpp>
#include <contour/test/RecordingAnnouncer.hpp>
#include <contour/test/RecordingUserNotificationCenter.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/screen/GutterSettings.hpp>
#include <vtbackend/shell/CommandBlock.hpp>
#include <vtbackend/testing/TestHelpers.hpp>

#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>

#include <core/Utf8Path.hpp>
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
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <sstream>
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

using contour::config::NotifyAction;

using contour::config::NotifyClearOn;

using contour::config::NotifyOutcome;

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
        REQUIRE_FALSE(session->blockActionsAnchor().isEmpty());

        session->detachDisplay(*session.surface);
        CHECK(session->blockActionsAnchor().isEmpty());
    }

    SECTION("the hyperlink's")
    {
        session->terminal().writeToScreen("\033]8;;https://example.com/\033\\link\033]8;;\033\\");
        session->sendMouseMoveEvent(vtbackend::Modifiers {},
                                    vtbackend::CellLocation { .line = vtbackend::LineOffset(0),
                                                              .column = vtbackend::ColumnOffset(1) },
                                    vtbackend::PixelCoordinate {});
        REQUIRE_FALSE(session->linkTargetText().isEmpty());

        session->detachDisplay(*session.surface);
        CHECK(session->linkTargetText().isEmpty());
    }
}

TEST_CASE("TerminalSession: the block menu's header says how the block ended, as of when it opens",
          "[contour][session][gutter][blockheader]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    session->terminal().writeToScreen(vtbackend::test::commandStarted("make"));
    auto const id = core::locked(session->terminal(), [&] {
        return session->terminal().commandBlockAt(vtbackend::LineOffset(0))->id;
    });

    auto const running = session->blockMenuState(id);
    REQUIRE(running.has_value());
    REQUIRE(running->header.size() >= 2);
    CHECK(running->header.front() == "make");
    CHECK(running->header[1].starts_with("Running"));

    session->terminal().writeToScreen(vtbackend::test::commandEnd(2));
    auto const failed = session->blockMenuState(id);
    REQUIRE(failed.has_value());
    CHECK(failed->header[1].starts_with("Failed · exit 2"));

    // A block the store does not hold has no menu.
    CHECK_FALSE(session->blockMenuState(vtbackend::CommandBlockId(id.value + 1000)).has_value());
}

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
        REQUIRE(
            app.manager().saveWindowLayout(win.id, "with-pager", contour::window::LayoutReplace::Replace));

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

TEST_CASE("TerminalSession: ToggleCommandFold folds the block it names, wherever the pointer is",
          "[contour][session][semanticblocks][folding]")
{
    TestApp testApp;
    auto session = makeSessionWithSurface(testApp.app());
    writeFinishedBlock(*session, "make", "built\r\nmore\r\n", ";cmdline_url=make");
    auto const* make = session->terminal().commandBlockAt(vtbackend::LineOffset(0));
    REQUIRE(make != nullptr);
    auto const head = make->headStableId;
    auto const pinned =
        contour::actions::ToggleCommandFold { .target = vtbackend::CommandBlockTarget::Pointer,
                                              .block = make->id };

    REQUIRE_FALSE(session->terminal().foldState().isCollapsed(head));
    CHECK((*session)(pinned));
    CHECK(session->terminal().foldState().isCollapsed(head));
    CHECK((*session)(pinned));
    CHECK_FALSE(session->terminal().foldState().isCollapsed(head));

    // A block the store no longer holds has nothing to fold.
    auto unknown = pinned;
    unknown.block = vtbackend::CommandBlockId(make->id.value + 1000);
    CHECK_FALSE((*session)(unknown));
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

        // Its facts head the menu: the command first, then how it ended.
        REQUIRE(state.blockUnderCursor->header.size() >= 2);
        CHECK(state.blockUnderCursor->header.front() == "make");
        CHECK_FALSE(state.blockUnderCursor->header[1].empty());

        // A fold hangs on its head while it has output, and the menu offers to close it...
        CHECK(state.blockUnderCursor->fold == contour::command::FoldPresence::Expanded);

        // ...and, once it is closed, to open it again.
        session->terminal().foldState().collapse(make->headStableId);
        session->sendMouseMoveEvent(
            Modifiers {},
            vtbackend::CellLocation { vtbackend::LineOffset(0), vtbackend::ColumnOffset(0) },
            pixels);
        CHECK(session->contextMenuState().blockUnderCursor->fold
              == contour::command::FoldPresence::Collapsed);
        session->terminal().foldState().expand(make->headStableId);

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

TEST_CASE("TerminalSession: a finish notification chimes only while the bell is on",
          "[contour][session][notification]")
{
    using vtbackend::NotificationSound;

    SECTION("an audible bell: the notification chimes")
    {
        FinishNoticeHarness const h { { .minDuration = 0s } };
        h.run("make", 0);
        REQUIRE(h.recorder->raised.size() == 1);
        CHECK(h.recorder->raised[0].sound == NotificationSound::Chime);
    }

    SECTION("bell.sound off: silent")
    {
        FinishNoticeHarness const h { { .minDuration = 0s }, contour::config::Bell { .sound = "off" } };
        h.run("make", 0);
        REQUIRE(h.recorder->raised.size() == 1);
        CHECK(h.recorder->raised[0].sound == NotificationSound::Silent);
    }

    SECTION("bell volume 0: silent")
    {
        FinishNoticeHarness const h { { .minDuration = 0s }, contour::config::Bell { .volume = 0.0f } };
        h.run("make", 0);
        REQUIRE(h.recorder->raised.size() == 1);
        CHECK(h.recorder->raised[0].sound == NotificationSound::Silent);
    }

    SECTION("notify_bell: the bell is the sound, so the notification is silent")
    {
        FinishNoticeHarness const h { { .minDuration = 0s, .action = NotifyAction::NotifyBell } };
        h.run("make", 0);
        REQUIRE(h.recorder->raised.size() == 1);
        CHECK(h.recorder->raised[0].sound == NotificationSound::Silent);
    }
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

    // History above the block and output below it, and the view parked at the very top: the click must
    // bring the block to the eye, not only its tab.
    auto& terminal = background->terminal();
    writeFiller(terminal, 60);
    runAndDeliverShellCommand(*background, "make", 2);
    REQUIRE(recorder->raised.size() == 1);
    CHECK(recorder->raised[0].title == "✗ make failed (exit 2)");
    writeFiller(terminal, 60);
    REQUIRE(core::locked(terminal, [&] { return terminal.viewport().scrollToTop(); }));
    auto const block = core::locked(terminal, [&] { return terminal.commandBlocks().lastCommand()->id; });

    recorder->fireActivated(finishNotificationIdentifier(background->id()));
    CHECK(window->activeSession() == background);

    // Its command row is at the top of the view, and flashes.
    auto const& grid = terminal.primaryScreen().grid();
    auto const* const record = terminal.commandBlocks().find(block);
    REQUIRE(record != nullptr);
    auto const head = vtbackend::headOffsetOf(grid, *record);
    REQUIRE(head.has_value());
    CHECK(terminal.viewport().isLineVisible(*head));
    CHECK(grid.lineText(terminal.viewport().topLine()).starts_with("$ make"));
    auto const flash = vtbackend::test::drawFrame(terminal).lineFlash;
    REQUIRE(flash.has_value());
    CHECK(flash->lineOffset == terminal.mainPageTopRow());

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: on macOS a finish notice goes to the notification centre, and its click finds "
          "the block",
          "[contour][session][notification][manager]")
{
    using namespace std::chrono_literals;

    // The session builds its own notifier, as in production: the host the app was given decides that it is
    // one over the centre, whatever OS the test runs on.
    auto centre = contour::test::RecordingUserNotificationCenter {};
    auto factory = std::make_unique<contour::test::MockPtySessionFactory>();
    factory->makeNotifier = nullptr;
    TestApp testApp { std::move(factory),
                      nullptr,
                      nullptr,
                      nullptr,
                      contour::test::TestApp::testHostIdentity(),
                      nullptr,
                      {},
                      nullptr,
                      contour::test::withUserNotifications(contour::platform::nullAppearanceServices(),
                                                           centre,
                                                           contour::config::HostPlatform::MacOS) };

    contour::test::ScopedController const window { testApp.manager() };
    window->createNewTab();
    auto* const background = window->activeSession();
    window->createNewTab();
    REQUIRE(background != nullptr);
    REQUIRE(window->activeSession() != background);

    auto& terminal = background->terminal();
    writeFiller(terminal, 60);
    // Twelve seconds, past the default policy's ten, in a tab the user is not looking at.
    terminal.writeToScreen(vtbackend::test::commandStarted("make"));
    testApp.steadyClock().advance(12s);
    terminal.writeToScreen(vtbackend::test::commandEnd(2));
    QCoreApplication::sendPostedEvents(background, QEvent::MetaCall);

    auto const requestId = contour::platform::sessionRequestPrefix(background->id())
                           + finishNotificationIdentifier(background->id());
    REQUIRE(centre.posted.size() == 1);
    CHECK(centre.posted[0].requestId == requestId);
    CHECK(centre.posted[0].title == "✗ make failed (exit 2)");
    CHECK(centre.posted[0].threadId == contour::platform::sessionRequestPrefix(background->id()));

    writeFiller(terminal, 60);
    REQUIRE(core::locked(terminal, [&] { return terminal.viewport().scrollToTop(); }));
    auto const block = core::locked(terminal, [&] { return terminal.commandBlocks().lastCommand()->id; });

    REQUIRE(centre.activate(requestId));
    CHECK(window->activeSession() == background);
    auto const& grid = terminal.primaryScreen().grid();
    auto const* const record = terminal.commandBlocks().find(block);
    REQUIRE(record != nullptr);
    auto const head = vtbackend::headOffsetOf(grid, *record);
    REQUIRE(head.has_value());
    CHECK(terminal.viewport().isLineVisible(*head));

    contour::test::closeAllTabs(*window);
}

TEST_CASE("TerminalSession: a click on a notice after its session is gone does nothing",
          "[contour][session][notification]")
{
    auto centre = contour::test::RecordingUserNotificationCenter {};
    TestApp testApp;
    auto const prefix = std::string { "contour-session-gone/" };
    auto session = makeSessionWith(
        testApp.app(),
        std::make_unique<vtpty::MockPty>(TestPageSize),
        std::make_unique<contour::platform::UserNotificationNotifier>(centre, prefix, 0s, nullptr),
        registerProfile(testApp.app(), "finish-notify", [](contour::config::TerminalProfile& profile) {
            profile.notifyOnCommandFinish = FinishNotificationConfig { .minDuration = 0s };
        }));
    runAndDeliverShellCommand(*session, "make", 2);
    REQUIRE(centre.posted.size() == 1);
    auto const requestId = centre.posted[0].requestId;
    REQUIRE(requestId.starts_with(prefix));
    REQUIRE(centre.subscriberCount() == 1);

    testApp.manager().setFocusedSession(nullptr);
    session.reset();

    // The notifier went with the session: it withdrew what it raised and left the centre.
    CHECK(centre.subscriberCount() == 0);
    CHECK(centre.withdrawn == std::vector<std::string> { requestId });
    CHECK_FALSE(centre.activate(requestId));
}

TEST_CASE("TerminalSession: a test app's notification centre is the null one, and its notices stay with "
          "the notifier the test gave",
          "[contour][session][notification]")
{
    FinishNoticeHarness h { { .minDuration = 0s } };

    CHECK(h.testApp.app().userNotificationCenter().availability()
          == contour::platform::UserNotificationAvailability::Unavailable);
    h.run("make", 2);
    CHECK(h.recorder->raised.size() == 1);
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
    // Nothing to scroll through yet: no bar.
    CHECK_FALSE(held->getIsScrollbarVisible());
    writeFiller(held->terminal(), 60);
    held->refreshScrollbarMarks();
    CHECK(held->getIsScrollbarVisible());

    // hide_in_alt_screen keeps its default, so vim or htop keep their last column.
    held->terminal().writeToScreen("\033[?1049h");
    CHECK_FALSE(held->getIsScrollbarVisible());

    held->terminal().writeToScreen("\033[?1049l");
    CHECK(held->getIsScrollbarVisible());
}

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
