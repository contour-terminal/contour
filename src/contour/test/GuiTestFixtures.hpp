// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/ContourGuiApp.hpp>
#include <contour/command/CommandCatalog.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/command/MenuTree.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DefaultsEdition.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/session/CommandOutputFiles.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/TerminalSession.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/test/CoreFixtures.hpp>
#include <contour/test/LauncherFixtures.hpp>
#include <contour/test/QmlChromeStyle.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/WindowController.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/ChannelPty.hpp>
#include <vtpty/MockPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <core/Environment.hpp>
#include <core/async/testing/ManualExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/testing/InMemoryFileSystem.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QEvent>
#include <QtCore/QTemporaryDir>
#include <QtQml/QQmlEngine>
#include <QtQuick/QQuickWindow>

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::test
{

/// The notifier every session a test makes raises through, unless the test wants another.
///
/// It raises nothing. The platform's own on Linux is a FreeDesktopNotifier over the machine's session
/// bus, and no test may reach that bus: test_main.cpp fails a run that does. Pass it as a session's
/// notifier wherever a test constructs a TerminalSession itself; the factories below hand it out.
/// @return The notifier.
[[nodiscard]] inline std::unique_ptr<contour::platform::Notifier> makeTestNotifier()
{
    return std::make_unique<contour::platform::NullNotifier>();
}

/// In-memory SessionFactory: every created session is backed by a vtpty::MockPty, so the manager's
/// session-creation paths (new tab, new split pane) run headlessly — no process is spawned, and a
/// test can seed/inspect each PTY's buffers. Records the cwd handed to each creation so
/// working-directory inheritance is assertable.
class MockPtySessionFactory final: public contour::session::SessionFactory
{
  public:
    /// The page size a created PTY falls back to when the caller passes no explicit @c pageSize (a
    /// brand-new window). Mirrors the profile default a real AppSessionFactory would use.
    static constexpr auto DefaultPageSize =
        vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) };

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        requestedCwds.push_back(std::move(cwd));
        // Record the size the manager asked for (nullopt == "use the profile default"), so a test can
        // assert that a new tab/split inherited the running window size instead of the default.
        requestedPageSizes.push_back(pageSize);
        requestedCommandOverrides.push_back(std::move(commandOverride));
        requestedProfileNames.push_back(std::move(profileName));
        auto const initialSize = pageSize.value_or(DefaultPageSize);
        auto pty = std::make_unique<vtpty::MockPty>(initialSize);
        createdPtys.push_back(pty.get());
        return pty;
    }

    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return reportedHosting;
    }

    [[nodiscard]] std::unique_ptr<contour::platform::Notifier> createNotifier() override
    {
        return makeNotifier ? makeNotifier() : nullptr;
    }

    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* /*pty*/, contour::session::HostedOperationRequest const& request) override
    {
        requestedHostedOperations.push_back(request);
        return hostedOperationRoute;
    }

    /// What hosting() reports: Remote stands in for an attach-mode factory, which runs no command here.
    contour::session::SessionHosting reportedHosting = contour::session::SessionHosting::Local;
    /// How createNotifier() makes each session's notifier: makeTestNotifier() unless a test says
    /// otherwise. A test that wants the platform's own -- the window's tray, off Linux -- unsets it.
    std::function<std::unique_ptr<contour::platform::Notifier>()> makeNotifier = makeTestNotifier;
    /// What requestHostedOperation() answers: RunOnHost stands in for a daemon, Refused for a tmux pane
    /// that has no command for the operation.
    contour::session::HostedOperationRoute hostedOperationRoute =
        contour::session::HostedOperationRoute::RunHere;
    /// Every request requestHostedOperation() was asked about, in order.
    std::vector<contour::session::HostedOperationRequest> requestedHostedOperations;
    std::vector<std::optional<std::string>> requestedCwds;
    /// The @c pageSize argument of each createPty() call (nullopt when the caller requested the default).
    std::vector<std::optional<vtbackend::PageSize>> requestedPageSizes;
    /// The @c commandOverride argument of each createPty() call.
    std::vector<std::optional<vtpty::Process::ExecInfo>> requestedCommandOverrides;
    /// The @c profileName argument of each createPty() call.
    std::vector<std::optional<std::string>> requestedProfileNames;
    /// Non-owning observation pointers; valid while the owning session lives.
    std::vector<vtpty::MockPty*> createdPtys;
};

/// A TerminalSession a test builds itself, outside any session factory: over @p pty, raising through
/// @p notifier, or makeTestNotifier() when it is given none -- never the platform's own.
/// @param manager     The manager it reports to.
/// @param pty         Its PTY (owned from here on).
/// @param app         The application it runs in.
/// @param profileName The profile it runs under; empty takes the app's default one.
/// @param notifier    The notifier to raise through; null takes makeTestNotifier().
/// @return The session; the caller owns it.
[[nodiscard]] inline std::unique_ptr<contour::session::TerminalSession> makeTestSession(
    contour::session::TerminalSessionManager* manager,
    std::unique_ptr<vtpty::Pty> pty,
    contour::ContourGuiApp& app,
    std::string profileName = {},
    std::unique_ptr<contour::platform::Notifier> notifier = nullptr)
{
    return std::make_unique<contour::session::TerminalSession>(manager,
                                                               std::move(pty),
                                                               app,
                                                               std::move(profileName),
                                                               std::nullopt,
                                                               std::nullopt,
                                                               notifier ? std::move(notifier)
                                                                        : makeTestNotifier());
}

/// A real session factory with every call passed through, except that a session it would hand the
/// platform's own notifier raises through makeTestNotifier() instead.
///
/// For the factories a test cannot otherwise give a notifier: the remote controllers the attach tests
/// drive are the real ones, and final. TestApp wraps every factory but the harness's own in one.
class TestNotifierSessionFactory final: public contour::session::SessionFactory
{
  public:
    /// @param factory The factory every call is passed to (owned).
    explicit TestNotifierSessionFactory(std::unique_ptr<contour::session::SessionFactory> factory):
        _factory { std::move(factory) }
    {
    }

    [[nodiscard]] std::unique_ptr<contour::platform::Notifier> createNotifier() override
    {
        if (auto notifier = _factory->createNotifier())
            return notifier;
        return makeTestNotifier();
    }

    [[nodiscard]] std::unique_ptr<vtpty::Pty> createPty(
        std::optional<std::string> cwd,
        std::optional<vtbackend::PageSize> pageSize = std::nullopt,
        std::optional<vtpty::Process::ExecInfo> commandOverride = std::nullopt,
        std::optional<std::string> profileName = std::nullopt) override
    {
        return _factory->createPty(
            std::move(cwd), pageSize, std::move(commandOverride), std::move(profileName));
    }

    [[nodiscard]] bool canCreateSession() const noexcept override { return _factory->canCreateSession(); }

    [[nodiscard]] contour::session::SessionHosting hosting() const noexcept override
    {
        return _factory->hosting();
    }

    [[nodiscard]] bool requestRemoteTab(vtpty::Pty const* actingPty,
                                        contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteTab(actingPty, spawn);
    }

    [[nodiscard]] bool requestRemoteSplit(vtpty::Pty const* actingPty,
                                          bool vertical,
                                          contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteSplit(actingPty, vertical, spawn);
    }

    void reportSplitRatio(vtpty::Pty const* firstPty, vtpty::Pty const* secondPty, double ratio) override
    {
        _factory->reportSplitRatio(firstPty, secondPty, ratio);
    }

    void reportTabPresentation(contour::session::LocalTabPresentation const& presentation) override
    {
        _factory->reportTabPresentation(presentation);
    }

    [[nodiscard]] bool requestRemoteWindow(contour::session::RemoteSpawn const& spawn) override
    {
        return _factory->requestRemoteWindow(spawn);
    }

    void requestRemoteClose(vtpty::Pty const* pty) override { _factory->requestRemoteClose(pty); }

    [[nodiscard]] contour::session::HostedOperationRoute requestHostedOperation(
        vtpty::Pty const* pty, contour::session::HostedOperationRequest const& request) override
    {
        return _factory->requestHostedOperation(pty, request);
    }

    void bindTerminal(vtpty::Pty const* pty, vtbackend::Terminal& terminal) override
    {
        _factory->bindTerminal(pty, terminal);
    }

  private:
    std::unique_ptr<contour::session::SessionFactory> _factory;
};

/// @p factory as TestApp installs it: the harness's own MockPtySessionFactory decides each session's
/// notifier itself, and any other is wrapped in a TestNotifierSessionFactory.
/// @param factory The factory a test handed TestApp, or null for the application's own.
/// @return The factory to install.
[[nodiscard]] inline std::unique_ptr<contour::session::SessionFactory> withTestNotifier(
    std::unique_ptr<contour::session::SessionFactory> factory)
{
    if (factory == nullptr || dynamic_cast<MockPtySessionFactory const*>(factory.get()) != nullptr)
        return factory;
    return std::make_unique<TestNotifierSessionFactory>(std::move(factory));
}

/// A ProcessLiveness that can tell nothing about any process.
class UnknownProcessLiveness final: public contour::session::ProcessLiveness
{
  public:
    [[nodiscard]] contour::session::ProcessState stateOf(int64_t /*processId*/) const override
    {
        return contour::session::ProcessState::Unknown;
    }
};

/// The MockPty backing @p session, for seeding output and inspecting the bytes the terminal wrote
/// towards the shell (key/mouse encodings, replies, focus events).
/// @param session A session created over a MockPty (via MockPtySessionFactory or directly).
/// @return The session's PTY, downcast. Throws std::bad_cast if it is not a MockPty.
[[nodiscard]] inline vtpty::MockPty& mockPtyOf(contour::session::TerminalSession& session)
{
    return dynamic_cast<vtpty::MockPty&>(session.terminal().device());
}

/// An in-memory LayoutStore: SaveLayout runs end to end (serialize -> persist -> re-read) with no
/// filesystem at all, and a test can inspect exactly what was handed to persistence. @c loadError
/// makes the store report an unreadable backing file, to exercise the refuse-rather-than-destroy
/// path without corrupting a real one.
class InMemoryLayoutStore final: public contour::config::LayoutStore
{
  public:
    [[nodiscard]] std::expected<contour::config::LayoutMap, std::string> load(
        std::filesystem::path const& path) const override
    {
        loadedPaths.push_back(path);
        if (loadError)
            return std::unexpected(*loadError);
        return layouts;
    }

    [[nodiscard]] std::expected<void, std::string> save(std::filesystem::path const& path,
                                                        contour::config::LayoutMap const& newLayouts) override
    {
        savedPaths.push_back(path);
        if (saveError)
            return std::unexpected(*saveError);
        layouts = newLayouts;
        return {};
    }

    /// The store's contents (seed it to model a pre-existing layouts.yml; read it back to assert
    /// what SaveLayout persisted).
    contour::config::LayoutMap layouts;
    /// When set, load() fails with this message (an unreadable/corrupt backing file).
    std::optional<std::string> loadError;
    /// When set, save() fails with this message (permissions, disk full, ...).
    std::optional<std::string> saveError;
    /// The path each load()/save() was asked for, so a test can assert WHERE layouts persist.
    mutable std::vector<std::filesystem::path> loadedPaths;
    std::vector<std::filesystem::path> savedPaths;
};

/// A SpeechSynthesizer that records what it was asked to say instead of saying it.
///
/// The read-aloud path cannot otherwise be driven headlessly: the real synthesizer answers
/// available() only by connecting to the machine's speech service and enumerating its voices, so a
/// test would depend on the developer's installed voices and would speak out loud when they have one.
class RecordingSpeechSynthesizer final: public contour::platform::SpeechSynthesizer
{
  public:
    [[nodiscard]] bool available() const override { return isAvailable; }
    void say(std::string_view text) override { spoken.emplace_back(text); }
    void stop() override { ++stopCount; }

    /// Everything say() was handed, in order — already passed through speakableText().
    std::vector<std::string> spoken;
    /// How many times stop() was asked for.
    size_t stopCount = 0;
    /// What available() reports (flip to false for the "no voice on this machine" path).
    bool isAvailable = true;
};

// The former fixture-local BlockingMockPty (an in-memory PTY with real blocking-read semantics
// for tests running a LIVE session read loop) now ships as vtpty::ChannelPty, so production
// remote sessions run on the identical Pty.

/// PTY whose start() produces a caller-chosen outcome, so every way a device can come up — or fail
/// to — is drivable from one fixture rather than a family of near-identical ones.
///
/// The Throw case is the shape of the #1711 crash: on Windows a CreateProcess() failure is
/// discovered in the PARENT (there is no fork), and the only channel Process::start() had was an
/// exception. Thrown from TerminalSession::start(), it unwound out of TerminalDisplay::setSession()
/// mid-mutation — past emit sessionChanged() — leaving a half-bound display and a session that,
/// never having started its exit watcher, no onClosed() could ever prune. It is kept as a case
/// because no device may take the process down that way, whatever it throws.
///
/// Built on vtpty::ChannelPty rather than vtpty::MockPty because the Diagnose case must leave a
/// session that is genuinely UP: MockPty answers an empty buffer with a zero-length read, which
/// Terminal::processInputOnce() reads as EOF — so the read loop this start() has just launched would
/// close the device from under the test within microseconds of returning. That is a race a debug build
/// happens to win and a release build loses. ChannelPty blocks instead, which is why production
/// remote sessions run on it too.
class ConfigurableStartPty final: public vtpty::ChannelPty
{
  public:
    /// What start() does.
    enum class StartBehavior : uint8_t
    {
        Diagnose, ///< Starts, reporting what it had to give up (a spawn-ladder rung below the first).
        Fail,     ///< Reports a failure as a value, the way vtpty does.
        Throw,    ///< Throws, the way the Windows spawn used to. See issue #1711.
    };

    /// The message the Windows reporters in #1711 actually saw.
    static constexpr auto FailureText = std::string_view { "The directory name is invalid." };

    /// A non-fatal notice, as the spawn ladder's directory-dropping rung produces.
    static constexpr auto DiagnosticText =
        std::string_view { "Failed to start in \"/gone\". Using the current directory instead." };

    ConfigurableStartPty(vtbackend::PageSize pageSize, StartBehavior behavior):
        vtpty::ChannelPty { pageSize }, _behavior { behavior }
    {
    }

    vtpty::StartResult start() override
    {
        switch (_behavior)
        {
            case StartBehavior::Throw: throw std::runtime_error(std::string { FailureText });
            case StartBehavior::Fail:
                return std::unexpected(vtpty::StartFailure { .error = vtpty::StartError::SpawnFailed,
                                                             .detail = std::string { FailureText } });
            case StartBehavior::Diagnose:
                return vtpty::ChannelPty::start().transform([](vtpty::StartOutcome outcome) {
                    outcome.diagnostic = std::string { DiagnosticText };
                    return outcome;
                });
        }
        return {};
    }

  private:
    StartBehavior _behavior;
};

/// The process's own environment, but for the names a test sets, which read as the test set them.
class TestEnvironment final: public core::Environment
{
  public:
    /// The names a test sets, and their values.
    using Overrides = std::map<std::string, std::string, std::less<>>;

    /// @param overrides The names a test sets; every other name reads as the process's own environment has
    /// it.
    explicit TestEnvironment(Overrides overrides = {}): _overrides { std::move(overrides) } {}

    /// @param name Name of the variable to look up.
    /// @return What the test set it to; else what the process's own environment holds.
    [[nodiscard]] std::optional<std::string> get(std::string_view name) const override
    {
        if (auto const i = _overrides.find(name); i != _overrides.end())
            return i->second;
        return _live.get(name);
    }

  private:
    core::LiveEnvironment _live;
    Overrides _overrides;
};

/// In-memory window location store; never reads or writes the test machine's user state.
class InMemoryWindowPositionStore final: public contour::window::WindowPositionStore
{
  public:
    std::optional<contour::window::WindowPosition> value;
    mutable size_t loads = 0;
    size_t saves = 0;
    [[nodiscard]] std::expected<std::optional<contour::window::WindowPosition>, std::string> load(
        std::filesystem::path const&) const override
    {
        ++loads;
        return value;
    }
    [[nodiscard]] std::expected<void, std::string> save(
        std::filesystem::path const&, contour::window::WindowPosition const& position) override
    {
        value = position;
        ++saves;
        return {};
    }
};

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
    explicit TestApp(std::unique_ptr<contour::session::SessionFactory> factory = nullptr,
                     std::unique_ptr<contour::config::LayoutStore> layoutStore = nullptr,
                     std::unique_ptr<contour::command::CommandHistoryStore> commandHistoryStore = nullptr,
                     std::unique_ptr<contour::platform::SpeechSynthesizer> speech = nullptr,
                     vtbackend::HostIdentity identity = testHostIdentity(),
                     core::platform::FileSystem const* fileSystem = nullptr,
                     TestEnvironment::Overrides environment = {},
                     std::unique_ptr<contour::platform::TaskbarProgressSink> taskbarProgress = nullptr,
                     contour::platform::AppearanceServices const& appearance =
                         contour::platform::nullAppearanceServices()):
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
             settingsWindowFactory())
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
             settingsWindowFactory())
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
             settingsWindowFactory())
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
    /// The recording launcher wired into this app (records URL-open / process-spawn requests).
    [[nodiscard]] RecordingExternalLauncher& launcher() noexcept
    {
        REQUIRE(_launcher != nullptr); // not on an app built with the default launcher
        return *_launcher;
    }

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
            }
            return contour::window::loadSettingsWindow(*_settingsEngine, host);
        };
    }

    RecordingExternalLauncher* _launcher = nullptr;
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
/// test that reloads the configuration from disk needs.
class ConfiguredTestApp
{
  public:
    /// @param factory Optional PTY factory override, as TestApp takes it.
    /// @param appearance The OS accessibility and appearance sources, as TestApp takes them.
    explicit ConfiguredTestApp(std::unique_ptr<contour::session::SessionFactory> factory = nullptr,
                               contour::platform::AppearanceServices const& appearance =
                                   contour::platform::nullAppearanceServices()):
        _testApp {
            std::move(factory), nullptr, nullptr, nullptr, TestApp::testHostIdentity(), nullptr, {}, nullptr,
            appearance
        },
        _configFile { std::filesystem::path(_configDir.path().toStdU16String()) / "contour.yml" }
    {
        REQUIRE(_configDir.isValid());
        _testApp.app().config().configFile = _configFile;
    }

    [[nodiscard]] contour::ContourGuiApp& app() noexcept { return _testApp.app(); }
    [[nodiscard]] contour::session::TerminalSessionManager& manager() noexcept { return _testApp.manager(); }

    /// @return The file the app's configuration is read from.
    [[nodiscard]] std::filesystem::path const& configFile() const noexcept { return _configFile; }

  private:
    QTemporaryDir _configDir; ///< First, so it outlives the app that reads from it.
    TestApp _testApp;
    std::filesystem::path _configFile;
};

/// Owns one manager-minted WindowController for a test's lifetime. Production deletes controllers
/// through removeWindowController() (which deleteLater()s them); the guard replays that and drains
/// the deferred-delete queue so LeakSanitizer sees a clean teardown. Removal is idempotent, so a
/// test may also remove the controller explicitly.
struct ScopedController
{
    contour::session::TerminalSessionManager& manager;
    contour::window::WindowController* controller;
    vtworkspace::WindowId id;

    explicit ScopedController(contour::session::TerminalSessionManager& m):
        manager(m), controller(m.createWindowController()), id(controller->windowId())
    {
    }
    /// A controller whose menus open as @p menuPresentation says, whatever the app's style says.
    ScopedController(contour::session::TerminalSessionManager& m,
                     contour::config::MenuPresentation menuPresentation):
        manager(m), controller(m.createWindowController(menuPresentation)), id(controller->windowId())
    {
    }
    ~ScopedController()
    {
        manager.removeWindowController(id); // no-op if the test already removed it
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
    ScopedController(ScopedController const&) = delete;
    ScopedController& operator=(ScopedController const&) = delete;
    ScopedController(ScopedController&&) = delete;
    ScopedController& operator=(ScopedController&&) = delete;

    [[nodiscard]] contour::window::WindowController* operator->() const noexcept { return controller; }
    [[nodiscard]] contour::window::WindowController& operator*() const noexcept { return *controller; }
};

/// The OS window half of @ref ScopedControllerWithOsWindow, a base of its own so that it is built
/// before the controller and destroyed after it.
struct OsWindowHolder
{
    std::unique_ptr<QQuickWindow> osWindow = std::make_unique<QQuickWindow>(); ///< Never shown.
};

/// A @ref ScopedController with an OS window it may be bound to (`bindWindow(osWindow.get())`), for a
/// test that needs the window's own state -- minimised, active. The controller keeps a raw pointer to
/// the window once bound, so the window outlives it: OsWindowHolder is the first base.
struct ScopedControllerWithOsWindow: OsWindowHolder, ScopedController
{
    explicit ScopedControllerWithOsWindow(contour::session::TerminalSessionManager& m): ScopedController(m) {}
};

/// Closes every tab of @p window, last first, so the sessions wind down as a user closing them would.
inline void closeAllTabs(contour::window::WindowController& window)
{
    for (auto const row: std::views::iota(0, window.count()) | std::views::reverse)
        window.closeTabAtIndex(row);
}

/// Tabs that each have a real (mock-PTY) session: their ids and session ids in row order.
struct SessionTabs
{
    std::vector<vtworkspace::TabId> tabs {};
    std::vector<vtworkspace::SessionId> sessions {};
};

/// Opens @p n tabs in @p controller's window, each with a session of its own.
/// @param manager    The session manager owning @p controller.
/// @param controller The window to open the tabs in.
/// @param n          How many tabs to open.
/// @return The tabs and their sessions, in row order.
[[nodiscard]] inline SessionTabs createSessionTabs(contour::session::TerminalSessionManager& manager,
                                                   contour::window::WindowController& controller,
                                                   int n)
{
    auto result = SessionTabs {};
    for ([[maybe_unused]] auto const _: std::views::iota(0, n))
    {
        controller.createNewTab();
        auto const* tab = manager.model().window(controller.windowId())->activeTab();
        REQUIRE(tab != nullptr);
        REQUIRE(controller.activeSession() != nullptr);
        result.tabs.push_back(tab->id());
        result.sessions.push_back(controller.activeSession()->modelSessionId());
    }
    return result;
}

/// The node titled @p title in @p nodes, searching submenus too.
/// @param nodes The nodes to search.
/// @param title The title to find.
/// @return The node, or nullptr if there is none.
[[nodiscard]] inline contour::command::MenuNode const* nodeTitled(
    std::span<contour::command::MenuNode const> nodes, std::string_view title)
{
    for (auto const& node: nodes)
    {
        if (node.title == title)
            return &node;
        if (auto const* nested = nodeTitled(node.children, title))
            return nested;
    }
    return nullptr;
}

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

/// Whether an action ran to its end while another thread held a lock.
enum class UnderLock : uint8_t
{
    Waited = 0, ///< It could finish only once the lock was let go.
    Finished,   ///< It finished while the lock was held.
};

/// Runs @p action while another thread holds @p lockable -- as a parser holds its terminal across a whole
/// write -- and tells whether it finished without waiting for that thread.
///
/// The holder lets go as soon as @p action returns, or once @p bound has passed: a regression costs the
/// bound rather than hanging the suite, and a pass costs nothing.
/// @param lockable What the other thread holds.
/// @param action   What must not wait for it.
/// @param bound    The longest the other thread holds on while @p action has not returned.
/// @return Finished when @p action returned while the lock was still held.
template <typename Lockable, typename Action>
[[nodiscard]] UnderLock runWhileHeld(Lockable& lockable,
                                     Action&& action,
                                     std::chrono::milliseconds bound = std::chrono::milliseconds { 500 })
{
    auto mutex = std::mutex {};
    auto changed = std::condition_variable {};
    auto held = false;
    auto done = false;
    auto released = std::atomic<bool> { false };

    auto holder = std::thread { [&]() {
        auto const hold = std::scoped_lock { lockable };
        {
            auto lock = std::unique_lock { mutex };
            held = true;
            changed.notify_all();
            changed.wait_for(lock, bound, [&]() { return done; });
        }
        released.store(true, std::memory_order_release);
    } };

    {
        auto lock = std::unique_lock { mutex };
        changed.wait(lock, [&]() { return held; });
    }
    std::forward<Action>(action)();
    auto const outcome = released.load(std::memory_order_acquire) ? UnderLock::Waited : UnderLock::Finished;

    {
        auto const lock = std::scoped_lock { mutex };
        done = true;
    }
    changed.notify_all();
    holder.join();
    return outcome;
}

} // namespace contour::test
