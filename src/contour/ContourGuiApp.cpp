// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/Logging.hpp>
#include <contour/cli/Hosting.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/display/ContentScale.hpp>
#include <contour/display/FirstFrameWatchdog.hpp>
#include <contour/display/GpuInventory.hpp>
#include <contour/display/LatencyProbeDriver.hpp>
#ifdef CONTOUR_WITH_GPU_SELECTION
    #include <contour/display/GraphicsDeviceSelector.hpp>
#endif
#include <contour/display/Logging.hpp>
#include <contour/display/OpenGlGpuEnvironment.hpp>
#include <contour/display/RenderingBackendSelection.hpp>
#include <contour/display/ShaderConfig.hpp> // createSurfaceFormat
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/platform/MultimediaEnvironment.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/remote/RoutingSessionFactory.hpp>
#include <contour/remote/TmuxController.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/PopupShadowImageProvider.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/WindowController.hpp>
#include <contour/window/WindowLiveness.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <vtpty/Process.hpp>
#include <vtpty/ProgramLookup.hpp>
#include <vtpty/SandboxInfo.hpp>

#ifdef CONTOUR_EXTENSIONS
    #include <vtextension/BundledExtensions.hpp>
    #include <vtextension/ExtensionConsent.hpp>
    #include <vtextension/SessionExtensions.hpp>
    #include <vtextension/wasm/DefaultBackend.hpp>
    #include <vtextension/wasm/WasmRuntime.hpp>
    #include <vthost/client/DaemonGrantReplies.hpp>
#endif

#include <text_shaper/BundledFonts.hpp>
#include <text_shaper/FontLocator.hpp>

#include <crispy/ScopedTimer.hpp>

#include <core/Utf8Path.hpp>
#include <core/Utils.hpp>
#include <core/cli/CLI.hpp>
#include <core/log/LogSink.hpp>
#include <core/log/LogStore.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/NativeFileSystem.hpp>
#include <core/platform/PathUtils.hpp>

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFileInfo>
#include <QtCore/QProcess>
#include <QtCore/QTimer>
#include <QtNetwork/QHostInfo>
#include <QtQml/qqmlextensionplugin.h>

#include <vthost/CommandRunner.hpp>
#include <vthost/Daemon.hpp>
#include <vthost/DaemonBinary.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Token.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/client/DaemonRestart.hpp>
#include <vthost/client/ReleaseDrift.hpp>
#include <vthost/latency/Report.hpp>
#if !defined(__APPLE__) && !defined(_WIN32)
    #include <QtDBus/QDBusConnection>
#endif
#include <QtCore/QtPlugin>
#include <QtDBus/QtDBus>
#include <QtGui/QGuiApplication>
#include <QtGui/QStyleHints>
#include <QtGui/QSurfaceFormat>
#include <QtGui/rhi/qrhi.h>
#include <QtMultimedia/QMediaDevices>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickGraphicsDevice>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtWidgets/QApplication>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <thread>
#include <vector>

#ifndef _WIN32
    #include <csignal>

    #include <pthread.h>
#endif

#include <QtQuickControls2/QQuickStyle>

using std::bind;
using std::cerr;
using std::make_unique;
using std::string;
using std::string_view;
using std::vector;

using vtpty::Process;
using vtpty::SshSession;

using namespace std::string_literals;

namespace fs = std::filesystem;

namespace CLI = core::cli;

namespace contour
{

fs::path uiModuleOverrideDirectory(fs::path const& configHome)
{
    return configHome / "Contour" / "Ui";
}

bool hasStrandedQmlOverrides(fs::path const& configHome)
{
    auto ec = std::error_code {};
    auto const legacyDirectory = configHome / "ui";
    if (!fs::is_directory(legacyDirectory, ec))
        return false;

    // Already migrated: the loose files are then leftovers, not a broken customization.
    if (fs::exists(uiModuleOverrideDirectory(configHome) / "qmldir", ec))
        return false;

    // A failed directory_iterator construction yields end(), so the loop simply does not run; `ec` is
    // left to the same fate as everywhere else this pattern appears (Config.cpp, SettingsController)
    // -- an unreadable config directory is not news the QML loader can act on.
    for (auto const& entry: fs::directory_iterator(legacyDirectory, ec))
    {
        auto entryError = std::error_code {};
        if (entry.is_regular_file(entryError) && entry.path().extension() == ".qml")
            return true;
    }
    return false;
}

namespace
{
    /// Worker threads of the pool every session's status-line commands and link matching run on.
    constexpr auto StatusLineCommandThreads = std::size_t { 2 };

    /// Worker threads every session's path probes run on: one, so that a probe blocked on a hung mount stalls
    /// only the other probes.
    constexpr auto LinkProbeThreads = std::size_t { 1 };

#ifndef _WIN32
    /// Blocks every signal in the constructing thread and restores the previous mask on destruction.
    ///
    /// A thread inherits the signal mask of the thread that creates it, so threads started while this
    /// guard is alive never receive an asynchronous signal.
    class ScopedAllSignalsBlocked
    {
      public:
        ScopedAllSignalsBlocked() noexcept
        {
            auto all = sigset_t {};
            sigfillset(&all);
            // Synchronous fault signals stay deliverable: they are raised by the faulting thread itself,
            // and blocking them turns a crash into a silent kill with no sanitizer or crash report.
            for (auto const fault: { SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGTRAP })
                sigdelset(&all, fault);
            pthread_sigmask(SIG_BLOCK, &all, &_previous);
        }
        ScopedAllSignalsBlocked(ScopedAllSignalsBlocked const&) = delete;
        ScopedAllSignalsBlocked& operator=(ScopedAllSignalsBlocked const&) = delete;
        ScopedAllSignalsBlocked(ScopedAllSignalsBlocked&&) = delete;
        ScopedAllSignalsBlocked& operator=(ScopedAllSignalsBlocked&&) = delete;
        ~ScopedAllSignalsBlocked() { pthread_sigmask(SIG_SETMASK, &_previous, nullptr); }

      private:
        sigset_t _previous {};
    };
#endif

    /// Creates a pool for the app's background work: status-line commands, link matching, path probes.
    ///
    /// On POSIX the workers are created with every signal blocked. Otherwise they would keep SIGINT and
    /// SIGTERM unblocked, and the kernel may deliver a process-directed signal to any thread that does
    /// not block it: `contour daemon` blocks them in its own thread and consumes them on a dedicated
    /// sigwait thread (vthost::runDaemon), so a delivery to a pool worker would kill the process with
    /// the default disposition before the daemon could clean up its socket. The spawned shells do not
    /// inherit the blocked mask (SystemCommandCapture resets it).
    /// @param threads The number of workers.
    /// @return The pool.
    [[nodiscard]] core::async::ThreadPoolExecutor makeBackgroundExecutor(std::size_t threads)
    {
#ifndef _WIN32
        auto const signalsBlocked = ScopedAllSignalsBlocked {};
#endif
        return core::async::ThreadPoolExecutor { threads };
    }

    /// The production ProgramReachablePredicate: the reachability test every external launch in the
    /// app already asks, phrased for the CLI's UTF-8 token.
    [[nodiscard]] bool isReachableProgram(std::string_view program)
    {
        return platform::isReachableProgram(
            QString::fromUtf8(program.data(), static_cast<qsizetype>(program.size())));
    }

    /// Whether an existing regular file at @p path is one this machine would execute.
    ///
    /// Asked of Qt rather than of `fs::perms::owner_exec`, because the POSIX exec bit is not the
    /// question everywhere: Windows has no such bit, so std::filesystem reports every readable file
    /// as owner-executable there and `contour terminal notes.txt` was run as a program instead of
    /// opening a shell beside it. Qt tests the exec bit on POSIX and the suffix against %PATHEXT% on
    /// Windows -- the same answer platform::isReachableProgram() gives for a token naming a path.
    ///
    /// @param path An existing regular file.
    /// @return true when the file names a program to run.
    [[nodiscard]] bool namesAnExecutableFile(fs::path const& path)
    {
        return QFileInfo(platform::toQString(path)).isExecutable();
    }

    /// The production CommandOutputFiles: the native filesystem, in a directory named after this process.
    /// @param env The process environment, which names the runtime directory.
    /// @param liveness What tells whose leftover directories may go. Borrowed: it must outlive the files.
    [[nodiscard]] std::unique_ptr<session::CommandOutputFiles> makeCommandOutputFiles(
        core::Environment const& env, session::ProcessLiveness const& liveness)
    {
        auto ec = std::error_code {};
        auto temporary = fs::temp_directory_path(ec);
        if (ec)
            temporary.clear();
        return std::make_unique<session::CommandOutputFiles>(
            core::platform::NativeFileSystem::instance(),
            liveness,
            session::commandOutputDirectory(
                env, vtpty::currentSandbox(), temporary, QCoreApplication::applicationPid()));
    }

    /// This machine as every session judges locality by it: its machine id and its host name.
    [[nodiscard]] vtbackend::HostIdentity thisMachineIdentity()
    {
        return vtbackend::readHostIdentity(core::platform::NativeFileSystem::instance(),
                                           QHostInfo::localHostName().toStdString());
    }

    /// Ends the app when no window is left to show anything: quit-on-last-window-closed was suspended
    /// for a daemon restart, and a failed restart must not leave a windowless process behind.
    void quitIfWindowless()
    {
        auto facts = std::vector<window::WindowFacts> {};
        for (auto const* topLevel: QGuiApplication::topLevelWindows())
            facts.push_back({ .visibility = topLevel->isVisible() ? window::WindowVisibility::Shown
                                                                  : window::WindowVisibility::Hidden,
                              .transience = topLevel->transientParent() != nullptr
                                                ? window::WindowTransience::Transient
                                                : window::WindowTransience::Independent });
        if (!window::anyWindowKeepsAppAlive(facts))
            QCoreApplication::quit();
    }

    /// What starting this client's local daemon needs: this release's binary, the spawner, and the
    /// command runner the service backends reach the OS through. Shared by the first connect and by the
    /// drift badge's restart, so the two start a daemon the same way.
    struct LocalDaemonStart
    {
        std::unique_ptr<vthost::DaemonBinarySource> binary;
        std::unique_ptr<vthost::DaemonSpawner> spawner;
        std::unique_ptr<vthost::CommandRunner> commands;
        vthost::ServiceBackendServices services;

        /// @param env The process environment.
        /// @param fileSystem The file system the binary source reads.
        /// @param programPath The running executable as it was started.
        LocalDaemonStart(core::Environment const& env,
                         core::platform::FileSystem const& fileSystem,
                         std::string const& programPath):
            binary { vthost::makeOwnDaemonBinary(env, fileSystem, CONTOUR_VERSION_STRING, programPath) },
            spawner { vthost::makeNativeDaemonSpawner() },
            commands { vthost::makeNativeCommandRunner() },
            services { .fileSystem = core::platform::NativeFileSystem::instance(),
                       .commands = *commands,
                       .environment = env }
        {
        }

        /// @param label The socket label.
        /// @param endpoint Where the daemon listens.
        /// @param placement Places a daemon spawned on demand.
        /// @param options What a spawned daemon is told.
        /// @return A registered service when there is one, spawning on demand otherwise. Valid while
        ///         this object lives.
        [[nodiscard]] std::unique_ptr<vthost::DaemonStarter> starter(
            std::string_view label,
            vthost::UnixEndpoint const& endpoint,
            vtpty::ProcessPlacement& placement,
            vthost::DaemonSpawnOptions const& options) const
        {
            return vthost::chooseDaemonStarter(
                vthost::findRegisteredService(label, vthost::nativeServiceBackendFactory(services)),
                vthost::OnDemandSpawn { .endpoint = endpoint,
                                        .spawner = *spawner,
                                        .binary = *binary,
                                        .placement = placement,
                                        .options = options,
                                        .timeout = std::chrono::seconds(5) });
        }
    };

    /// The `--latency-probe` options, which `terminal` and `client` share.
    [[nodiscard]] CLI::Option latencyProbeFileOption()
    {
        return CLI::Option { "latency-probe",
                             CLI::Value { ""s },
                             "Types keys at the first window and writes when their echo was drawn to FILE "
                             "(see docs/internals/latency.md). For `bench-latency`.",
                             "FILE" };
    }

    [[nodiscard]] CLI::Option latencyProbeVsyncOption()
    {
        // A CLI boundary flag, not an API bool.
        return CLI::Option { "latency-probe-vsync",
                             CLI::Value { false },
                             "With --latency-probe: swap on the vertical blank like a normal run, instead of "
                             "unpaced." };
    }

    [[nodiscard]] CLI::Option latencyProbeKeysOption()
    {
        return CLI::Option { "latency-probe-keys",
                             CLI::Value { static_cast<int>(vthost::latency::ProbePlan {}.measuredKeys) },
                             "With --latency-probe: how many keys are measured.",
                             "N" };
    }
} // namespace

VerbatimCommand classifyVerbatimCommand(std::string_view token, ProgramReachablePredicate isReachable)
{
    auto const path = core::pathFromUtf8(token).value_or(fs::path {});
    auto ec = std::error_code {};
    auto const status = fs::status(path, ec);

    if (fs::is_regular_file(status))
    {
        // An executable file names itself; a plain file names the directory it lives in, which is
        // what `contour terminal ~/notes.txt` (a file manager's "open terminal here") asks for.
        if (namesAnExecutableFile(path))
            return VerbatimCommand { .kind = VerbatimCommandKind::Program, .workingDirectory = {} };
        return VerbatimCommand { .kind = VerbatimCommandKind::WorkingDirectory,
                                 .workingDirectory = path.parent_path() };
    }

    if (fs::is_directory(status))
        return VerbatimCommand { .kind = VerbatimCommandKind::WorkingDirectory, .workingDirectory = path };

    // Nothing on disk under that name -- but `contour terminal echo hello` names its program the way
    // anyone names one, and the spawn resolves it through execvp()'s PATH search. Ask the same
    // question here, or the documented way to run a command is reported as a mistake (#2102).
    if (isReachable(token))
        return VerbatimCommand { .kind = VerbatimCommandKind::Program, .workingDirectory = {} };

    return VerbatimCommand { .kind = VerbatimCommandKind::Unknown, .workingDirectory = {} };
}

ContourGuiApp::ContourGuiApp(core::Environment const& env,
                             std::unique_ptr<session::SessionFactory> sessionFactory,
                             std::unique_ptr<platform::ExternalLauncher> externalLauncher,
                             std::unique_ptr<config::LayoutStore> layoutStore,
                             std::unique_ptr<command::CommandHistoryStore> commandHistoryStore,
                             std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer,
                             std::unique_ptr<session::CommandOutputFiles> commandOutputFiles,
                             vtbackend::TerminalClocks terminalClocks,
                             std::optional<vtbackend::HostIdentity> hostIdentity,
                             core::platform::FileSystem const* fileSystem,
                             std::shared_ptr<vtpty::ProcessPlacement> processPlacement,
                             std::optional<vtbackend::LinkDetectionServices> linkServices,
                             std::unique_ptr<platform::TaskbarProgressSink> taskbarProgress,
                             std::unique_ptr<window::WindowPositionStore> windowPositionStore,
                             platform::AppearanceServices const& appearanceServices,
                             core::platform::SteadyTimePoint processStart):
    ContourApp { env },
    _startupTimeline { core::platform::defaultSteadyClock(), processStart },
    _sshSearchPath { vtpty::execSearchPath(env.get("PATH")) },
    _commandCapture { vtpty::SystemCommandCapture::defaultShell(env), _commandShutdown.get_token() },
    _commandExecutor(makeBackgroundExecutor(StatusLineCommandThreads)),
    _processPlacement(processPlacement ? std::move(processPlacement) : vtpty::makeDefaultProcessPlacement()),
    _linkProbeExecutor(makeBackgroundExecutor(LinkProbeThreads)),
    _injectedLinkServices { std::move(linkServices) },
    _sessionFactory(std::make_unique<remote::RoutingSessionFactory>(
        sessionFactory ? std::move(sessionFactory)
                       : std::make_unique<session::AppSessionFactory>(*this, _processPlacement))),
    _routingFactory(static_cast<remote::RoutingSessionFactory*>(_sessionFactory.get())),
    _externalLauncher(externalLauncher ? std::move(externalLauncher)
                                       : platform::makeExternalLauncher(_processPlacement)),
    _layoutStore(layoutStore ? std::move(layoutStore) : std::make_unique<config::FileLayoutStore>()),
    _commandHistoryStore(commandHistoryStore ? std::move(commandHistoryStore)
                                             : std::make_unique<command::FileCommandHistoryStore>()),
    _windowPositionStore(windowPositionStore ? std::move(windowPositionStore)
                                             : std::make_unique<window::FileWindowPositionStore>()),
    _speechSynthesizer(speechSynthesizer ? std::move(speechSynthesizer) : platform::makeSpeechSynthesizer()),
    _commandOutputFiles(commandOutputFiles ? std::move(commandOutputFiles)
                                           : makeCommandOutputFiles(env, _processLiveness)),
    _terminalClocks(terminalClocks),
    // Read once, for every session, rather than by each one on the GUI thread as it is built.
    _hostIdentity(hostIdentity ? std::move(*hostIdentity) : thisMachineIdentity()),
    _fileSystem(fileSystem ? *fileSystem : core::platform::NativeFileSystem::instance()),
    _taskbarProgressSink(taskbarProgress ? std::move(taskbarProgress) : platform::makeTaskbarProgressSink()),
    // The one read of the home directory: QDir's form (`C:/Users/...` on Windows) is the one the tab
    // tooltip's paths arrive in, which the environment's native separators would not match.
    _sessionManager(*this,
                    *_sessionFactory,
                    *_layoutStore,
                    *_commandHistoryStore,
                    QDir::homePath().toStdString(),
                    *_taskbarProgressSink),
    _gpuInventory { display::makePlatformGpuInventory() },
    _accessibilitySource(
        appearanceServices.accessibility([this](config::AccessibilityPreferences preferences) {
            onAccessibilityPreferencesChanged(preferences);
        })),
    _platformAppearance(appearanceServices.appearance(
        [this](platform::PlatformAppearance appearance) { onPlatformAppearanceChanged(appearance); }))
{
    // One filesystem for the paths a session shows, opens and detects (Terminal's contract): injected link
    // services carry the app's.
    Require(!_injectedLinkServices || &_injectedLinkServices->fileSystem == &_fileSystem);
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
    link("contour.font-locator", bind(&ContourGuiApp::fontConfigAction, this));
    link("contour.info.config", bind(&ContourGuiApp::checkConfig, this));
    link("contour.client", bind(&ContourGuiApp::clientAction, this));
}

vtbackend::LinkDetectionServices ContourGuiApp::linkDetectionServices() noexcept
{
    if (_injectedLinkServices)
        return *_injectedLinkServices;
    return { .matchExecutor = _commandExecutor,
             .probeExecutor = _linkProbeExecutor,
             .fileSystem = fileSystem(),
             .clock = _terminalClocks.steady.get(),
             .tuning = {} };
}

ContourGuiApp::~ContourGuiApp()
{
    // Before any member is destroyed: cancels the status-line commands still running or queued, so the
    // command pool joins without waiting out their timeouts.
    _commandShutdown.request_stop();
}

#ifdef CONTOUR_EXTENSIONS
vtextension::ExtensionRuntime& ContourGuiApp::extensionRuntime()
{
    if (!_extensionRuntime)
    {
        if (!_extensionBackend)
            _extensionBackend = { vtextension::wasm::makeDefaultBackend().release(),
                                  [](vtextension::wasm::IWasmBackend* made) {
                                      std::default_delete<vtextension::wasm::IWasmBackend> {}(made);
                                  } };
        auto const executableDirectory =
            std::filesystem::path { QCoreApplication::applicationDirPath().toStdU16String() };
        auto runtime = std::make_unique<vtextension::ExtensionRuntime>(
            vtextension::ExtensionRuntimeConfig {
                .grantsFile = _config.configFile.parent_path() / vtextension::ExtensionGrantsFileName,
                .bundledDirectories = vtextension::bundledExtensionDirectories(
                    vtextension::nativeBundledExtensionPlatform(), processEnvironment(), executableDirectory),
                .workerCount = vtextension::extensionWorkerCount(std::thread::hardware_concurrency()),
                .limits = {} },
            vtextension::ExtensionRuntimeServices { .backend = *_extensionBackend,
                                                    .capture = _commandCapture,
                                                    .wallClock = _terminalClocks.wall.get(),
                                                    .localTime = _terminalClocks.localTime,
                                                    .environment = processEnvironment(),
                                                    .fileSystem = _fileSystem });
        _extensionRuntime = { runtime.release(), [](vtextension::ExtensionRuntime* made) {
                                 std::default_delete<vtextension::ExtensionRuntime> {}(made);
                             } };
    }
    return *_extensionRuntime;
}

void ContourGuiApp::releaseRetiredExtensionsSoon()
{
    // A grace long enough for a lane to run a runner's last job, which a command in flight on the same lane
    // can delay; whatever retires later is released by the next host, or with the runtime.
    constexpr auto RetiredExtensionGrace = std::chrono::seconds { 1 };
    QTimer::singleShot(RetiredExtensionGrace, this, [this]() {
        if (_extensionRuntime)
            _extensionRuntime->releaseRetired();
    });
}

void ContourGuiApp::askHostedExtensions()
{
    if (!_nativeController)
        return;
    if (!_daemonGrants)
    {
        auto replies = std::make_unique<vthost::client::DaemonGrantReplies>(
            [this](std::string_view hostId, std::string_view digest, vthost::GrantState answer) {
                if (_nativeController)
                    _nativeController->answerPermission(
                        std::string { hostId }, std::string { digest }, answer);
            });
        _daemonGrants = { replies.release(), [](vtextension::IGrantStore* made) {
                             std::default_delete<vtextension::IGrantStore> {}(made);
                         } };
    }
    for (auto& [pty, request]: _nativeController->takePermissionRequests())
    {
        auto* session = _sessionManager.sessionForPty(pty);
        if (!session)
            continue;
        if (auto consent = vthost::client::consentRequestOf(request))
            session->askForHostedExtension(std::move(*consent), *_daemonGrants);
        else
            // A newer daemon's permission this client cannot phrase: asking without it would have the
            // user grant what they were not shown. Asked again on the next attach.
            errorLog()("Extension {} asks for a control permission this version of Contour cannot show; "
                       "not asking.",
                       request.hostId);
    }
}
#endif

void ContourGuiApp::applyGraphicsDevice(QQuickWindow& window)
{
    logGpuInUse(window);
#ifdef CONTOUR_WITH_GPU_SELECTION
    if (_graphicsDeviceSelector)
    {
        _graphicsDeviceSelector->applyTo(window);
        if (auto fallback = _graphicsDeviceSelector->takeProbeFallback())
            _pendingGpuFallback = std::move(fallback);
        reportGpuFallbackOnceUp(window);
        if (_graphicsDeviceSelector->selector().preference != config::GpuPreference::Auto)
            watchFirstFrame(window, [this](QQuickWindow& failed) { onGpuFailure(failed); });
        return;
    }
#endif
    reportGpuFallbackOnceUp(window);
    if (_openGlGpuTitle)
        watchFirstFrame(window, [this](QQuickWindow& /*failed*/) { relaunchOnAutomaticGpu(); });
}

void ContourGuiApp::watchFirstFrame(QQuickWindow& window, std::function<void(QQuickWindow&)> onFailure)
{
    // Only the first window is watched: later ones render on the same GPU (Vulkan and Direct3D reuse the
    // first window's choice, OpenGL's is process-wide), so its first frame answers for all of them.
    if (_firstFrameWatchdog)
        return;
    using namespace std::chrono_literals;
    _firstFrameWatchdog = std::make_unique<display::FirstFrameWatchdog>(
        window, 10s, [onFailure = std::move(onFailure), guarded = QPointer<QQuickWindow>(&window)] {
            if (guarded) // the window may have been closed before it ever drew
                onFailure(*guarded);
        });
}

std::optional<ContourGuiApp::LatencyProbeRequest> ContourGuiApp::readLatencyProbeRequest(
    std::string const& verb) const
{
    auto const file = parameters().get<string>(verb + ".latency-probe");
    if (file.empty())
        return std::nullopt;
    auto const keys = std::max(1, parameters().get<int>(verb + ".latency-probe-keys"));
    return LatencyProbeRequest {
        .reportFile = core::pathFromUtf8(file).value_or(fs::path {}),
        .keys = static_cast<std::size_t>(keys),
        .pacing = parameters().get<bool>(verb + ".latency-probe-vsync") ? display::SwapPacing::VSync
                                                                        : display::SwapPacing::Unpaced,
    };
}

void ContourGuiApp::armLatencyProbe()
{
    using namespace std::chrono_literals;
    if (_latencyProbeDriver || !_latencyProbeRequest)
        return;
    // Built on the first attempt, so its prompt timeout runs from here: a first window that never shows a
    // terminal display ends the run with PromptTimedOut instead of retrying until closed by hand.
    if (!_keystrokeProbe)
    {
        auto plan = vthost::latency::ProbePlan {};
        plan.measuredKeys = _latencyProbeRequest->keys;
        _keystrokeProbe =
            std::make_unique<vthost::latency::KeystrokeProbe>(core::platform::defaultSteadyClock(), plan);
    }
    // The first window shows its terminal display a little after the window itself exists.
    auto const focused = _sessionManager.focusedWindow();
    auto* const controller = focused ? _sessionManager.controllerFor(*focused) : nullptr;
    auto* const terminalDisplay = controller ? controller->activeDisplay() : nullptr;
    auto* const window = terminalDisplay ? terminalDisplay->window() : nullptr;
    if (!window)
    {
        // Nothing is due while the prompt is awaited; asking settles the prompt timeout.
        std::ignore = _keystrokeProbe->takeDueKey();
        if (_keystrokeProbe->phase() == vthost::latency::ProbePhase::PromptTimedOut)
            finishLatencyProbe();
        else
            QTimer::singleShot(5ms, this, [this] { armLatencyProbe(); });
        return;
    }

    _latencyProbeDriver = std::make_unique<display::LatencyProbeDriver>(
        *window, *terminalDisplay, *_keystrokeProbe, [this] { finishLatencyProbe(); });
    // After the driver's own handler, so the probe has seen the frame when this asks it.
    connect(
        window,
        &QQuickWindow::frameSwapped,
        this,
        [this] {
            if (_keystrokeProbe->phase() != vthost::latency::ProbePhase::AwaitingPrompt)
                _startupTimeline.mark(vthost::latency::StartupMark::FirstPromptSwapped);
        },
        Qt::DirectConnection);
    // The prompt may have been drawn before the driver listened, and an idle terminal draws no more frames.
    window->update();
}

void ContourGuiApp::finishLatencyProbe()
{
    auto const result = _keystrokeProbe->result();
    auto const report = vthost::latency::formatProbeReport(result, _startupTimeline);
    auto out = std::ofstream(_latencyProbeRequest->reportFile, std::ios::binary | std::ios::trunc);
    out << report;
    out.close();
    auto exitCode = result.phase == vthost::latency::ProbePhase::Finished ? EXIT_SUCCESS : EXIT_FAILURE;
    if (!out)
    {
        cerr << std::format("contour: cannot write the latency probe report to {}\n",
                            core::pathToUtf8(_latencyProbeRequest->reportFile));
        exitCode = EXIT_FAILURE;
    }
    _latencyProbeExitCode = exitCode;
    QCoreApplication::exit(exitCode);
}

void ContourGuiApp::logGpuInUse(QQuickWindow& window)
{
    // The first window names the GPU its first frame was really drawn on, for every backend -- `auto`
    // chooses nothing itself, so this is the only place it shows. The first frame, not
    // sceneGraphInitialized: Qt emits that once per window even when it re-creates a lost device over and
    // over, so it would name a GPU that never drew. Runs on the render thread, which owns the QRhi.
    if (_gpuInUseLog)
        return;
    _gpuInUseLog = connect(
        &window,
        &QQuickWindow::frameSwapped,
        &window,
        [this, &window, requested = _config.renderer.value().gpu] {
            _startupTimeline.mark(vthost::latency::StartupMark::FirstFrameSwapped);
            startupLog()("startup timeline:\n{}", _startupTimeline.describe());
            auto const* const rhi = window.rhi();
            if (!rhi)
                return;
            auto const info = rhi->driverInfo();
            // Vulkan and Direct3D report the PCI ids; OpenGL reports none, and a software device's
            // Khronos vendor id (0x10005 for Mesa) is no PCI id either.
            auto const isPciId = info.vendorId <= 0xFFFF && info.deviceId <= 0xFFFF;
            auto const id = isPciId ? config::PciId { .vendor = static_cast<std::uint16_t>(info.vendorId),
                                                      .device = static_cast<std::uint16_t>(info.deviceId) }
                                    : config::PciId {};
            startupLog()(
                "{}",
                display::gpuInUseLine(info.deviceName.toStdString(), id, rhi->backendName(), requested));
        },
        static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::SingleShotConnection));
}

#ifdef CONTOUR_WITH_GPU_SELECTION
void ContourGuiApp::onGpuFailure(QQuickWindow& window)
{
    auto const failed = _graphicsDeviceSelector->choose();
    auto const& fallback = _graphicsDeviceSelector->fallBackToAuto();
    if (!failed || !fallback || !fallback->adapter || fallback->candidate.id == failed->candidate.id)
    {
        errorLog()("The configured GPU could not render, and there is no other GPU to fall back to.");
        return;
    }
    window.setGraphicsDevice(QQuickGraphicsDevice::fromRhiAdapter(fallback->adapter));
    window.releaseResources();
    window.update();
    reportGpuFallback(failed->candidate.title, fallback->candidate.title);
}
#endif

void ContourGuiApp::relaunchOnAutomaticGpu()
{
    // OpenGL's GPU is chosen by the driver stack when the display connection opens, once per process, so
    // the only way onto another GPU is a new process: this one, restarted once without the variables it
    // set. The restarted one runs `auto` and has no watchdog, so it cannot restart again.
    auto const failed = _openGlGpuTitle.value_or("The configured GPU");
    // The marker is set together with every variable that selects a GPU; the list it sits in also holds
    // ones that select none (keepMultimediaOffTheGpus()).
    if (std::ranges::find(_selfOnlyEnvironment, display::SelfSetEnvironmentMarker)
        == _selfOnlyEnvironment.end())
    {
        errorLog()("{} could not render. Contour set nothing to select it, so there is nothing to undo.",
                   failed);
        return;
    }

    auto const inheritedEntries = QProcessEnvironment::systemEnvironment().toStringList();
    auto inheritedStore = std::vector<std::string> {};
    for (auto const& entry: inheritedEntries)
        inheritedStore.push_back(entry.toStdString());
    auto const inherited = std::vector<std::string_view>(inheritedStore.begin(), inheritedStore.end());
    auto environment = QStringList {};
    for (auto const& entry: display::gpuFallbackRelaunchEnvironment(inherited, _selfOnlyEnvironment, failed))
        environment.push_back(QString::fromStdString(entry));

    auto arguments = QStringList {};
    for (auto const index: std::views::iota(1, _argc))
        arguments.push_back(QString::fromUtf8(_argv[index]));

    auto process = QProcess {};
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments(arguments);
    process.setEnvironment(environment);
    process.setWorkingDirectory(QDir::currentPath());
    if (!process.startDetached())
    {
        errorLog()("{} could not render, and restarting Contour on the automatic GPU failed: {}",
                   failed,
                   process.errorString().toStdString());
        return;
    }
    errorLog()("{} could not render; restarting Contour on the automatic GPU.", failed);
    QCoreApplication::exit(EXIT_SUCCESS);
}

void ContourGuiApp::takeGpuFallbackRequest()
{
    auto const nameStore = std::string(display::GpuFallbackEnvironmentName);
    auto const* const name = nameStore.c_str();
    if (!qEnvironmentVariableIsSet(name))
        return;
    _gpuFallbackFrom = qEnvironmentVariable(name).toStdString();
    // Contour no longer intervenes, so the GPU now rendering is the system's default one.
    _pendingGpuFallback = display::GpuFallback { .failed = *_gpuFallbackFrom,
                                                 .used = display::displayGpuTitle(_gpuInventory->list()) };
    qunsetenv(name); // neither the shells nor a later restart may see it
}

config::GpuSelector ContourGuiApp::effectiveGpuSelector() const
{
    return _gpuFallbackFrom ? config::GpuSelector {} : _config.renderer.value().gpu;
}

void ContourGuiApp::reportGpuFallbackOnceUp(QQuickWindow& window)
{
    if (!_pendingGpuFallback)
        return;
    connect(
        &window,
        &QQuickWindow::frameSwapped,
        this,
        [this] {
            if (auto const fallback = std::exchange(_pendingGpuFallback, std::nullopt))
                reportGpuFallback(fallback->failed, fallback->used);
        },
        Qt::SingleShotConnection);
}

void ContourGuiApp::reportGpuFallback(std::string_view failed, std::string_view used)
{
    auto const message = std::format("{} could not render; using {} for this session.", failed, used);
    errorLog()("{}", message);
    if (!_gpuNotifier)
        _gpuNotifier = platform::makeDesktopNotifier(std::chrono::seconds(10));
    auto notification = vtbackend::DesktopNotification {};
    notification.title = "Contour";
    notification.body = message;
    _gpuNotifier->notify(notification);
}

void ContourGuiApp::keepMultimediaOffTheGpus()
{
    auto const isSet = [](std::string_view name) {
        return qEnvironmentVariableIsSet(std::string(name).c_str());
    };
    for (auto const name: platform::multimediaProbeVariablesToEmpty(isSet))
    {
        auto nameStore = std::string(name);
        qputenv(nameStore.c_str(), "");
        _selfOnlyEnvironment.push_back(std::move(nameStore));
    }
}

void ContourGuiApp::applyOpenGlGpuSelection()
{
#ifdef __linux__
    // A Contour started by another Contour inherits the GPU variables its parent set. Drop them first, so
    // this process decides afresh from the user's real environment (and never mistakes them for the user's).
    for (auto const& name: display::selfSetNamesFrom(
             qEnvironmentVariable(std::string(display::SelfSetEnvironmentMarker).c_str()).toStdString()))
        qunsetenv(name.c_str());
    qunsetenv(std::string(display::SelfSetEnvironmentMarker).c_str());

    auto const& renderer = _config.renderer.value();
    auto const selector = effectiveGpuSelector();
    if (selector.preference == config::GpuPreference::Auto)
        return; // `auto` does not intervene: the driver stack's default GPU, as without renderer.gpu
    if (renderer.renderingBackend != config::RenderingBackend::OpenGL
        && renderer.renderingBackend != config::RenderingBackend::Auto)
        return; // Vulkan/D3D choose through Qt's adapter API instead; Software has no GPU
    auto const gpus = _gpuInventory->list();
    auto const choice = display::chooseGpu(gpus, selector);
    if (!choice)
        return;
    auto const& chosen = gpus[choice->index];
    if (choice->outcome == display::RequestOutcome::FellBack)
        errorLog()("renderer.gpu: no GPU {} is present; using {}.", selector, chosen.title);
    _openGlGpuTitle = chosen.title;
    // Nothing is set when the chosen GPU already drives the display, or when the user exported the
    // variables themselves; then the driver decides, and only the scene graph's report names the GPU.
    auto const assignments = display::openGlSelectionEnvironment(
        chosen, [](std::string_view name) { return qEnvironmentVariableIsSet(std::string(name).c_str()); });
    for (auto const& assignment: assignments)
    {
        qputenv(assignment.name.c_str(), QByteArray::fromStdString(assignment.value));
        _selfOnlyEnvironment.push_back(assignment.name);
    }
    if (!assignments.empty())
    {
        auto const names = display::selfSetMarkerValue(assignments);
        qputenv(std::string(display::SelfSetEnvironmentMarker).c_str(), QByteArray::fromStdString(names));
        _selfOnlyEnvironment.emplace_back(display::SelfSetEnvironmentMarker);
        display::displayLog()("renderer.gpu: set {} so that OpenGL renders on '{}'.", names, chosen.title);
    }
#else
    // Spec 4.3: no supported way to choose an OpenGL GPU here.
    auto const& renderer = _config.renderer.value();
    if (renderer.renderingBackend == config::RenderingBackend::OpenGL
        && renderer.gpu.preference != config::GpuPreference::Auto)
        errorLog()(
            "renderer.gpu is ignored for OpenGL on this platform; use Direct3D or Vulkan where available.");
#endif
}

std::expected<void, std::string> ContourGuiApp::awaitNativeAttach(std::chrono::milliseconds budget,
                                                                  std::string hostLabel)
{
    if (auto connected = _nativeController->awaitConnected(budget); !connected)
    {
        _nativeController.reset();
        return std::unexpected(connected.error());
    }
    _attachedHostLabel = std::move(hostLabel);
    _routingFactory->setDelegate(_nativeController.get());
    // After the connect: the drift offer reads what the handshake negotiated. Only a daemon this client
    // can restart -- a local one, which is what _restartDaemon is set for -- is offered a restart.
    if (_restartDaemon)
        offerDaemonRestartIfDrifted(*_nativeController);
    return {};
}

void ContourGuiApp::offerDaemonRestartIfDrifted(remote::NativeController const& controller)
{
    auto const layout = controller.negotiated()
                            .transform(vthost::client::layoutSurvivesRestart)
                            .value_or(vthost::client::LayoutAfterRestart::Lost);
    _daemonDrift.show(
        vthost::client::assessDrift(CONTOUR_VERSION_STRING, controller.daemonRelease(), layout));
    connect(&_daemonDrift, &window::DaemonDriftModel::restartRequested, this, [this] {
        // Stopping the daemon drops this client's connection, which closes every window. Without this
        // the last window closing would quit the app before the queued hand-off below ever ran.
        QGuiApplication::setQuitOnLastWindowClosed(false);
        // Ended here, rather than lost when the daemon stops: a lost connection is sought again, and a
        // reconnect would start a daemon of its own while the restart below waits for none to accept.
        // Before stop(), which closes every mirror pty -- @see TerminalSession::noteTransportLost
        if (_nativeController)
        {
            _sessionManager.noteTransportLost();
            _nativeController->stop();
        }
        _daemonRestartThread = std::jthread { [this] {
            auto result = _restartDaemon();
            QMetaObject::invokeMethod(
                &_daemonDrift,
                [this, result = std::move(result)] {
                    QGuiApplication::setQuitOnLastWindowClosed(true);
                    if (!result)
                    {
                        _daemonDrift.noteRestartFailed(QString::fromStdString(result.error()));
                        quitIfWindowless();
                        return;
                    }
                    // Until the client reconnects by itself, the new process is what attaches to the
                    // new daemon.
                    auto arguments = QStringList {};
                    for (auto const argument: std::span { _argv, static_cast<size_t>(_argc) }.subspan(1))
                        arguments.push_back(QString::fromUtf8(argument));
                    if (auto const started =
                            externalLauncher().runDetached(QString::fromStdString(programPath()), arguments);
                        !started)
                    {
                        _daemonDrift.noteRestartFailed(
                            tr("the daemon restarted, but this window could not be reopened"));
                        quitIfWindowless();
                        return;
                    }
                    QCoreApplication::quit();
                },
                Qt::QueuedConnection);
        } };
    });
}

std::expected<void, std::string> ContourGuiApp::restartLocalDaemon(std::string const& label,
                                                                   std::filesystem::path const& controlPath,
                                                                   vthost::DaemonSpawnOptions const& options)
{
    auto const endpoint = vthost::UnixEndpoint { .socketPath = controlPath };
    auto start = LocalDaemonStart { processEnvironment(), fileSystem(), programPath() };
    auto const starter = start.starter(label, endpoint, *_processPlacement, options);
    // Never starts a daemon itself: the restart below starts the new one through the starter. The stop waits
    // for the old daemon to write the workspace the new one restores.
    auto const control = vthost::client::makeDaemonControl(
        std::make_shared<vthost::UnixTransportFactory>(controlPath, nullptr),
        vthost::client::DefaultStopTimeout);
    return vthost::client::restartDaemon(*control, *starter)
        .transform_error([](vthost::client::DaemonRestartFailure const& failure) { return failure.detail; });
}

namespace
{
    /// Each would be silently ignored beside `--host`: the host's entry says where and how to connect.
    constexpr auto HostExclusions = std::array {
        cli::OptionPresence { .name = "connect-tcp", .givenAs = cli::GivenAs::Text },
        cli::OptionPresence { .name = "socket", .givenAs = cli::GivenAs::Text },
        cli::OptionPresence { .name = "tmux", .givenAs = cli::GivenAs::Flag },
        cli::OptionPresence { .name = "token", .givenAs = cli::GivenAs::Text },
        cli::OptionPresence { .name = "token-file", .givenAs = cli::GivenAs::Text },
        cli::OptionPresence { .name = "tls-ca", .givenAs = cli::GivenAs::Text },
        // A host attach mirrors all of the remote daemon's windows; one window would need its own intent.
        cli::OptionPresence { .name = "window", .givenAs = cli::GivenAs::Text },
    };
} // namespace

std::expected<cli::HostTransport, std::string> ContourGuiApp::resolveClientHost(std::string const& nameOrUrl,
                                                                                ClientRequest& request)
{
    if (auto const given = firstGivenOption("contour.client", HostExclusions))
        return std::unexpected { std::format("pass either --host or --{}, not both", *given) };
    auto host = config::resolveHost(request.config, nameOrUrl);
    if (!host)
        return std::unexpected { host.error() };
    // The host's profile, unless --profile names one: the GUI renders with it and the daemon creates with it.
    if (request.profile.empty())
        request.profile = host->profile;
    // A saved host by its name, an ad-hoc one by its URL, canonically spelled.
    auto label = nameOrUrl.contains("://") ? config::toString(host->url) : nameOrUrl;
    return cli::makeHostTransport(
        std::move(label),
        *host,
        fileSystem(),
        vthost::makeSystemStdioSpawner(vtpty::ProgramSearch {
            .path = _sshSearchPath, .workingDirectory = {}, .fileSystem = fileSystem() }));
}

int ContourGuiApp::clientAction()
{
    // Installed first, so an ensureDaemon spawn failure is captured. Read here with the other client-verb
    // parameters, BEFORE the re-parse further down replaces the flag store with the terminal verb's surface
    // and drops every contour.client.* key.
    auto request = ClientRequest {};
    request.logFilter = parameters().get<string>("contour.client.log");
    request.logFile = parameters().get<string>("contour.client.log-file");
    if (auto const installed = installLogging("contour.client", /*showProcessId=*/true); !installed)
    {
        cerr << std::format("contour client: {}\n", installed.error());
        return EXIT_FAILURE;
    }
    _latencyProbeRequest = readLatencyProbeRequest("contour.client");

    request.engine =
        parameters().get<bool>("contour.client.tmux") ? ClientEngine::Tmux : ClientEngine::Native;
    request.tmuxSocket = parameters().get<string>("contour.client.tmux-socket");
    request.socket = parameters().get<string>("contour.client.socket");
    request.label = parameters().get<string>("contour.client.label");
    request.profile = parameters().get<string>("contour.client.profile");
    request.config = parameters().get<string>("contour.client.config");
    request.connectTcp = parameters().get<string>("contour.client.connect-tcp");
    request.tlsCaPath = parameters().get<string>("contour.client.tls-ca");
    request.sizePolicy = parameters().get<string>("contour.client.size-policy");
    if (auto const window = parameters().get<string>("contour.client.window"); !window.empty())
    {
        auto id = uint64_t {};
        if (auto const [end, error] = std::from_chars(window.data(), window.data() + window.size(), id);
            error != std::errc {} || end != window.data() + window.size())
        {
            cerr << std::format("contour client: invalid --window '{}' (expected a daemon window id)\n",
                                window);
            return EXIT_FAILURE;
        }
        request.window = id;
    }
    auto token = vthost::resolveToken(parameters().get<string>("contour.client.token"),
                                      parameters().get<string>("contour.client.token-file"));
    if (!token)
    {
        cerr << std::format("contour client: {}\n", token.error());
        return EXIT_FAILURE;
    }
    request.token = *std::move(token);
    if (auto const host = parameters().get<string>("contour.client.host"); !host.empty())
    {
        auto resolved = resolveClientHost(host, request);
        if (!resolved)
        {
            cerr << std::format("contour client: {}\n", resolved.error());
            return EXIT_FAILURE;
        }
        request.host = *std::move(resolved);
    }

    // Boot the GUI under the terminal verb's parameter surface: re-parse a synthetic argv so every
    // contour.terminal.* key resolves, carrying the attach-specific profile/config choices over.
    auto argv = std::vector<char const*> { "contour", "terminal" };
    if (!request.profile.empty())
    {
        argv.push_back("profile");
        argv.push_back(request.profile.c_str());
    }
    if (!request.config.empty())
    {
        argv.push_back("config");
        argv.push_back(request.config.c_str());
    }
    if (!reparseParameters(static_cast<int>(argv.size()), argv.data()))
        return EXIT_FAILURE;

    _clientRequest = std::move(request);
    auto const rv = terminalGuiAction();
    _clientRequest.reset();
    return rv;
}

std::expected<void, std::string> ContourGuiApp::establishHosting()
{
    // The verb that launched, for what a failure is prefixed with.
    auto const verb = std::string_view { _clientRequest ? "contour client" : "contour" };
    auto const mode =
        cli::resolveDaemonMode(_clientRequest ? std::optional { config::DaemonMode::Required } : std::nullopt,
                               parameters().get<string>("contour.terminal.daemon-mode"),
                               _config.daemon.value().mode);
    if (!mode)
        return std::unexpected(std::format("{}: {}", verb, mode.error()));
    _daemonMode = *mode;
    // A program the command line names runs here or nowhere: a daemon session runs the host's profile.
    auto const program = _cliCommand ? cli::LaunchProgram::Named : cli::LaunchProgram::Profile;
    if (!cli::attemptsAttach(_daemonMode, program))
    {
        if (cli::decideHosting(_daemonMode, program, std::nullopt) == cli::HostingDecision::Fail)
            return std::unexpected(std::format("{}: {}", verb, cli::NamedProgramRefusal));
        return {};
    }
    if (_clientRequest && _clientRequest->engine == ClientEngine::Tmux)
        return startTmuxAttach(*_clientRequest);
    return startNativeAttach().transform_error(
        [verb](std::string const& reason) { return std::format("{}: {}", verb, reason); });
}

std::expected<void, std::string> ContourGuiApp::startTmuxAttach(ClientRequest const& request)
{
    _tmuxController = std::make_unique<remote::TmuxController>(
        request.tmuxSocket, _processPlacement, fileSystem(), core::platform::defaultSteadyClock());
    if (auto connected = _tmuxController->connectAndWait(std::chrono::seconds(10)); !connected)
    {
        _tmuxController.reset();
        return std::unexpected(std::format("contour client --tmux: {}", connected.error()));
    }
    _routingFactory->setDelegate(_tmuxController.get());
    _onGuiBooted = [this] {
        if (auto const window = _sessionManager.focusedWindow())
            _tmuxController->adoptPendingPanes(_sessionManager, *window);
    };
    connect(_tmuxController.get(),
            &remote::TmuxController::remotePaneDiscovered,
            this,
            _onGuiBooted,
            Qt::QueuedConnection);
    // A %window-renamed reflects onto the owning tab's title (a tmux window is a tab).
    connect(
        _tmuxController.get(),
        &remote::TmuxController::tabTitleChanged,
        this,
        [this] { _tmuxController->applyPendingRenames(_sessionManager); },
        Qt::QueuedConnection);
    connect(
        _tmuxController.get(),
        &remote::TmuxController::connectionClosed,
        this,
        [this] { _tmuxController->stop(); },
        Qt::QueuedConnection);
    return {};
}

namespace
{
    /// Where the fonts shipped with the application live, relative to the running executable:
    /// `Contents/Resources/fonts` in the macOS bundle, `<prefix>/share/contour/fonts` on Linux
    /// (the executable being `<prefix>/bin/contour`). Windows ships none; it uses Cascadia Mono.
    [[nodiscard]] text::BundledFontDirectories bundledFontDirectories(fs::path const& executableDirectory)
    {
#if defined(__APPLE__)
        return { .paths = { (executableDirectory / ".." / "Resources" / "fonts").lexically_normal() } };
#elif defined(_WIN32)
        (void) executableDirectory;
        return {};
#else
        return { .paths = {
                     (executableDirectory / ".." / "share" / "contour" / "fonts").lexically_normal() } };
#endif
    }

    /// Where a native attach connects, and what it follows there.
    struct NativeAttachTarget
    {
        std::shared_ptr<vthost::ITransportFactory> transport;
        vthost::proto::AttachIntent intent;
    };

    /// @param connectTcp `--connect-tcp`: the daemon's HOST:PORT.
    /// @param token The token it is sent.
    /// @param tlsCaPath `--tls-ca`: the PEM pinning its certificate; empty to trust on first use.
    /// @return The TLS transport to a daemon over TCP, which a client follows every window of.
    [[nodiscard]] std::expected<NativeAttachTarget, std::string> tcpAttachTarget(
        std::string const& connectTcp,
        std::string const& token,
        std::string const& tlsCaPath,
        core::platform::FileSystem const& fileSystem)
    {
        auto const hostPort = vthost::parseHostPort(connectTcp);
        if (!hostPort)
            return std::unexpected(
                std::format("invalid --connect-tcp '{}' (expected HOST:PORT)", connectTcp));
        auto tcp = vthost::TcpEndpoint {
            .host = hostPort->first, .port = hostPort->second, .token = token, .caPem = {}
        };
        if (!tlsCaPath.empty())
        {
            auto caPem = cli::readPinnedCa(
                fileSystem, core::pathFromUtf8(tlsCaPath).value_or(fs::path {}), "--tls-ca");
            if (!caPem)
                return std::unexpected(caPem.error());
            tcp.caPem = *std::move(caPem);
        }
        return NativeAttachTarget {
            .transport = std::make_shared<vthost::TlsTransportFactory>(std::move(tcp)),
            .intent = vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows,
                                                    .windows = {} },
        };
    }
} // namespace

void ContourGuiApp::openDaemonSpawnGate() noexcept
{
    _daemonSpawnGate->store(DaemonSpawnGate::Open);
    _daemonSpawnGate->notify_all();
}

std::expected<void, std::string> ContourGuiApp::startNativeAttach()
{
    auto const& settings = _config.daemon.value();
    auto target = std::expected<NativeAttachTarget, std::string> {};
    // Ten seconds for the local daemon: the first connect starts it too, when none serves, and that waits up
    // to five seconds for it to accept before the connect's own share of the budget begins.
    auto budget = std::chrono::milliseconds { std::chrono::seconds(10) };
    auto hostLabel = std::string {};
    if (_clientRequest && _clientRequest->host)
    {
        // A remote daemon is started by the remote relay, never from here; a client follows every window.
        auto& host = *_clientRequest->host;
        budget = std::max(host.connectTimeout, std::chrono::milliseconds { remote::MinimumConnectAwait });
        hostLabel = host.label;
        target = NativeAttachTarget {
            .transport = host.factory,
            .intent = vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::AllWindows,
                                                    .windows = {} },
        };
    }
    else if (_clientRequest && !_clientRequest->connectTcp.empty())
        target = tcpAttachTarget(
            _clientRequest->connectTcp, _clientRequest->token, _clientRequest->tlsCaPath, fileSystem());
    else
    {
        // The local daemon: its socket and size policy are the client's options, else the `daemon:` section.
        auto const label =
            _clientRequest ? _clientRequest->label : std::string { vthost::DefaultSocketLabel };
        auto const controlPath = vthost::muxSocketPath(
            label, cli::resolveSocketOption(_clientRequest ? _clientRequest->socket : "", settings.socket));
        auto const sizePolicy =
            cli::resolveSizePolicy(_clientRequest ? _clientRequest->sizePolicy : "", settings.sizePolicy);
        if (!sizePolicy)
            return std::unexpected(sizePolicy.error());
        // The spawned daemon inherits our logging: it is the instance nobody can configure by hand and whose
        // stderr lands in this process's terminal. It inherits our config and profile for the same reason --
        // its hosted terminals must emulate the way THIS client's configuration says, or the scrollback it
        // retains disagrees with what we render.
        auto const daemonOptions = vthost::DaemonSpawnOptions {
            .filter =
                _clientRequest ? _clientRequest->logFilter : parameters().get<string>("contour.terminal.log"),
            .logFile = _clientRequest ? _clientRequest->logFile
                                      : parameters().get<string>("contour.terminal.log-file"),
            .configPath =
                _clientRequest ? _clientRequest->config : parameters().get<string>("contour.terminal.config"),
            .profileName = _clientRequest ? _clientRequest->profile
                                          : parameters().get<string>("contour.terminal.profile"),
            .sizePolicy = *sizePolicy,
        };
        _restartDaemon = [this, label, controlPath, daemonOptions] {
            return restartLocalDaemon(label, controlPath, daemonOptions);
        };
        // Before every connect, the first and each reconnect alike, so a daemon that crashed or was stopped
        // is started again. Spawned, if it must be, from this version's own copy where an upgrade would
        // disturb the installed binary, and that copy held leased until the daemon serves. Runs on the
        // transport's worker thread, one call at a time, so the attach's reactor goes on serving -- and
        // stop() does not wait -- while a spawned daemon comes up; nothing else uses what it captures.
        auto ensureDaemon =
            [start = std::make_shared<LocalDaemonStart>(processEnvironment(), fileSystem(), programPath()),
             spawnGate = _daemonSpawnGate,
             placement = _processPlacement,
             label,
             controlPath,
             daemonOptions]() -> std::expected<void, vthost::TransportError> {
            if (start->spawner->probe(controlPath) == vthost::DaemonPresence::Serving)
                return {};
            spawnGate->wait(DaemonSpawnGate::Closed);
            // A registered service is started before a daemon is spawned on demand.
            auto const starter = start->starter(
                label, vthost::UnixEndpoint { .socketPath = controlPath }, *placement, daemonOptions);
            if (auto const started = starter->start(); !started)
                // Transient: a reconnect tries again, and a first connect is not retried anyway.
                return std::unexpected(vthost::TransportError {
                    .kind = vthost::TransportErrorKind::Unreachable, .message = started.error() });
            return {};
        };
        // A plain launch opens one new window beside whatever the daemon already shows, or brings them all
        // back when no other client does; `--window` shows exactly one.
        auto intent =
            vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Launch, .windows = {} };
        if (_clientRequest && _clientRequest->window)
            intent = vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Windows,
                                                   .windows = { *_clientRequest->window } };
        target = NativeAttachTarget {
            .transport = std::make_shared<vthost::UnixTransportFactory>(controlPath, std::move(ensureDaemon)),
            .intent = std::move(intent),
        };
    }
    if (!target)
        return std::unexpected(target.error());

    // The emulation settings the daemon gives the sessions this client creates, from the configuration
    // loaded already; every session created later takes them from the configuration in memory, exactly
    // as a local new tab does, so a reload reaches remote and local tabs alike. A failure is a warning:
    // hosting under the daemon's own resolved profile is a sane thing to do meanwhile.
    auto attachSettings = std::optional<vtbackend::Settings> {};
    if (auto resolved = currentEmulationSettings(); resolved)
        attachSettings = *std::move(resolved);
    else
        errorLog()("attach: {}; the daemon's own profile will be used for new sessions.", resolved.error());

    _nativeController = std::make_unique<remote::NativeController>(
        std::move(target->transport),
        std::move(target->intent),
        profileName(),
        std::move(attachSettings),
        [this] { return currentEmulationSettings(); },
        core::platform::defaultSteadyClock(),
        _startupTimeline);
    // A window spawned to host a daemon window binds itself here (from its Main.qml, via
    // consumeAttachWindow) instead of creating a fresh first tab.
    _sessionManager.setAttachWindowBinder(
        [this](window::WindowController* controller) { return bindPendingAttachWindow(controller); });
    // Reconcile the GUI against the daemon's authoritative window→tab→split tree (B2/B4): whenever a layout
    // arrives or changes (GUI boot, or a window/tab/split authored on the daemon, here or by another
    // client), map each daemon window onto an OS window and realize any tab not yet shown, each pane bound
    // to its remote session. Incremental — what is already shown is left untouched.
    _onGuiBooted = [this] {
        reconcileAttachWindows();
    };
    connect(_nativeController.get(),
            &remote::NativeController::layoutChanged,
            this,
            _onGuiBooted,
            Qt::QueuedConnection);
    connectReconnectSignals();
    // A dying connection ends every mirror session (stop() closes the ptys), closing the tabs through the
    // shell-exit teardown.
    connect(
        _nativeController.get(),
        &remote::NativeController::connectionClosed,
        this,
        [this] {
            // Null once a connect that failed has been given up on (see _awaitAttach).
            if (!_nativeController)
                return;
            // Before stop(), which closes every mirror pty — the order matters.
            // @see TerminalSession::noteTransportLost
            _sessionManager.noteTransportLost();
            _nativeController->stop();
        },
        Qt::QueuedConnection);
#ifdef CONTOUR_EXTENSIONS
    connect(
        _nativeController.get(),
        &remote::NativeController::extensionPermissionRequested,
        this,
        [this] { askHostedExtensions(); },
        Qt::QueuedConnection);
#endif

    // The connect, hello, first spawn and first snapshot run on the reactor while the GUI boots;
    // terminalGuiAction() waits for them through _awaitAttach just before the first window, by when booting
    // has already spent part of the budget, and settles the hosting there. The signals above are connected
    // first, so nothing the connection emits meanwhile is lost. A cold start's daemon start is part of the
    // connect, in the transport's ensureDaemon, on a worker thread of the transport's own while the GUI
    // boots.
    _nativeController->startConnecting();
    _awaitAttach = [this, budget, hostLabel = std::move(hostLabel)] {
        return awaitNativeAttach(budget, hostLabel);
    };
    return {};
}

std::expected<void, std::string> ContourGuiApp::settleHosting(
    std::expected<void, std::string> const& attached)
{
    switch (cli::decideHosting(_daemonMode,
                               attached ? cli::AttachResult::Attached : cli::AttachResult::Unreachable))
    {
        case cli::HostingDecision::InProcess:
        case cli::HostingDecision::Attached: return {};
        case cli::HostingDecision::InProcessWithNotice:
            // The attach is given up (awaitNativeAttach() released the controller): nothing waits for the
            // daemon's windows, and the first window opens a tab of its own, which runs here.
            errorLog()("Hosting this window's sessions in-process: {}", attached.error());
            _onGuiBooted = nullptr;
            _restartDaemon = nullptr;
            _sessionManager.setAttachWindowBinder(nullptr);
            _hostingNotice = cli::hostingNotice(attached.error());
            return {};
        case cli::HostingDecision::Fail:
            return std::unexpected(
                std::format("{}: {}", _clientRequest ? "contour client" : "contour", attached.error()));
    }
    return {};
}

int ContourGuiApp::run(int argc, char const* argv[])
{
    _argc = argc;
    _argv = argv;

    return cli::ContourApp::run(argc, argv);
}

core::cli::Command ContourGuiApp::parameterDefinition() const
{
    auto command = cli::ContourApp::parameterDefinition();

    // NOLINTBEGIN
    command.children.insert(
        command.children.begin(),
        CLI::Command {
            "client",
            "(experimental) Connects to a terminal multiplexer daemon in a GUI window (auto-spawns "
            "the daemon if needed). Its options and wire protocol may still change between "
            "releases.",
            CLI::OptionList {
                CLI::Option { "socket",
                              CLI::Value { ""s },
                              "Path of the daemon's control socket file. Defaults to the "
                              "configuration's `daemon.socket`, else $XDG_RUNTIME_DIR/contour/LABEL "
                              "(respecting $CONTOUR_MUX).",
                              "PATH" },
                CLI::Option { "label",
                              CLI::Value { std::string { vthost::DefaultSocketLabel } },
                              "Socket label distinguishing daemon instances.",
                              "NAME" },
                CLI::Option { "tmux",
                              CLI::Value { false },
                              "Attaches to a real tmux server (spawns `tmux -C attach-session`): "
                              "tmux windows become tabs, panes become splits." },
                CLI::Option {
                    "tmux-socket", CLI::Value { ""s }, "tmux server socket path (-S) for --tmux.", "PATH" },
                CLI::Option { "profile",
                              CLI::Value { ""s },
                              "Config profile the GUI renders remote sessions with.",
                              "NAME" },
                CLI::Option {
                    "config", CLI::Value { ""s }, "Path to configuration file the GUI loads.", "FILE" },
                CLI::Option { "host",
                              CLI::Value { ""s },
                              "Attaches to a remote host: a name under the configuration's `hosts:`, or a "
                              "URL, `ssh://[USER@]HOST[:PORT]` (key or agent authentication only). The "
                              "remote host's daemon is started there when none runs.",
                              "NAME|URL" },
                CLI::Option { "connect-tcp",
                              CLI::Value { ""s },
                              "Connect to a TCP daemon at HOST:PORT (TLS-encrypted, "
                              "token-authenticated) instead of the local socket.",
                              "HOST:PORT" },
                CLI::Option { "token",
                              CLI::Value { ""s },
                              "Preshared token sent to a --connect-tcp daemon. Visible to other "
                              "local users via the process list; prefer --token-file.",
                              "TOKEN" },
                CLI::Option { "token-file",
                              CLI::Value { ""s },
                              "Reads the preshared token from FILE instead of the command line. "
                              "Trailing newlines are ignored.",
                              "FILE" },
                CLI::Option { "tls-ca",
                              CLI::Value { ""s },
                              "PEM trust anchor pinning the daemon's TLS certificate "
                              "for --connect-tcp. Omitted = TOFU (encrypt, don't verify; "
                              "the token authenticates).",
                              "FILE" },
                CLI::Option { "log",
                              CLI::Value { ""s },
                              "Enables logging for a comma (,) separated list of tags, or `all` "
                              "(see `contour list-debug-tags`). Also passed to an auto-spawned "
                              "daemon. Overrides $LOG.",
                              "TAGS" },
                CLI::Option { "log-file",
                              CLI::Value { ""s },
                              "Appends log output to FILE instead of standard error ('-' means "
                              "standard error). Also passed to an auto-spawned daemon.",
                              "FILE" },
                latencyProbeFileOption(),
                latencyProbeVsyncOption(),
                latencyProbeKeysOption(),
                CLI::Option { "size-policy",
                              CLI::Value { ""s },
                              "Which attached client's size the shared grid takes when several "
                              "clients of different sizes are attached: `latest`, `smallest` or "
                              "`largest` (see `contour daemon --help`). Defaults to the "
                              "configuration's `daemon.size_policy`. Applies only to an "
                              "auto-spawned daemon; one already running keeps the policy it was "
                              "started with.",
                              "POLICY" },
                CLI::Option { "window",
                              CLI::Value { ""s },
                              "Shows only the daemon window with this id (see `contour daemon "
                              "status`) instead of opening a new one; fails when the daemon has "
                              "no such window.",
                              "ID" },
            },
        });

    command.children.insert(
        command.children.begin(),
        CLI::Command {
            "font-locator",
            "Inspects font locator service.",
            CLI::OptionList {
                CLI::Option { "config",
                              CLI::Value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::Option {
                    "profile", CLI::Value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::Option { "debug",
                              CLI::Value { ""s },
                              "Enables debug logging, using a comma (,) separated list of tags. "
                              "Writes to standard error unless `log-file` names a file.",
                              "TAGS" },
                // `log` and `log-file` are not optional here: both verbs funnel through
                // loadConfig(), which reads them unconditionally, and FlagStore::get() is a
                // `values.at(key)` over the options the *invoked* verb declared. Leaving either
                // out of this list is not "no logging for font-locator", it is a std::out_of_range
                // thrown before the verb does anything at all.
                CLI::Option { "log",
                              CLI::Value { ""s },
                              "Enables logging for a comma (,) separated list of tags, or `all` "
                              "(see `contour list-debug-tags`). Synonym for `debug`. Writes to "
                              "standard error unless `log-file` names a file.",
                              "TAGS" },
                CLI::Option { "log-file",
                              CLI::Value { ""s },
                              "Appends log output to FILE instead of standard error ('-' means "
                              "standard error).",
                              "FILE" },
            },
        });

    command.children.insert(
        command.children.begin(),
        CLI::Command {
            "terminal",
            "Spawns a new terminal application.",
            CLI::OptionList {
                CLI::Option { "config",
                              CLI::Value { contour::config::defaultConfigFilePath() },
                              "Path to configuration file to load at startup.",
                              "FILE" },
                CLI::Option {
                    "profile", CLI::Value { ""s }, "Terminal Profile to load (overriding config).", "NAME" },
                CLI::Option { "debug",
                              CLI::Value { ""s },
                              "Enables debug logging, using a comma (,) separated list of tags. "
                              "Writes to standard error unless `log-file` names a file.",
                              "TAGS" },
                CLI::Option { "log",
                              CLI::Value { ""s },
                              "Enables logging for a comma (,) separated list of tags, or `all` "
                              "(see `contour list-debug-tags`). Synonym for `debug`. Writes to "
                              "standard error unless `log-file` names a file.",
                              "TAGS" },
                // Shell redirection is NOT an alternative to this on Windows: tryAttachConsole()
                // (main.cpp) runs AttachConsole() and then freopen()s stderr onto CONOUT$, which
                // replaces a `2> file` redirection before a single log line is written. The result
                // is a file holding only the pre-attach noise while the log itself goes to the
                // console -- indistinguishable, to whoever is reading the file, from "the thing you
                // asked about never happened". Writing through the sink sidesteps the console
                // entirely.
                CLI::Option { "log-file",
                              CLI::Value { ""s },
                              "Appends log output to FILE instead of standard error ('-' means "
                              "standard error).",
                              "FILE" },
                CLI::Option { "live-config", CLI::Value { false }, "Enables live config reloading." },
                CLI::Option { "daemon-mode",
                              CLI::Value { ""s },
                              "Where the sessions run: `in_process` (in this process), `auto` (in "
                              "the daemon, started if need be, else in this process with a notice) "
                              "or `required` (in the daemon, or not at all). Defaults to the "
                              "configuration's `daemon.mode`.",
                              "MODE" },
                latencyProbeFileOption(),
                latencyProbeVsyncOption(),
                latencyProbeKeysOption(),
                CLI::Option {
                    "dump-state-at-exit",
                    CLI::Value { ""s },
                    "Dumps internal state at exit into the given directory. This is for debugging contour.",
                    "PATH" },
                CLI::Option { "early-exit-threshold",
                              CLI::Value { -1 },
                              "If the spawned process exits earlier than the given threshold seconds, an "
                              "error message will be printed and the window not closed immediately." },
                CLI::Option { "working-directory",
                              CLI::Value { ""s },
                              "Sets initial working directory (overriding config).",
                              "DIRECTORY" },
                CLI::Option { "layout", CLI::Value { ""s }, "Opens the named layout at startup.", "NAME" },
                CLI::Option {
                    "class",
                    CLI::Value { ""s },
                    "Sets the class part of the WM_CLASS property for the window (overriding config).",
                    "WM_CLASS" },
                CLI::Option {
                    "platform", CLI::Value { ""s }, "Sets the QPA platform.", "PLATFORM[:OPTIONS]" },
                CLI::Option { "session",
                              CLI::Value { ""s },
                              "Sets the sessioni ID used for resuming a prior session.",
                              "SESSION_ID" },
#ifdef __linux__
                CLI::Option {
                    "display", CLI::Value { ""s }, "Sets the X11 display to connect to.", "DISPLAY_ID" },
#endif
                CLI::Option {
                    CLI::OptionName { 'e', "execute" },
                    CLI::Value { ""s },
                    "DEPRECATED: Program to execute instead of running the shell as configured.",
                    "PROGRAM",
                    CLI::Presence::Optional,
                    CLI::Deprecated { "Only supported for compatibility with very old KDE desktops." } },
            },
            CLI::CommandList {},
            CLI::CommandSelect::Implicit,
            CLI::Verbatim { "PROGRAM ARGS...",
                            "Executes given program instead of the one provided in the configuration." } });

    // NOLINTEND
    return command;
}

std::chrono::seconds ContourGuiApp::earlyExitThreshold() const
{
    auto const configThreshold = config().earlyExitThreshold.value();
    auto const parameterThreshold = parameters().get<int>("contour.terminal.early-exit-threshold");

    // default threshold is documentation::DefaultEarlyExitThreshold seconds
    if (parameterThreshold >= 0)
        return std::chrono::seconds(parameterThreshold);

    if (configThreshold != config::documentation::DefaultEarlyExitThreshold)
        return std::chrono::seconds(configThreshold);

    return std::chrono::seconds(config::documentation::DefaultEarlyExitThreshold);
}

std::expected<vtbackend::Settings, std::string> ContourGuiApp::currentEmulationSettings() const
{
    return config::emulationSettingsFor(_config, profileName());
}

string ContourGuiApp::profileName() const
{
    if (auto profile = parameters().get<string>("contour.terminal.profile"); !profile.empty())
        return profile;

    if (!_config.defaultProfileName.value().empty())
        return _config.defaultProfileName.value();

    if (_config.profiles.value().size() == 1)
        return _config.profiles.value().begin()->first;

    return ""s;
}

std::string ContourGuiApp::layoutName() const
{
    if (auto name = parameters().get<std::string>("contour.terminal.layout"); !name.empty())
        return name;
    return config().defaultLayoutName.value();
}

std::optional<fs::path> ContourGuiApp::dumpStateAtExit() const
{
    auto const path = parameters().get<std::string>("contour.terminal.dump-state-at-exit");
    auto file = core::pathFromUtf8(path);
    if (path.empty() || !file)
        return std::nullopt;
    return *std::move(file);
}

void ContourGuiApp::onExit(session::TerminalSession& session)
{
    if (auto const* localProcess = dynamic_cast<vtpty::Process const*>(&session.terminal().device()))
        _exitStatus = localProcess->checkStatus();
#ifdef VTPTY_LIBSSH2
    else if (auto const* sshSession = dynamic_cast<vtpty::SshSession const*>(&session.terminal().device()))
        _exitStatus = sshSession->exitStatus();
#endif
}

void ContourGuiApp::applyGuiTheme(config::GuiTheme theme,
                                  config::ColorConfig const& colors,
                                  std::optional<vtbackend::RGBColor> liveBackground)
{
    // platform::qtColorSchemeFor() is the pure decision (see GuiTheme.hpp): a forced scheme for dark/light,
    // std::nullopt for system.
    auto const scheme = platform::qtColorSchemeFor(theme, colors, liveBackground);

    // Force the chrome palette explicitly. This is the load-bearing step: QStyleHints::setColorScheme
    // does NOT regenerate QGuiApplication::palette() on platforms whose platform-theme plugin owns the
    // palette (KDE Plasma, GNOME, …), so relying on it alone leaves the chrome uncolored on the Linux
    // desktop. Setting the application palette via setPalette does recolor it, and every QML
    // SystemPalette follows — so all chrome recolors without touching any component.
    if (scheme)
    {
        // Capture the OS palette once, before the first override, so System can later restore it.
        if (!_appliedGuiScheme.has_value())
            _guiSystemPalette = QGuiApplication::palette();
        _appliedGuiScheme = scheme;
        display::displayLog()("Applying GUI theme override: {}", theme);
        QGuiApplication::setPalette(platform::buildThemePalette(*scheme));
    }
    else if (_appliedGuiScheme.has_value())
    {
        // Returning to System after a prior dark/light override: restore the captured OS palette.
        // Note: once an explicit palette has been set, Qt no longer auto-tracks live OS theme
        // switches for the chrome until the next restart (the documented System-mode tradeoff).
        display::displayLog()("Applying GUI theme: system (restore OS palette)");
        QGuiApplication::setPalette(_guiSystemPalette);
        _appliedGuiScheme = std::nullopt;
    }
    else
        display::displayLog()("Applying GUI theme: system (follow OS color scheme)");

#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
    // Also drive QStyleHints so QStyleHints::colorScheme() reports the pinned scheme: Qt Quick
    // Controls internals and the platforms that DO regenerate their own palette (Windows/macOS) honor
    // it. On the palette-owning Linux platform themes this is inert on its own — hence the explicit
    // setPalette above. Setting the scheme emits no colorSchemeChanged to the terminal grid unless the
    // reported scheme actually changes, and the grid handler tracks the OS whenever the chrome is not pinned
    // (_appliedGuiScheme empty).
    if (scheme)
        QGuiApplication::styleHints()->setColorScheme(*scheme);
    else
        QGuiApplication::styleHints()->unsetColorScheme();
#endif
}

void ContourGuiApp::followLiveBackground(vtbackend::RGBColor liveBackground)
{
    // Only `theme: terminal` follows the terminal's background; any other theme would only repaint the
    // whole application for the scheme it already shows. Likewise when the background moved without
    // crossing the light/dark threshold.
    auto const theme = _config.theme.value();
    if (theme != config::GuiTheme::Terminal)
        return;
    if (platform::qtColorSchemeFor(theme, defaultProfileColors(), liveBackground) == _appliedGuiScheme)
        return;
    applyGuiTheme(theme, defaultProfileColors(), liveBackground);
}

config::WindowControlStyle ContourGuiApp::resolvedWindowControlStyle(
    config::WindowControlStyle configured) const
{
    // Resolving here rather than in the provider is deliberate: only a composition root knows the
    // environment to resolve Auto against, and keeping the provider a pure function of one concrete
    // enumerator is what makes it drivable from a test with no environment at all.
    //
    // One place does it, so the style the first frame draws and the style every later reload applies
    // cannot come to different answers about what this host is.
    return config::resolveWindowControlStyle(configured, config::detectHostPlatform(processEnvironment()));
}

void ContourGuiApp::applyWindowControlStyle(config::WindowControlStyle style)
{
    auto const resolved = resolvedWindowControlStyle(style);

    display::displayLog()("Applying window control style: {} (configured: {})", resolved, style);

    // Guarded, exactly as applyGuiTheme is safe before the GUI exists: this is public and documented
    // as callable at startup, and the provider is built partway through terminalGuiAction(). Any
    // caller reaching it earlier -- a headless verb, a test fixture that builds a WindowController
    // without ever running the GUI action -- would otherwise dereference a null unique_ptr. There is
    // nothing to apply to yet, and the provider is constructed with the resolved style regardless.
    if (_windowControlStyleProvider)
        _windowControlStyleProvider->setStyle(resolved);
}

config::AccessibilityPreferences ContourGuiApp::accessibilityPreferences() const
{
    return _accessibilitySource->current();
}

platform::PlatformAppearance ContourGuiApp::platformAppearance() const
{
    return _platformAppearance->current();
}

window::ChromeInputs ContourGuiApp::chromeInputs() const
{
    auto const* palette = config::preferredColorPalette(defaultProfileColors(), _colorPreference);
    return window::ChromeInputs {
        .row = _chromeStyle,
        .font = window::resolveChromeFont(_config, _chromeStyle, profileName()),
        .accessibility = accessibilityPreferences(),
        .appearance = platformAppearance(),
        .scheme = config::chromeSchemeColorsOf(palette ? *palette : vtbackend::ColorPalette {}),
    };
}

void ContourGuiApp::refreshChromeStyle()
{
    // Guarded as applyWindowControlStyle() is: the host is built partway through terminalGuiAction(),
    // and a reload or an OS change may arrive before it (or without it, in a headless test).
    if (_chromeStyleHost)
        _chromeStyleHost->update(chromeInputs());
}

void ContourGuiApp::onConfigurationReloaded()
{
    refreshChromeStyle();
    // Guarded as refreshChromeStyle() is: the notice is built with the QML engine, which a headless
    // app never creates.
    if (_newDefaultsNotice)
        _newDefaultsNotice->refresh();
}

void ContourGuiApp::onAccessibilityPreferencesChanged(config::AccessibilityPreferences preferences)
{
    refreshChromeStyle();
    // The terminal's own animations follow the motion preference too, in every running session.
    sessionsManager().updateMotionPreference(preferences.motion);
}

void ContourGuiApp::onPlatformAppearanceChanged(platform::PlatformAppearance /*appearance*/)
{
    refreshChromeStyle();
}

int ContourGuiApp::checkConfig()
{
    _config = contour::config::loadConfigNamed(parameters().get<string>("contour.terminal.config"));

    contour::config::compareEntries(
        _config, core::log::Category("", "Console Logger", core::log::Category::State::Enabled));

    return EXIT_SUCCESS;
}

bool ContourGuiApp::loadConfig(string const& target)
{
    auto const& flags = parameters();
    auto const prefix = "contour." + target + ".";

    auto configFailures = 0;
    auto const configLogger = [&](string const& msg) {
        cerr << "Configuration failure. " << msg << '\n';
        ++configFailures;
    };

    // `debug` and `log` are synonyms; whichever was given wins, `log` if both.
    auto const logFilter = [&] {
        auto const fromLog = flags.get<string>(prefix + "log");
        return fromLog.empty() ? flags.get<string>(prefix + "debug") : fromLog;
    }();

    auto const logFileSpec = flags.get<string>(prefix + "log-file");
    auto const parsedLogFile = core::log::parseLogFileSpec(logFileSpec);
    if (!parsedLogFile)
        cerr << std::format("contour: cannot use log file '{}': {}\n",
                            core::escape(logFileSpec),
                            parsedLogFile.error().message());
    // The file to log to; none for standard error, which is also where a spec that is not UTF-8 leaves it.
    auto const logFile = parsedLogFile.value_or(std::nullopt);

    // A destination is worth installing for EITHER half: a filter needs somewhere to write, and
    // `--log-file` alone is a request to redirect whatever $LOG already enabled. Gating the whole
    // block on a non-empty filter made `--log-file` a silent no-op -- no file created, and on
    // Windows tryAttachConsole() has already taken `2>` away, so nothing to fall back on either.
    // An empty OutputConfig::filter deliberately leaves the current enablement alone, so passing
    // one through is safe.
    if (!logFilter.empty() || logFile.has_value())
    {
        // ScopedOutput does both halves at once: it enables the named categories AND installs an
        // enabled sink for them, and it lets the destination be a file -- which on Windows is the
        // only way to capture this at all (see the log-file option).
        //
        // NOTE ON THE STREAM. core::log::Sink::console() is constructed disabled (LogStore.cpp:29),
        // but App::run() enables it via customizeLogStoreOutput() before any verb is dispatched,
        // so the bare core::log::configure() this replaced was NOT discarding its output -- it went
        // to std::cout. A ScopedOutput with no file writes to std::cerr, so `--debug`/`--log`
        // output moves from stdout to stderr here. That is deliberate (diagnostics belong on
        // stderr, and it matches the client and daemon verbs, which route through
        // App::installLogging()), and it is stated in the option help so a pipeline that captured
        // stdout has something to find.
        auto output = core::log::ScopedOutput::create({
            .filter = logFilter,
            .file = logFile,
            .showProcessId = false,
        });

        // An unopenable log file must not cost the user their logging as well. create() applies the
        // filter from inside the ScopedOutput constructor, so a failed open means the categories
        // were never enabled either -- warn and continue would boot the GUI completely silent, and
        // silence is the one thing whoever passed --log cannot distinguish from "it never happened".
        // The stderr destination has no fallible step, so this retry cannot fail in turn.
        if (!output)
        {
            cerr << std::format("contour: {}; logging to standard error instead.\n", output.error());
            output = core::log::ScopedOutput::create({
                .filter = logFilter,
                .file = std::nullopt,
                .showProcessId = false,
            });
        }

        if (output)
            _guiLogOutput = std::move(*output);
        else
            cerr << std::format("contour: {}\n", output.error());

        // A pattern matching nothing is nearly always a typo, and its symptom -- no output -- is
        // indistinguishable from "the thing you asked about never happened", which is exactly the
        // wrong signal to give someone reproducing a rendering artifact.
        for (auto const& unmatched: core::log::unmatchedFilters(logFilter))
            cerr << std::format("contour: '{}' matches no known tag (see `contour "
                                "list-debug-tags`).\n",
                                unmatched);
    }

    _config = contour::config::loadConfigNamed(flags.get<string>(prefix + "config"));

    _config.live.value() = _config.live.value() || parameters().boolean("contour.terminal.live-config");

    if (!_config.profile(profileName()))
    {
        auto const s =
            accumulate(begin(_config.profiles.value()),
                       end(_config.profiles.value()),
                       ""s,
                       [](string const& acc, auto const& profile) -> string {
                           return acc.empty() ? profile.first : std::format("{}, {}", acc, profile.first);
                       });
        configLogger(
            std::format("No profile with name '{}' found. Available profiles: {}", profileName(), s));
    }

    if (auto const wd = flags.get<string>("contour.terminal.working-directory"); !wd.empty())
        _config.profile(profileName())->shell.value().workingDirectory =
            core::pathFromUtf8(wd).value_or(fs::path {});

    config::TerminalProfile* profile = _config.profile(profileName());
    if (!profile)
        configLogger("Could not resolve configuration profile.");

    if (configFailures)
        return EXIT_FAILURE;

    // Possibly override shell to be executed
    auto exe = flags.get<string>("contour.terminal.execute");
    if (!flags.verbatim.empty() || !exe.empty())
    {
        auto& shell = profile->shell.value();
        auto const profileShellProgram = shell.program;
        shell.arguments.clear();
        if (!exe.empty())
        {
            shell.program = std::move(exe);
            for (auto i: flags.verbatim)
                shell.arguments.emplace_back(i);
        }
        else
        {
            auto const frontCommand = flags.verbatim.front();
            auto const verbatim = classifyVerbatimCommand(frontCommand, isReachableProgram);

            switch (verbatim.kind)
            {
                case VerbatimCommandKind::Program: shell.program = frontCommand; break;
                case VerbatimCommandKind::WorkingDirectory:
                    shell.workingDirectory = verbatim.workingDirectory;
                    break;
                case VerbatimCommandKind::Unknown:
                    errorLog()("Do not know what to do with `{}` will use it as a program", frontCommand);
                    shell.program = frontCommand;
                    break;
            }

            for (size_t i = 1; i < flags.verbatim.size(); ++i)
                shell.arguments.emplace_back(flags.verbatim.at(i));
        }

        // Record the effective CLI command (the directory-only branches above leave the shell
        // program untouched and carry no command to record) — see cliCommand().
        if (shell.program != profileShellProgram)
        {
            auto command = vtpty::Process::ExecInfo {};
            command.program = shell.program;
            command.arguments = shell.arguments;
            _cliCommand = std::move(command);
        }
    }

    if (auto const wmClass = flags.get<string>("contour.terminal.class"); !wmClass.empty())
    {
        auto* profile = _config.profile(profileName());
        if (profile)
        {
            profile->wmClass = wmClass;
        }
    }

    return true;
}

void ContourGuiApp::registerBundledFonts() const
{
    // Qt-free, so the headless verbs need no QCoreApplication to find the fonts shipped beside them.
    auto const executable = vthost::currentExecutablePath();
    if (!executable)
    {
        errorLog()("Bundled fonts not registered: cannot locate the executable: {}.",
                   executable.error().message());
        return;
    }

    auto const registered = text::registerBundledFonts(bundledFontDirectories(executable->parent_path()));
    if (registered)
        return;
    // A missing directory (a build tree that was never installed) is expected: the configured fonts, or the
    // system's, are used instead. A rejected font file is not.
    switch (registered.error())
    {
        case text::FontRegistrationError::DirectoryMissing:
            startupLog()("Bundled fonts not registered: DirectoryMissing.");
            break;
        case text::FontRegistrationError::RegistrationFailed:
            errorLog()("Bundled fonts not registered: RegistrationFailed.");
            break;
    }
}

int ContourGuiApp::fontConfigAction()
{
    if (!loadConfig("font-locator"))
        return EXIT_FAILURE;

    registerBundledFonts();

    vtrasterizer::FontDescriptions const& fonts =
        _config.profile(_config.defaultProfileName.value())->fonts.value();
    text::FontDescription const& fontDescription = fonts.regular;
    text::FontLocator& fontLocator = createFontLocator(fonts.fontLocator);
    text::FontSourceList const fontSources = fontLocator.locate(fontDescription);

    std::cout << std::format("Matching fonts using  : {}\n", fonts.fontLocator);
    std::cout << std::format("Font description      : {}\n", fontDescription);
    std::cout << std::format("Number of fonts found : {}\n", fontSources.size());
    for (text::FontSource const& fontSource: fontSources)
        std::cout << std::format("  {}\n", fontSource);

    return EXIT_SUCCESS;
}

int ContourGuiApp::terminalGuiAction()
{
    {
        auto const timer = crispy::ScopedTimer(startupLog, "loadConfig");
        if (!loadConfig("terminal"))
            return EXIT_FAILURE;
    }
    _startupTimeline.mark(vthost::latency::StartupMark::ConfigLoaded);
    // The client verb has read its own options before it re-parsed into this verb's surface.
    if (!_latencyProbeRequest)
        _latencyProbeRequest = readLatencyProbeRequest("contour.terminal");

    // Where the sessions live: decided, and an attach started, before anything else boots, so the connect
    // runs beside the boot. Whatever it set up is undone on every return -- after `app` below is gone, as the
    // attach engines' ptys outlive the windows.
    auto const releaseHosting = core::Finally { [this] {
        // A spawn still held back is let go, so that stopping the attach below finds nothing waiting.
        openDaemonSpawnGate();
        _awaitAttach = nullptr;
        _onGuiBooted = nullptr;
        if (_nativeController || _tmuxController)
            _routingFactory->setDelegate(nullptr);
        if (_nativeController)
            _nativeController->stop();
        if (_tmuxController)
            _tmuxController->stop();
    } };
    if (auto const hosting = establishHosting(); !hosting)
    {
        cerr << hosting.error() << '\n';
        return EXIT_FAILURE;
    }

#ifdef __APPLE__
    QGuiApplication::setAttribute(Qt::AA_MacDontSwapCtrlAndMeta, true);
#endif

    // Software rendering must be requested before the QApplication is constructed. The concrete RHI
    // graphics API (OpenGL/Vulkan/Direct3D/Metal/auto) is selected further below via
    // QQuickWindow::setGraphicsApi, once the QApplication exists.
    QGuiApplication::setAttribute(Qt::AA_UseSoftwareOpenGL,
                                  _config.renderer.value().renderingBackend
                                      == config::RenderingBackend::Software);

    auto const* profile = _config.profile(profileName());
    if (!profile)
    {
        errorLog()("Could not access configuration profile.");
        return EXIT_FAILURE;
    }
    auto appName = QString::fromStdString(profile->wmClass.value());
    QCoreApplication::setApplicationName(appName);
    QCoreApplication::setOrganizationName("contour");
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
    // Expose the profile name to the shell.
    _config.profile(profileName())->shell.value().env["CONTOUR_PROFILE"] = profileName();

    vector<string> qtArgsStore;
    vector<char const*> qtArgsPtr;
    qtArgsPtr.push_back(_argv[0]);

    auto const addQtArgIfSet = [&](string const& key, char const* arg) -> bool {
        if (string const& s = parameters().get<string>(key); !s.empty())
        {
            qtArgsPtr.push_back(arg);
            qtArgsStore.push_back(s);
            qtArgsPtr.push_back(qtArgsStore.back().c_str());
            return true;
        }
        return false;
    };

    addQtArgIfSet("contour.terminal.session", "-session");
    if (!addQtArgIfSet("contour.terminal.platform", "-platform"))
    {
        if (!_config.platformPlugin.value().empty())
        {
            static constexpr auto PlatformArg = string_view("-platform");
            qtArgsPtr.push_back(PlatformArg.data());
            qtArgsPtr.push_back(_config.platformPlugin.value().c_str());
        }
    }

#ifdef __linux__
    addQtArgIfSet("contour.terminal.display", "-display");
#endif

    auto qtArgsCount = static_cast<int>(qtArgsPtr.size());

    // The driver stack reads its GPU-selection variables when the display connection opens.
    takeGpuFallbackRequest();
    applyOpenGlGpuSelection();
    // Before the multimedia warm-up below loads the backend, and while this is the only thread.
    keepMultimediaOffTheGpus();

    // NB: We use QApplication over QGuiApplication because we want to use SystemTrayIcon.
    // NOTE: Cannot use ScopedTimer here because QApplication must live until end of function.
    auto const qtInitStart = std::chrono::steady_clock::now();
    QApplication const app(qtArgsCount, (char**) qtArgsPtr.data());
    openDaemonSpawnGate();
    // Every Qt object this app owns goes before QApplication, on every return from here on -- the
    // failed-attach one included, which leaves before the event loop ever runs. Declared right after
    // `app`, so it runs before `app` is destroyed.
    auto const releaseQtObjects = core::Finally { [this] {
        // The QML engine first: its context properties point at the style providers, and the windows
        // the helpers below served are gone with it.
        _qmlEngine.reset();
        // The GPU-selection helpers hold Qt objects (a timer, a D-Bus notifier, a Vulkan instance).
        _latencyProbeDriver.reset();
        _firstFrameWatchdog.reset();
        _gpuNotifier.reset();
#ifdef CONTOUR_WITH_GPU_SELECTION
        _graphicsDeviceSelector.reset();
#endif
    } };
    setupQCoreApplication();

    registerBundledFonts();

    if (startupLog.isEnabled())
    {
        auto const elapsed = std::chrono::steady_clock::now() - qtInitStart;
        auto const ms =
            static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count())
            / 1000.0;
        startupLog()("QApplication + setup: {:.1f} ms", ms);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // Seed the terminal's light/dark preference from the *real* OS color scheme. This is read
    // before applyGuiTheme() may override the application color scheme below, so a force-pinned GUI
    // theme (theme: dark|light) never drags the terminal grid away from the OS preference.
    _colorPreference = QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark
                           ? vtbackend::ColorPreference::Dark
                           : vtbackend::ColorPreference::Light;

    display::displayLog()("Color theme mode at startup: {}", _colorPreference);

    connect(
        QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, [this](Qt::ColorScheme newScheme) {
            // Only the terminal grid tracks the OS here, and only while the GUI theme defers to the
            // system (theme: system, or terminal over a dual color config). When the GUI theme is
            // force-pinned, colorScheme() reflects our own override, which must not move the terminal
            // palette.
            if (_appliedGuiScheme.has_value())
                return;

            auto const newValue = newScheme == Qt::ColorScheme::Dark ? vtbackend::ColorPreference::Dark
                                                                     : vtbackend::ColorPreference::Light;
            if (_colorPreference == newValue)
                return;

            _colorPreference = newValue;
            display::displayLog()("Color preference changed to {} mode\n", _colorPreference);
            sessionsManager().updateColorPreference(_colorPreference);
            // The Terminal row paints with the scheme the new preference selects.
            refreshChromeStyle();
        });
#endif

    // Apply the configured GUI chrome theme (dark/light/system). Must run after the OS scheme has
    // been captured above and before the first QQuickWindow is created, so the chrome opens themed.
    applyGuiTheme(_config.theme.value(), defaultProfileColors());

    // Pin the Fusion Qt Quick Controls style app-wide so the hand-drawn tab strip, its controls, and the
    // tab context menu render, blend, and stay readable on every OS. Qt otherwise picks the native style
    // per platform (e.g. Windows/FluentWinUI3 on Qt 6.7+, whose opaque button chrome and dark-only menu
    // text clash with our custom palette-driven title bar) or the flat Basic style, neither of which
    // gives the menu an opaque, themed, readable surface on our transparent window. NB: set
    // unconditionally — QQuickStyle::name() already resolves to the (non-empty) native default here, so
    // guarding on it silently skips Fusion.
    //
    // `ui_style: terminal` swaps that for ContourTui, our own style (src/contour/styles/ContourTui),
    // which paints a control as terminal chrome: cell-quantized, square-cornered, in the terminal
    // font. Fusion stays the FALLBACK, so a control ContourTui does not implement still gets a
    // readable, themed one rather than the flat Basic default.
    //
    // This is why `ui_style` takes effect on the next start rather than live: the style is resolved
    // once, here, before the first control exists, and Qt offers no way to re-resolve it afterwards.
    _chromeStyle =
        config::resolveChromeStyle(_config.uiStyle.value(), config::detectHostPlatform(processEnvironment()));
    QQuickStyle::setFallbackStyle(QStringLiteral("Fusion"));
    QQuickStyle::setStyle(QString::fromUtf8(config::uiStyleTokens(_chromeStyle).quickControlsStyle));

    // Select the Qt RHI graphics API from the configured renderer backend, before the first
    // QQuickWindow is created. RenderingBackend::Auto leaves the choice to Qt, which resolves the
    // platform-native backend (Direct3D 11 on Windows, Metal on macOS, OpenGL on Linux). The renderer
    // is backend-portable (RhiRenderer uses QRhi::clipSpaceCorrMatrix()/isYUpInFramebuffer(), and the
    // shaders are compiled for every backend), so no GL-specific assumptions leak through here.
    auto const graphicsApiFor =
        [](config::RenderingBackend backend) -> std::optional<QSGRendererInterface::GraphicsApi> {
        switch (backend)
        {
            case config::RenderingBackend::Auto: return std::nullopt;
            case config::RenderingBackend::OpenGL: return QSGRendererInterface::OpenGL;
            case config::RenderingBackend::Software: return QSGRendererInterface::OpenGL;
            case config::RenderingBackend::Vulkan: return QSGRendererInterface::Vulkan;
            case config::RenderingBackend::Direct3D11: return QSGRendererInterface::Direct3D11;
            case config::RenderingBackend::Direct3D12: return QSGRendererInterface::Direct3D12;
            case config::RenderingBackend::Metal: return QSGRendererInterface::Metal;
        }
        return std::nullopt;
    };

    // Resolve the configured backend against what this platform can actually composite (see
    // RenderingBackendSelection.h): an unavailable backend — including desktop OpenGL on macOS, which
    // maps a window but never composites the scene graph, leaving the user with an invisible window —
    // self-heals to Auto so a mis-set config never prevents startup or hangs on a dead window.
    auto const configuredBackend = _config.renderer.value().renderingBackend;
    auto const requestedBackend = resolveRenderingBackend(display::currentRhiPlatform(), configuredBackend);
    if (requestedBackend != configuredBackend)
        errorLog()("Renderer backend {} is not available on this platform; falling back to auto.",
                   configuredBackend);

    if (auto const api = graphicsApiFor(requestedBackend))
        QQuickWindow::setGraphicsApi(*api);

#ifdef CONTOUR_WITH_GPU_SELECTION
    // `auto` does not intervene, so it needs no selector: Qt lists and creates the devices as it always did.
    if (auto const implementation = display::adapterImplementationFor(requestedBackend);
        implementation && effectiveGpuSelector().preference != config::GpuPreference::Auto)
        _graphicsDeviceSelector = std::make_unique<display::GraphicsDeviceSelector>(
            display::makeQtAdapterLister(*implementation), effectiveGpuSelector());
#endif

    QGuiApplication::setWindowIcon(QIcon(":/contour/logo-256.png"));

    QSurfaceFormat::setDefaultFormat(display::createSurfaceFormat(
        _latencyProbeRequest ? _latencyProbeRequest->pacing : display::SwapPacing::VSync));

    // Hands out the accessibility interface for the terminal item, so OS magnifiers and screen readers
    // can follow the caret. Installed here rather than from a static initializer: the ordering against
    // the application object matters, and the factory must be in place before the first item is created.
    display::TerminalAccessible::installFactory();

    ensureTermInfoFile();

    // clang-format off
    qmlRegisterType<display::TerminalDisplay>("Contour.Terminal", 1, 0, "ContourTerminal");
    qmlRegisterUncreatableType<session::TerminalSession>("Contour.Terminal", 1, 0, "TerminalSession", "Use factory.");
    qmlRegisterUncreatableType<session::TerminalSessionManager>("Contour.Terminal", 1, 0, "TerminalSessionManager", "Do not use me directly.");
    qmlRegisterUncreatableType<session::PaneProxy>("Contour.Terminal", 1, 0, "PaneProxy", "Created by the session manager.");
    qmlRegisterUncreatableType<window::WindowController>("Contour.Terminal", 1, 0, "WindowController", "Created by the session manager.");
    qmlRegisterUncreatableType<window::CommandPaletteModel>("Contour.Terminal", 1, 0, "CommandPaletteModel", "Created by the window controller.");
    qmlRegisterUncreatableType<window::SettingsController>("Contour.Terminal", 1, 0, "SettingsController", "Created by the window controller.");
    qRegisterMetaType<session::TerminalSession*>("TerminalSession*");
    qRegisterMetaType<session::PaneProxy*>("PaneProxy*");
    qRegisterMetaType<window::WindowController*>("WindowController*");
    qRegisterMetaType<window::CommandPaletteModel*>("CommandPaletteModel*");
    qRegisterMetaType<window::SettingsController*>("SettingsController*");
    // clang-format on

    // Constructed with the resolved style rather than default-constructed and then applied: a
    // constructed object is a usable object, and the first frame must already draw the right
    // controls. applyWindowControlStyle() below exists for the reloads that follow.
    _windowControlStyleProvider = make_unique<window::WindowControlStyleProvider>(
        resolvedWindowControlStyle(_config.windowControlStyle.value()));

    {
        auto const timer = crispy::ScopedTimer(startupLog, "QML engine setup");
        _qmlEngine = make_unique<QQmlApplicationEngine>();

        // Keep the override seam resolveResource() gave the QML back when it shipped as loose qrc
        // files, now expressed the way a QML module is overridden: the config directory becomes an
        // import path, so a `Contour/Ui` module placed there (its own qmldir plus the components to
        // replace) shadows the compiled-in one.
        //
        // Only this path, not the source tree resolveResource() also probed under !NDEBUG: an import
        // path is searched for <path>/Contour/Ui/qmldir, and src/contour/qml is a flat directory of
        // components with no module layout, so pointing at it would silently never resolve. The
        // developer loop is a rebuild, which is what qmlcachegen wants anyway.
        //
        // The seam moved, so anyone still using the old one is told rather than left wondering why
        // their customization stopped applying.
        auto const configHome = config::configHome();
        if (hasStrandedQmlOverrides(configHome))
            errorLog()("Ignoring the QML overrides in '{}': the interface is loaded as a QML module "
                       "now. Move them into '{}', alongside a qmldir naming each component, to have "
                       "them apply again.",
                       core::platform::normalizePath(configHome / "ui"),
                       core::platform::normalizePath(uiModuleOverrideDirectory(configHome)));
        _qmlEngine->addImportPath(platform::toQString(configHome));

        QQmlContext* context = _qmlEngine->rootContext();
        context->setContextProperty("terminalSessions", &_sessionManager);
        context->setContextProperty("daemonDrift", &_daemonDrift);
        // The new-defaults banner's decision, over the files beside contour.yml: "Use New" writes
        // settings.yml and reloads, "Keep Mine" remembers the answer in the GUI state file.
        auto const configDir = _config.configFile.parent_path();
        _newDefaultsNotice =
            make_unique<window::NewDefaultsNotice>([this]() -> config::Config const& { return _config; },
                                                   std::make_shared<config::FileGuiConfigStore>(configDir),
                                                   std::make_shared<config::FileGuiStateStore>(configDir),
                                                   [this] { return _sessionManager.reloadAllSessions(); });
        context->setContextProperty("newDefaults", _newDefaultsNotice.get());
        // A context property rather than a registered QML singleton: two separate modules read these
        // tokens -- Contour.Ui and the ContourTui Quick Controls style -- and a singleton would make
        // the style module import this application's own C++ URI, which a style that Qt resolves by
        // name must not do. It also gives each test engine its own style (see test/QmlChromeStyle.h).
        // See UiStyleProvider.h for the price this trade pays.
        //
        // An injected instance rather than a QML-created singleton, because it is configured entirely
        // at construction (see AGENT.md's complete-constructor rule) and its configuration is this
        // app's, not the engine's. The host republishes a new provider when an input changes.
        _chromeStyleHost = make_unique<window::ChromeStyleHost>(*context, chromeInputs());
        // A second provider beside chromeStyle rather than more properties on it: this one changes
        // live and chromeStyle's are CONSTANT by contract. See WindowControlStyleProvider.hpp.
        context->setContextProperty("windowControls", _windowControlStyleProvider.get());
        // The popups' drop shadow, as a nine-patch image. An image provider rather than more chrome
        // tokens because what QML needs here is a TEXTURE: a real Gaussian cannot be assembled from
        // QML primitives without banding, and the engine takes ownership of this.
        _qmlEngine->addImageProvider(QStringLiteral("popupshadow"), new window::PopupShadowImageProvider());
    }
    _startupTimeline.mark(vthost::latency::StartupMark::QmlEngineReady);

    // Attach mode's connect has run beside everything above; the first window needs it done, and decides
    // whether there is a daemon to show at all. A `required` one that is not there ends the launch here,
    // before any window exists.
    if (_awaitAttach)
    {
        if (auto const settled = settleHosting(std::exchange(_awaitAttach, nullptr)()); !settled)
        {
            cerr << settled.error() << '\n';
            return EXIT_FAILURE;
        }
    }

    // auto const HTS = "\033H";
    // auto const TBC = "\033[g";
    // printf("\r%s        %s                        %s\r", TBC, HTS, HTS);

    // Pre-warm the Qt multimedia singleton on a background thread.
    // This triggers the expensive FFmpeg/VDPAU/VA-API/Vulkan driver probing
    // without blocking the main thread. When done, we signal QML via
    // multimediaReady so the bell Loader activates only after the probe completes.
    //
    // Skipped entirely when no profile can ever ring an audible bell -- and that is a CRASH fix, not
    // a speed one. The probe brings up a PipeWire client and its monitor threads, and those race with
    // Qt Multimedia's teardown when the process exits before they have settled. It surfaces as a
    // SIGSEGV inside PipeWire's own protocol thread rather than anywhere in Contour, which is why the
    // event-pumping join below -- which correctly fixes the *deadlock* -- does not fix this.
    //
    // Measured against a five-second session (`contour <config> ucs-detect`), which is short enough
    // to lose the race reliably: 3 crashes in 7 runs with the probe running, 0 in 7 without it.
    //
    // Profiles are checked rather than just the default one, because a profile can be switched to at
    // runtime. A profile that DOES have an audible bell still runs the probe and can still lose the
    // race; that part is an upstream Qt Multimedia/PipeWire teardown issue, and is far less likely in
    // a session that lives longer than its own startup.
    auto const anyProfileHasAudibleBell =
        std::ranges::any_of(_config.profiles.value(),
                            [](auto const& profile) { return profile.second.bell.value().sound != "off"; });

    auto multimediaWarmedUp = std::atomic<bool> { false };
    auto multimediaWarmupThread = std::thread([this, &multimediaWarmedUp, anyProfileHasAudibleBell] {
        if (anyProfileHasAudibleBell)
            QMediaDevices::audioOutputs();
        platform::postToObject(&_sessionManager, [this] { _sessionManager.setMultimediaReady(true); });
        multimediaWarmedUp.store(true, std::memory_order_release);
    });

    // Spawn initial window.
    {
        auto const timer = crispy::ScopedTimer(startupLog, "newWindow (QML load)");
        newWindow();
    }
    _startupTimeline.mark(vthost::latency::StartupMark::WindowCreated);
    // The window that runs its shells itself, as the daemon could not be had, says so.
    if (!_hostingNotice.empty())
        if (auto const window = _sessionManager.focusedWindow())
            if (auto* controller = _sessionManager.controllerFor(*window))
                controller->showHostingNotice(QString::fromStdString(_hostingNotice));

    // Attach mode adopts the remaining remote sessions as tabs now that a
    // window exists to put them in.
    if (_onGuiBooted)
        _onGuiBooted();

    if (_latencyProbeRequest)
        armLatencyProbe();

    // Run the event loop FIRST (it populates _exitStatus via onExit during the run), THEN map that
    // status to the process exit code. The mapping is the pure exitCodeFor() (ExitCode.h), extracted
    // so it is unit-testable without the event loop. NB: sequenced explicitly — passing exec() as an
    // argument alongside _exitStatus would read _exitStatus before the loop populated it.
    auto const loopResult = QApplication::exec();
    // A finished probe has the last word: the echo child it ends exits 0 whether or not the probe succeeded.
    auto const rv = _latencyProbeExitCode.value_or(session::exitCodeFor(_exitStatus, loopResult));

    // Ensure the multimedia warmup thread has finished before destroying Qt objects -- but keep serving
    // this thread's event queue while waiting, rather than blocking straight into join().
    //
    // QMediaDevices::audioOutputs() is not self-contained on a worker thread: underneath, Qt's
    // QPlatformAudioDevices::create() posts a *blocking* QMetaObject::invokeMethod back to the main
    // thread and waits on it. So once exec() has returned, a bare join() deadlocks by construction --
    // the warmup thread waits for an event loop that is gone, while the main thread waits for the warmup
    // thread. That is not hypothetical: it is reachable whenever the app quits before the probe finishes
    // (`contour execute sh -c "exit 0"` is the extreme case), and in practice the process did not even
    // hang to be noticed -- PipeWire's own loop thread crashed on the half-built device monitor first.
    //
    // Pumping until the probe reports done gives the blocking invoke someone to answer it, so the probe
    // completes and the join is immediate. When the probe wins the race (the normal case) the flag is
    // already set and this degenerates to the plain join it was before.
    while (!multimediaWarmedUp.load(std::memory_order_acquire))
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    if (multimediaWarmupThread.joinable())
        multimediaWarmupThread.join();

    // printf("\r%s", TBC);
    return rv; // releaseQtObjects tears the Qt objects down before `app`
}

void ContourGuiApp::setupQCoreApplication()
{
    auto const* profile = _config.profile(profileName());
    Require(profile);

    auto const defaultAppName = QStringLiteral(CONTOUR_APP_ID);
    auto const defaultOrgDomain = QStringLiteral("contour-terminal.org");
    auto const defaultOrgName = QStringLiteral("contour");

    auto const platformName = QGuiApplication::platformName();
    auto const wmClass = QString::fromStdString(profile->wmClass.value());

    auto const effectiveAppName = [&]() -> QString const& {
        // On X11, we want to set the WM_CLASS property to the configured value.
        if (platformName == "xcb" && !wmClass.isEmpty())
            return wmClass;
        return defaultAppName;
    }();

    // On Wayland, we want to set the Application Id to the configured value via the desktop file name.
    // We use the desktop file name as the application id, because that's what Qt uses to set the
    // app id on Wayland.
    // I know this sounds weird. This is because it is weird. But it's the only way to set the
    // application id on Wayland when using Qt.
    if (platformName == "wayland" && !wmClass.isEmpty())
        QGuiApplication::setDesktopFileName(wmClass);
    else
        QGuiApplication::setDesktopFileName(defaultAppName);

    QCoreApplication::setOrganizationDomain(defaultOrgDomain);
    QCoreApplication::setOrganizationName(defaultOrgName);
    QCoreApplication::setApplicationName(effectiveAppName);
    QCoreApplication::setApplicationVersion(CONTOUR_VERSION_STRING);
}

void ContourGuiApp::ensureTermInfoFile()
{
    if (!vtpty::Process::isFlatpak())
        return;

    auto const hostTerminfoBaseDirectory =
        Process::homeDirectory() / ".var/app/org.contourterminal.Contour/terminfo/c";
    if (!fs::is_directory(hostTerminfoBaseDirectory))
        fs::create_directories(hostTerminfoBaseDirectory);

    auto const sandboxTerminfoFile = fs::path("/app/share/terminfo/c/contour");
    if (!fs::is_regular_file(hostTerminfoBaseDirectory / "contour"))
        fs::copy_file(sandboxTerminfoFile, hostTerminfoBaseDirectory / "contour");
}

void ContourGuiApp::newWindow(QScreen* targetScreen)
{
    _pendingSpawnScreen = targetScreen;
    // The engine is always up when one window spawns another (the GUI is running); the guard makes
    // the stage/consume contract exercisable headlessly, where no QML engine exists.
    if (_qmlEngine)
        _qmlEngine->loadFromModule("Contour.Ui", "Main");
}

QScreen* ContourGuiApp::takePendingSpawnScreen() noexcept
{
    auto* screen = _pendingSpawnScreen.data();
    _pendingSpawnScreen.clear();
    return screen;
}

bool ContourGuiApp::requestRemoteWindow(session::RemoteSpawn const& spawn)
{
    return _routingFactory != nullptr && _routingFactory->requestRemoteWindow(spawn);
}

void ContourGuiApp::reconcileAttachWindows()
{
    if (!_nativeController)
        return;

    // Daemon window ids come ascending, so the primary (lowest-id) window is handled
    // first — it maps to the boot window; the rest each get their own OS window.
    for (auto const daemonWindow: _nativeController->windowIds())
    {
        if (auto const mapped = _attachWindowMap.find(daemonWindow); mapped != _attachWindowMap.end())
        {
            // Already shown: bring its tree up to date.
            contour::remote::applyRemoteLayout(
                _sessionManager, mapped->second, *_nativeController, daemonWindow);
            continue;
        }
        if (_pendingAttachWindow == daemonWindow)
            continue; // an OS window is already spawning for this daemon window

        if (_attachBoot == AttachBoot::Pending)
        {
            // The primary daemon window adopts the boot window (already created before
            // the first layout arrived). A missing focused window (mid-boot) stages
            // the daemon window so bindPendingAttachWindow can bind it once the boot
            // window becomes ready, instead of hoping for another layout push.
            if (auto const boot = _sessionManager.focusedWindow())
                bindDaemonWindow(daemonWindow, *boot);
            else if (!_pendingAttachWindow)
                _pendingAttachWindow = daemonWindow;
            continue;
        }

        // A daemon window holding no tab is nothing to show yet -- one a reconnect found emptied, under a
        // client that follows every window -- and gets its OS window once it holds one.
        if (auto const known = _nativeController->layout(daemonWindow); !known || known->tabs.empty())
            continue;

        // A new daemon window: spawn an OS window to host it. Its Main.qml claims the
        // staged id (consumeAttachWindow → bindPendingAttachWindow), records the mapping
        // and reconciles — so it never creates a stray fresh tab. The QML load is
        // synchronous, so the stage is consumed before this returns.
        _pendingAttachWindow = daemonWindow;
        newWindow();
    }
}

std::optional<std::uint64_t> primaryDaemonWindowToAdopt(bool anyWindowMapped,
                                                        std::vector<std::uint64_t> const& daemonWindowIds)
{
    if (anyWindowMapped || daemonWindowIds.empty())
        return std::nullopt;
    return daemonWindowIds.front(); // windowIds() is ascending, so front() is the primary window
}

bool ContourGuiApp::bindPendingAttachWindow(window::WindowController* controller)
{
    if (!_nativeController || controller == nullptr)
        return false;

    // A reconcile-spawned secondary OS window claims the daemon window staged for it.
    if (_pendingAttachWindow)
    {
        auto const daemonWindow = *_pendingAttachWindow;
        _pendingAttachWindow.reset();
        bindDaemonWindow(daemonWindow, controller->windowId());
        return true;
    }

    // The boot (first) OS window in attach mode ADOPTS the daemon's primary window instead of
    // authoring a fresh tab. Claiming it here makes Main.qml skip win.createNewTab(), which in
    // attach mode would author a spurious extra tab on the daemon (and create no local first tab).
    // Bind the primary window now if the daemon already reported one; otherwise still claim the boot
    // window (return true) and let reconcileAttachWindows bind it once the first layout arrives.
    if (_attachBoot == AttachBoot::Pending)
    {
        if (auto const adopt = primaryDaemonWindowToAdopt(false, _nativeController->windowIds()))
            bindDaemonWindow(*adopt, controller->windowId());
        return true;
    }

    return false;
}

void ContourGuiApp::connectReconnectSignals()
{
    // Every pane says it is reconnecting while the connection is sought again, rather than showing a
    // grid that merely stopped.
    connect(
        _nativeController.get(),
        &remote::NativeController::connectionStateChanged,
        this,
        [this] {
            if (_nativeController)
                _sessionManager.setHostConnection(_nativeController->connectionState());
        },
        Qt::QueuedConnection);
    connect(
        _nativeController.get(),
        &remote::NativeController::windowsLost,
        this,
        [this](std::vector<std::uint64_t> const& daemonWindows) { closeLostAttachWindows(daemonWindows); },
        Qt::QueuedConnection);
}

void ContourGuiApp::closeLostAttachWindows(std::vector<std::uint64_t> const& daemonWindows)
{
    if (!_nativeController)
        return;
    auto doomed = std::vector<window::WindowController*> {};
    for (auto const daemonWindow: daemonWindows)
    {
        auto const mapped = _attachWindowMap.find(daemonWindow);
        if (mapped == _attachWindowMap.end())
            continue;
        // Before the close, which closes their ptys: their shells did not exit, the connection to them went.
        // @see TerminalSession::noteTransportLost
        if (auto* window = _sessionManager.model().window(mapped->second))
            for (auto const index: std::views::iota(0, window->tabCount()))
                for (auto* session: _sessionManager.sessionsOfTab(window->tabAt(index)))
                    session->noteTransportLost();
        if (auto* controller = _sessionManager.controllerFor(mapped->second))
            doomed.push_back(controller);
        _attachWindowMap.erase(mapped);
    }
    // The windows that came instead first: closing the last stale window before would end the application.
    reconcileAttachWindows();
    for (auto* controller: doomed)
        controller->closeWindow();
}

void ContourGuiApp::bindDaemonWindow(std::uint64_t daemonWindow, vtworkspace::WindowId osWindow)
{
    _attachBoot = AttachBoot::Adopted;
    _attachWindowMap.emplace(daemonWindow, osWindow);
    contour::remote::applyRemoteLayout(_sessionManager, osWindow, *_nativeController, daemonWindow);
}

display::ForcedFontDpiProvider* ContourGuiApp::forcedFontDpiProvider()
{
    // Lazy: the provider inspects QGuiApplication::platformName(), so it must not be constructed before
    // the Qt application. First use is a display's setSession(), which is well past that point.
    if (!_forcedFontDpiProvider && QCoreApplication::instance() != nullptr)
        _forcedFontDpiProvider = std::make_unique<display::ForcedFontDpiProvider>();
    return _forcedFontDpiProvider.get();
}

} // namespace contour
