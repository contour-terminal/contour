// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/ContourGuiApp.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/test/GuiTestFixtures.hpp>

#include <core/platform/NativeFileSystem.hpp>

#include <QtCore/QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace contour::test
{

/// Builds a ContourGuiApp whose parameters() are populated with defaults (so profileName() resolves
/// to the default "main" profile) without running the GUI. The default-constructed config already
/// seeds a "main" profile and a "default" colorscheme, so no config file is needed. The app's real
/// session manager is usable headlessly as long as no PTY is spawned (tabs minted straight through
/// the vtworkspace model have no backing sessions).
class TestApp
{
  public:
    /// @param factory Optional PTY factory override; pass a MockPtySessionFactory (keep a raw
    ///                observation pointer first) to run session-creation paths headlessly. Any other
    ///                is installed inside a TestNotifierSessionFactory, @see withTestNotifier().
    /// @param layoutStore Optional layout-persistence override; pass an InMemoryLayoutStore (keep a
    ///                raw observation pointer first) to drive SaveLayout without touching the disk.
    /// @param commandHistoryStore Optional command-history override; pass an
    ///                InMemoryCommandHistoryStore (keep a raw observation pointer first) to drive the
    ///                command palette's MRU persistence without touching the disk.
    ///
    /// @param speech Optional speech override; pass a RecordingSpeechSynthesizer (keep a raw
    ///                observation pointer first) to drive the read-aloud path. @see defaultSpeech()
    ///                for what a test app gets without one.
    /// @param identity The machine the app's sessions run on. @see testHostIdentity() for the default.
    /// @param fileSystem Where the app's sessions look up paths, link detection's probes too; null for the
    ///                fixture's own in-memory one, fileSystem(). Borrowed.
    /// @param environment The environment variables the app reads as the test sets them.
    /// @param taskbarProgress Optional native taskbar sink override.
    /// @param appearance The OS accessibility and appearance sources. The null ones by default, so a test
    ///                never reads the host's settings; pass test::appearanceServicesWith() to drive them.
    /// @param shellRcEditor The editor of the shells' startup files. A RecordingShellRcEditor by default,
    ///                so no test run reaches a real rc file; @see shellRcEditor().
    /// @param attachedHostLabel The remote host the app counts as attached to; empty: this machine.
    /// @param shellHost The host the shell-integration offer decides on. A POSIX one by default, so the offer
    ///                behaves the same on every machine running the tests; pass HostPlatform::Windows to see
    ///                it refuse.
    explicit TestApp(
        std::unique_ptr<contour::session::SessionFactory> factory = nullptr,
        std::unique_ptr<contour::config::LayoutStore> layoutStore = nullptr,
        std::unique_ptr<contour::command::CommandHistoryStore> commandHistoryStore = nullptr,
        std::unique_ptr<contour::platform::SpeechSynthesizer> speech = nullptr,
        vtbackend::HostIdentity identity = testHostIdentity(),
        core::platform::FileSystem const* fileSystem = nullptr,
        TestEnvironment::Overrides environment = {},
        std::unique_ptr<contour::platform::TaskbarProgressSink> taskbarProgress = nullptr,
        contour::platform::AppearanceServices const& appearance = contour::platform::nullAppearanceServices(),
        std::unique_ptr<contour::platform::IShellRcEditor> shellRcEditor = nullptr,
        std::string attachedHostLabel = {},
        contour::config::HostPlatform shellHost = contour::config::HostPlatform::Other):
        _environment { std::move(environment) },
        _sessionFiles { fileSystem ? fileSystem : &_fileSystem },
        _app(_environment,
             withTestNotifier(std::move(factory)),
             makeRecordingLauncher(),
             std::move(layoutStore),
             std::move(commandHistoryStore),
             speechOrDefault(std::move(speech)),
             makeOutputFiles(),
             clocks(),
             std::move(identity),
             _sessionFiles,
             nullptr,
             linkServices(),
             std::move(taskbarProgress),
             std::make_unique<InMemoryWindowPositionStore>(),
             appearance,
             core::platform::defaultSteadyClock().now(),
             settingsWindowFactory(),
             shellIntegrationServices(std::move(shellRcEditor), std::move(attachedHostLabel), shellHost),
             scriptedDragEndProbe())
    {
        parseTerminalVerb();
    }

    /// A test app whose sessions run on @p identity: for a test of what counts as this machine.
    /// @param identity The machine the app's sessions run on.
    explicit TestApp(vtbackend::HostIdentity identity):
        TestApp(nullptr, nullptr, nullptr, nullptr, std::move(identity))
    {
    }

    /// A test app whose processes are placed by @p placement, and which opens and starts programs with the
    /// app's own default launcher rather than a recording one -- so launcher() is not available on it.
    /// @param placement Places what the app's default session factory and launcher start.
    explicit TestApp(std::shared_ptr<vtpty::ProcessPlacement> placement):
        _sessionFiles { &_fileSystem },
        _app(_environment,
             nullptr,
             nullptr,
             nullptr,
             nullptr,
             defaultSpeech(),
             makeOutputFiles(),
             clocks(),
             testHostIdentity(),
             _sessionFiles,
             std::move(placement),
             linkServices(),
             nullptr,
             std::make_unique<InMemoryWindowPositionStore>(),
             contour::platform::nullAppearanceServices(),
             core::platform::defaultSteadyClock().now(),
             settingsWindowFactory(),
             shellIntegrationServices(),
             scriptedDragEndProbe())
    {
        parseTerminalVerb();
    }

    /// Where the sessions of a test app look up paths, link detection's probes included.
    enum class SessionFiles : uint8_t
    {
        /// The fixture's own in-memory filesystem, fileSystem(), which the test fills: the default.
        Fixture = 0,
        ThisMachine, ///< This machine's: for a test of what the disk holds.
    };

    /// A test app whose sessions look up paths where @p files says: by default the fixture's own filesystem,
    /// the one its command output files are on too.
    /// @param files Where the app's sessions look up paths.
    /// @param environment The environment variables the app reads as the test sets them.
    explicit TestApp(SessionFiles files, TestEnvironment::Overrides environment = {}):
        TestApp(nullptr,
                nullptr,
                nullptr,
                nullptr,
                testHostIdentity(),
                files == SessionFiles::ThisMachine ? &core::platform::NativeFileSystem::instance() : nullptr,
                std::move(environment))
    {
    }

    /// Parses an explicit argv so a test can drive a non-"terminal" subcommand (e.g. font-locator,
    /// info, documentation) whose action reads its own contour.<cmd>.* parameters.
    /// @param args The command tokens after the program name (e.g. {"font-locator"}).
    explicit TestApp(std::initializer_list<char const*> args,
                     std::unique_ptr<contour::session::SessionFactory> factory = nullptr):
        _sessionFiles { &_fileSystem },
        _app(_environment,
             withTestNotifier(std::move(factory)),
             makeRecordingLauncher(),
             nullptr,
             nullptr,
             defaultSpeech(),
             makeOutputFiles(),
             clocks(),
             testHostIdentity(),
             _sessionFiles,
             nullptr,
             linkServices(),
             nullptr,
             std::make_unique<InMemoryWindowPositionStore>(),
             contour::platform::nullAppearanceServices(),
             core::platform::defaultSteadyClock().now(),
             settingsWindowFactory(),
             shellIntegrationServices(),
             scriptedDragEndProbe())
    {
        std::vector<char const*> argv;
        argv.reserve(args.size() + 1);
        argv.push_back("contour");
        for (auto const* a: args)
            argv.push_back(a);
        REQUIRE(_app.parseParametersForTesting(static_cast<int>(argv.size()), argv.data()));
    }

    TestApp(TestApp const&) = delete;
    TestApp& operator=(TestApp const&) = delete;
    TestApp(TestApp&&) = delete;
    TestApp& operator=(TestApp&&) = delete;

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _app; }
    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept
    {
        return _app.sessionsManager();
    }
    /// The drag-end probe wired into this app: a test says through it where a drag no window took ended.
    [[nodiscard]] ScriptedDragEndProbe& dragEndProbe() noexcept { return *_dragEndProbe; }
    /// The recording launcher wired into this app (records URL-open / process-spawn requests).
    [[nodiscard]] RecordingExternalLauncher& launcher() noexcept
    {
        REQUIRE(_launcher != nullptr); // not on an app built with the default launcher
        return *_launcher;
    }

    /// The recording editor of the shells' startup files wired into this app.
    [[nodiscard]] RecordingShellRcEditor& shellRcEditor() noexcept
    {
        REQUIRE(_shellRcEditor != nullptr); // not on an app handed another editor
        return *_shellRcEditor;
    }

    /// What the app's GUI state remembers, in memory.
    [[nodiscard]] InMemoryGuiStateStore& guiState() noexcept { return *_guiState; }

    /// The binary a test app's rc line names.
    static constexpr std::string_view ContourBinary = "/opt/contour/bin/contour";

    /// Where OpenCommandOutput's files go in a test app: a directory on the in-memory filesystem.
    static constexpr std::string_view OutputDirectory = "/run/contour-test-output";

    /// The host name of the machine a test app's sessions run on, unless the test names another.
    static constexpr std::string_view HostName = "contour-test-host";

    /// The machine a test app's sessions run on, unless the test names another: a fixed one, so that what
    /// counts as this machine does not depend on the machine the suite runs on.
    [[nodiscard]] static vtbackend::HostIdentity testHostIdentity()
    {
        return vtbackend::HostIdentity { .machineId = "0123456789abcdef0123456789abcdef",
                                         .hostname = std::string { HostName } };
    }

    /// The in-memory filesystem the app's command output files are written to, and, unless the test chose
    /// this machine's (SessionFiles::ThisMachine), the one every session looks paths up in.
    [[nodiscard]] core::platform::testing::InMemoryFileSystem& fileSystem() noexcept { return _fileSystem; }

    /// Runs a whole link detection cycle on @p session's terminal, as a display would over the quiet time
    /// after output: a frame, the match job, a frame, the probes, a frame.
    /// @param session A session of this app.
    void detectLinks(contour::session::TerminalSession& session)
    {
        auto& terminal = session.terminal();
        terminal.refreshRenderBuffer();
        terminal.tick(terminal.currentTime() + vtbackend::LinkDetectorTuning {}.debounce);
        terminal.refreshRenderBuffer();
        _linkMatch.drain();
        terminal.refreshRenderBuffer();
        _linkProbe.drain();
        terminal.refreshRenderBuffer();
    }

    /// Runs the path probes the app's sessions have handed the probe thread, a follow gesture's included.
    void drainLinkProbes() { _linkProbe.drain(); }

    /// The steady clock every session's terminal reads: it moves only when a test advances it.
    [[nodiscard]] core::platform::ManualClock& steadyClock() noexcept { return _steadyClock; }

    /// The wall clock every session's terminal reads: it moves only when a test advances it.
    [[nodiscard]] core::platform::ManualWallClock& wallClock() noexcept { return _wallClock; }

  private:
    /// Parses the "terminal" subcommand, so that parameters() carries every contour.terminal.* default
    /// (profile, dump-state-at-exit, ...) that TerminalSession reads during construction.
    void parseTerminalVerb()
    {
        // The GUI suite drives mechanisms (link following, the bell, page sizing, ...) from the app's
        // configuration before any file is loaded, and it was written against the defaults every release
        // before the 2026 redesign shipped: Ctrl follows links, the bell sounds, a page is 80x25. A plain
        // Config is DefaultsEdition::Native2026, so the fixture states the edition its expectations are
        // in. The Native2026 values themselves are pinned by Config.firstLaunchDefaults.
        _app.config() =
            contour::config::Config { .defaultsEdition = contour::config::DefaultsEdition::Classic };

        char const* argv[] = { "contour", "terminal" };
        REQUIRE(_app.parseParametersForTesting(2, argv));
    }

    /// The synthesizer a test app gets when the test does not name one.
    ///
    /// The NULL one, never the production synthesizer -- and this is the single place that decides
    /// so, because it is an invariant of the suite rather than a default anyone should vary. A real
    /// synthesizer connects to the machine's speech service (speech-dispatcher on Linux) as soon as
    /// a session is asked for its context-menu state: surprising for a headless suite, audible if
    /// the machine has a voice, and a leak inside libspeechd on every connection.
    [[nodiscard]] static std::unique_ptr<contour::platform::SpeechSynthesizer> defaultSpeech()
    {
        return std::make_unique<contour::platform::NullSpeechSynthesizer>();
    }

    /// The synthesizer a test names, or defaultSpeech() when it names none.
    /// @param speech The test's synthesizer; null for none.
    /// @return @p speech, or the default one.
    [[nodiscard]] static std::unique_ptr<contour::platform::SpeechSynthesizer> speechOrDefault(
        std::unique_ptr<contour::platform::SpeechSynthesizer> speech)
    {
        if (speech)
            return speech;
        return defaultSpeech();
    }

    /// The app's command output files, on the fixture's in-memory filesystem.
    [[nodiscard]] std::unique_ptr<contour::session::CommandOutputFiles> makeOutputFiles() const
    {
        return std::make_unique<contour::session::CommandOutputFiles>(
            _fileSystem, _processLiveness, std::filesystem::path(OutputDirectory));
    }

    /// What the app's sessions detect links with: the fixture's manual executors, which detectLinks() drains,
    /// the filesystem the app gets and a manual clock.
    [[nodiscard]] vtbackend::LinkDetectionServices linkServices() noexcept
    {
        return vtbackend::LinkDetectionServices { .matchExecutor = _linkMatch,
                                                  .probeExecutor = _linkProbe,
                                                  .fileSystem = *_sessionFiles,
                                                  .clock = _linkClock,
                                                  .tuning = {} };
    }

    /// The clocks the app's sessions read: the fixture's manual ones, in this process's time zone.
    [[nodiscard]] vtbackend::TerminalClocks clocks() noexcept
    {
        return vtbackend::TerminalClocks { .steady = _steadyClock,
                                           .wall = _wallClock,
                                           .localTime = &vtbackend::systemLocalTime };
    }

    /// Builds the recording launcher, stashing a raw observation pointer before handing ownership to
    /// the app. Runs in the member-initializer list, so _launcher is set before _app is constructed.
    std::unique_ptr<contour::platform::ExternalLauncher> makeRecordingLauncher()
    {
        auto launcher = std::make_unique<RecordingExternalLauncher>();
        launcher->readInput = [this](std::filesystem::path const& file) {
            return _fileSystem.readFile(file).value_or(std::string {});
        };
        _launcher = launcher.get();
        return launcher;
    }

    /// What the offer to install shell integration reads and writes in a test app: a recording editor (or the
    /// one a test handed), the in-memory GUI state, a fixed binary and, optionally, a remote host.
    /// Runs in the member-initializer list, so the raw pointers are set before _app is constructed.
    [[nodiscard]] contour::ShellIntegrationServices shellIntegrationServices(
        std::unique_ptr<contour::platform::IShellRcEditor> own = nullptr,
        std::string attachedHostLabel = {},
        contour::config::HostPlatform shellHost = contour::config::HostPlatform::Other)
    {
        auto editor = own ? std::move(own) : std::make_unique<RecordingShellRcEditor>();
        _shellRcEditor = dynamic_cast<RecordingShellRcEditor*>(editor.get());
        return contour::ShellIntegrationServices { .editor = std::move(editor),
                                                   .state = _guiState,
                                                   .contourBinary = std::filesystem::path { ContourBinary },
                                                   .attachedHostLabel = std::move(attachedHostLabel),
                                                   .host = shellHost };
    }

    /// The probe the app decides drag ends with. Runs in the member-initializer list, so the raw pointer is
    /// set before _app is constructed.
    [[nodiscard]] std::unique_ptr<contour::window::DragEndProbe> scriptedDragEndProbe()
    {
        auto probe = std::make_unique<ScriptedDragEndProbe>();
        _dragEndProbe = probe.get();
        return probe;
    }

    /// Builds the app's Settings window offscreen, on an engine of the fixture's own: the app never builds
    /// its own engine in a test. The engine is made on the first show, with the chrome tokens installed.
    /// Runs in the member-initializer list; what it returns is called only once _app exists.
    [[nodiscard]] contour::window::SettingsWindowFactory settingsWindowFactory()
    {
        return [this](contour::window::SettingsWindowHost& host) {
            if (!_settingsEngine)
            {
                _settingsEngine = std::make_unique<QQmlEngine>();
                installChromeStyle(*_settingsEngine);
                // The preview is a terminal display, which only the GUI action registers: without the stub
                // the window fails to load whenever no earlier case of the run registered one.
                qmlRegisterType<StubContourTerminal>("Contour.Terminal", 1, 0, "ContourTerminal");
            }
            return contour::window::loadSettingsWindow(*_settingsEngine, host);
        };
    }

    RecordingExternalLauncher* _launcher = nullptr;
    RecordingShellRcEditor* _shellRcEditor = nullptr;
    ScriptedDragEndProbe* _dragEndProbe = nullptr;
    std::shared_ptr<InMemoryGuiStateStore> _guiState = std::make_shared<InMemoryGuiStateStore>();
    /// The process's own environment: what the fixture exercises is the GUI, not what any variable
    /// reads as. A test that cares sets the names it needs (TestApp(SessionFiles,
    /// TestEnvironment::Overrides)).
    TestEnvironment _environment;
    /// Declared before _app, whose sessions borrow them. They start at the time the app is made, so that
    /// times read from them look like times the suite runs at.
    core::platform::ManualClock _steadyClock { core::platform::defaultSteadyClock().now() };
    core::platform::ManualWallClock _wallClock { core::platform::defaultSystemWallClock().now() };
    /// Declared before _app, which borrows it for its command output files and its sessions' link detection.
    core::platform::testing::InMemoryFileSystem _fileSystem { core::platform::testing::FileEntry {
        .path = "/run", .isDirectory = true } };
    /// What the app's command output files are told about other processes: nothing, so they keep
    /// whatever they find.
    UnknownProcessLiveness _processLiveness;
    /// Declared before _app, whose sessions' link detection borrows them, and after _fileSystem: the jobs
    /// still queued are dropped, never run, when the executors go, so nothing reads the filesystem then.
    core::platform::ManualClock _linkClock;
    core::async::testing::ManualExecutor _linkMatch; ///< Link matching; detectLinks() drains it.
    core::async::testing::ManualExecutor _linkProbe; ///< Path probes; detectLinks() drains it.
    /// Where the app's sessions look up paths, its link detection too: _fileSystem unless the test chose
    /// another. Declared before _app, which borrows it.
    core::platform::FileSystem const* _sessionFiles = nullptr;
    /// The engine the app's Settings window is built on; see settingsWindowFactory(). Declared before _app,
    /// so the window the app owns goes before the engine it was built on.
    std::unique_ptr<QQmlEngine> _settingsEngine;
    contour::ContourGuiApp _app;
};

/// A TestApp whose configuration is a file of its own, in a directory that lives as long as the app: what a
/// test that reloads the configuration from disk needs. Its sessions look up paths in the fixture's own
/// in-memory file system unless the test chooses this machine's: what a test of Settings needs, because
/// Settings writes its side files through the app's file system, and the configuration loader reads them back
/// from the disk.
class ConfiguredTestApp
{
  public:
    /// @param factory Optional PTY factory override, as TestApp takes it.
    /// @param appearance The OS accessibility and appearance sources, as TestApp takes them.
    /// @param files      Where the app's sessions look up paths, and Settings writes its side files.
    explicit ConfiguredTestApp(
        std::unique_ptr<contour::session::SessionFactory> factory = nullptr,
        contour::platform::AppearanceServices const& appearance = contour::platform::nullAppearanceServices(),
        TestApp::SessionFiles files = TestApp::SessionFiles::Fixture):
        _testApp { std::move(factory),
                   nullptr,
                   nullptr,
                   nullptr,
                   TestApp::testHostIdentity(),
                   files == TestApp::SessionFiles::ThisMachine ? &core::platform::NativeFileSystem::instance()
                                                               : nullptr,
                   {},
                   nullptr,
                   appearance },
        _configFile { std::filesystem::path(_configDir.path().toStdU16String()) / "contour.yml" }
    {
        REQUIRE(_configDir.isValid());
        _testApp.app().config().configFile = _configFile;
    }

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _testApp.app(); }
    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept { return _testApp.manager(); }

    /// @return The file the app's configuration is read from.
    [[nodiscard]] std::filesystem::path const& configFile() const noexcept { return _configFile; }

    /// @return What the app opened URLs and programs with, as TestApp::launcher() gives it.
    [[nodiscard]] RecordingExternalLauncher& launcher() noexcept { return _testApp.launcher(); }

  private:
    QTemporaryDir _configDir; ///< First, so it outlives the app that reads from it.
    TestApp _testApp;
    std::filesystem::path _configFile;
};

/// Runs the menu bar row titled @p title of @p tree through the app's own menu bar path, at the row's scope.
/// @param app   The test app whose app() runs the row.
/// @param tree  The menu bar's tree.
/// @param title The row's title; the row must exist.
inline void runMenuBarRow(TestApp& app, contour::command::MenuTree const& tree, std::string_view title)
{
    auto const* row = nodeTitled(tree.nodes, title);
    REQUIRE(row != nullptr);
    app.app().runMenuBarAction(tree.actions[row->actionIndex], row->scope);
}

} // namespace contour::test
