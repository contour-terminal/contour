// SPDX-License-Identifier: Apache-2.0
#include <contour/ContourGuiApp.hpp>
#include <contour/Logging.hpp>
#include <contour/askpass/AskpassPinPrompt.hpp>
#include <contour/cli/BackgroundExecutor.hpp>
#include <contour/cli/ClientAttach.hpp>
#include <contour/cli/ConnectList.hpp>
#include <contour/cli/ConnectProbe.hpp>
#include <contour/cli/DaemonDiscovery.hpp>
#include <contour/cli/DaemonVerbs.hpp>
#include <contour/cli/Hosting.hpp>
#include <contour/cli/NewDaemon.hpp>
#include <contour/cli/WorkspaceListing.hpp>
#include <contour/command/LauncherEntries.hpp>
#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/ChromeTokenRows.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/ControlsStyle.hpp>
#include <contour/config/GuiConfigStore.hpp>
#include <contour/config/GuiStateStore.hpp>
#include <contour/config/LaunchTarget.hpp>
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
#include <contour/display/ShortcutOverride.hpp>
#include <contour/display/TerminalAccessible.hpp>
#include <contour/display/TerminalDisplay.hpp>
#include <contour/platform/AccessibilityPreferenceSource.hpp>
#include <contour/platform/ControlsStyleProbe.hpp>
#include <contour/platform/FontFamilySource.hpp>
#include <contour/platform/GuiTheme.hpp>
#include <contour/platform/LauncherMenu.hpp>
#include <contour/platform/MenuBarIntegration.hpp>
#include <contour/platform/MultimediaEnvironment.hpp>
#include <contour/platform/NativeMenuPresenter.hpp>
#include <contour/platform/Notifier.hpp>
#include <contour/platform/PlatformAppearance.hpp>
#include <contour/platform/QtInvoke.hpp>
#include <contour/platform/QtPath.hpp>
#include <contour/platform/SecureInput.hpp>
#include <contour/remote/DaemonLink.hpp>
#include <contour/remote/DaemonSwitchDirectory.hpp>
#include <contour/remote/NativeController.hpp>
#include <contour/remote/RemoteLayout.hpp>
#include <contour/remote/TmuxController.hpp>
#include <contour/remote/WindowSwitcher.hpp>
#include <contour/session/PaneProxy.hpp>
#include <contour/session/Permission.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/window/AppMenuBar.hpp>
#include <contour/window/ChromeStyleHost.hpp>
#include <contour/window/CommandPaletteModel.hpp>
#include <contour/window/ConnectPickerController.hpp>
#include <contour/window/HotkeyRefusals.hpp>
#include <contour/window/NewDefaultsNotice.hpp>
#include <contour/window/NoticeModel.hpp>
#include <contour/window/PopupShadowImageProvider.hpp>
#include <contour/window/QtSaveScheduler.hpp>
#include <contour/window/QuickTerminalController.hpp>
#include <contour/window/SettingsController.hpp>
#include <contour/window/SettingsPreview.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/SettingsWindowRules.hpp>
#include <contour/window/SwitcherController.hpp>
#include <contour/window/TabBarHitValue.hpp>
#include <contour/window/WindowControlStyleProvider.hpp>
#include <contour/window/WindowController.hpp>
#include <contour/window/WindowLivenessQt.hpp>

#include <vtbackend/vt/DesktopNotification.hpp>

#include <vtpty/EnvironmentBlock.hpp>
#include <vtpty/ExecutablePath.hpp>
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
#include <text_shaper/NerdFontSymbolRanges.hpp>
#include <text_shaper/NerdFontSymbolsLocator.hpp>

#include <crispy/ScopedTimer.hpp>

#include <core/Random.hpp>
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
#include <vthost/DaemonSpawn.hpp>
#include <vthost/DaemonStarter.hpp>
#include <vthost/KnownDaemons.hpp>
#include <vthost/SocketPath.hpp>
#include <vthost/StatePath.hpp>
#include <vthost/StdioChild.hpp>
#include <vthost/Token.hpp>
#include <vthost/Transport.hpp>
#include <vthost/client/ControlClient.hpp>
#include <vthost/client/DaemonControlClient.hpp>
#include <vthost/client/DaemonRestart.hpp>
#include <vthost/client/DaemonUpgrade.hpp>
#include <vthost/client/KeySource.hpp>
#include <vthost/client/ReleaseDrift.hpp>
#include <vthost/latency/Report.hpp>
#if !defined(__APPLE__) && !defined(_WIN32)
    #include <QtDBus/QDBusConnection>
#endif
#include <QtCore/QtPlugin>
#include <QtDBus/QtDBus>
#include <QtGui/QCursor>
#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>
#include <QtGui/QStyleHints>
#include <QtGui/QSurfaceFormat>
#include <QtGui/QWindow>
#include <QtGui/rhi/qrhi.h>
#include <QtMultimedia/QMediaDevices>
#include <QtQml/QQmlApplicationEngine>
#include <QtQml/QQmlComponent>
#include <QtQml/QQmlContext>
#include <QtQuick/QQuickGraphicsDevice>
#include <QtQuick/QQuickWindow>
#include <QtQuick/QSGRendererInterface>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

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

    /// How long a window switch waits for the target daemon to list its workspaces: an ssh connect's worth.
    constexpr auto SwitchListingTimeout = std::chrono::milliseconds { std::chrono::seconds { 15 } };

    /// How long "New daemon…" waits for the daemon it started to serve, as an on-demand start does.
    constexpr auto NamedDaemonStartTimeout = std::chrono::seconds { 5 };

    /// What a missing sleep/wake monitor costs, said where it is logged.
    constexpr auto NoPowerMonitorNote = std::string_view {
        "on macOS a sleep is still noticed, by the late liveness tick after it; on Linux and the BSDs the "
        "connection's clock stops during a suspend, so nothing is noticed and nothing is lost."
    };

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
        if (!window::anyWindowKeepsAppAlive(window::windowFactsOf(QGuiApplication::topLevelWindows())))
            QCoreApplication::quit();
    }

    /// @return This process's environment as it stands now, as "NAME=VALUE" entries.
    [[nodiscard]] std::vector<std::string> currentEnvironmentEntries()
    {
        auto entries = std::vector<std::string> {};
        for (auto const& entry: QProcessEnvironment::systemEnvironment().toStringList())
            entries.push_back(entry.toStdString());
        return entries;
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
        /// @param daemonEnvironment What a daemon spawned on demand, and every service command (systemctl,
        ///        launchctl), runs under: both run on the attach's worker. @see _daemonEnvironment
        LocalDaemonStart(core::Environment const& env,
                         core::platform::FileSystem const& fileSystem,
                         std::string const& programPath,
                         std::vector<std::string> const& daemonEnvironment):
            binary { vthost::makeOwnDaemonBinary(env, fileSystem, CONTOUR_VERSION_STRING, programPath) },
            spawner { vthost::makeNativeDaemonSpawner(daemonEnvironment) },
            commands { vthost::makeNativeCommandRunner(daemonEnvironment) },
            services { .fileSystem = core::platform::NativeFileSystem::instance(),
                       .commands = *commands,
                       .environment = env }
        {
        }

        /// Points the daemon task at the installed executable if an upgrade removed its program. Called while
        /// a daemon already serves, when no starter is made to do it.
        /// @param label The socket label.
        void repairRegistration(std::string_view label) const
        {
            std::ignore =
                vthost::findRepairedRegisteredService(label,
                                                      vthost::nativeServiceBackendFactory(services),
                                                      contour::cli::nativeRepairInputs(services.fileSystem),
                                                      vthost::RegistrationUse::Manage);
        }

        /// @param label The socket label.
        /// @param endpoint Where the daemon listens.
        /// @param placement Places a daemon spawned on demand.
        /// @param options What a spawned daemon is told.
        /// @return A registered service when there is one that serves @p endpoint, spawning on demand
        ///         otherwise (an endpoint from `$CONTOUR_MUX` or `--socket` is not the service's). Valid
        ///         while this object lives.
        [[nodiscard]] std::unique_ptr<vthost::DaemonStarter> starter(
            std::string_view label,
            vthost::UnixEndpoint const& endpoint,
            vtpty::ProcessPlacement& placement,
            vthost::DaemonSpawnOptions const& options) const
        {
            return vthost::chooseDaemonStarter(
                vthost::findRepairedRegisteredService(label,
                                                      vthost::nativeServiceBackendFactory(services),
                                                      contour::cli::nativeRepairInputs(services.fileSystem),
                                                      vthost::RegistrationUse::Start),
                vthost::labelSocketPath(label, services.environment),
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

VerbatimCommand classifyVerbatimCommand(std::string_view token,
                                        ProgramReachablePredicate isReachable,
                                        vtpty::ProgramHost host)
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
    // question here, or the documented way to run a command is reported as a mistake (#2102). On
    // the sandbox's host it is flatpak-spawn that searches, in a PATH this process cannot see -- for a
    // bare name only: a path that is not on disk here is no more a program than it is outside Flatpak.
    auto const resolvedOnSandboxHost = host == vtpty::ProgramHost::SandboxHost && !path.has_parent_path();
    if (resolvedOnSandboxHost || isReachable(token))
        return VerbatimCommand { .kind = VerbatimCommandKind::Program, .workingDirectory = {} };

    return VerbatimCommand { .kind = VerbatimCommandKind::Unknown, .workingDirectory = {} };
}

namespace
{
    /// The default way the app ends its event loop.
    void exitQtEventLoop(int status)
    {
        QCoreApplication::exit(status);
    }
} // namespace

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
                             core::platform::SteadyTimePoint processStart,
                             window::SettingsWindowFactory settingsWindowFactory,
                             ShellIntegrationServices shellIntegration,
                             std::unique_ptr<window::DragEndProbe> dragEndProbe,
                             std::unique_ptr<platform::ApplicationRestart> applicationRestart,
                             std::unique_ptr<remote::SwitchingService> windowSwitching,
                             std::unique_ptr<session::SwitchDirectory> switchDirectory,
                             std::function<void(int)> exitEventLoop,
                             window::BootstrapPromptFactory bootstrapPromptFactory,
                             std::unique_ptr<remote::IWorkspaceLister> startupLister):
    ContourApp { env },
    _startupTimeline { core::platform::defaultSteadyClock(), processStart },
    _exitEventLoop { exitEventLoop ? std::move(exitEventLoop)
                                   : std::function<void(int)> { exitQtEventLoop } },
    _bootstrapPromptFactory { std::move(bootstrapPromptFactory) },
    _sshSearchPath { vtpty::execSearchPath(env.get("PATH")) },
    _commandCapture { vtpty::SystemCommandCapture::defaultShell(env), _commandShutdown.get_token() },
    _commandExecutor(cli::makeBackgroundExecutor(StatusLineCommandThreads)),
    _processPlacement(processPlacement ? std::move(processPlacement) : vtpty::makeDefaultProcessPlacement()),
    _gitFileSystem(fileSystem ? *fileSystem : core::platform::NativeFileSystem::instance()),
    _linkProbeExecutor(cli::makeBackgroundExecutor(LinkProbeThreads)),
    _injectedLinkServices { std::move(linkServices) },
    _sessionFactory(sessionFactory ? std::move(sessionFactory)
                                   : std::make_unique<session::AppSessionFactory>(*this, _processPlacement)),
    _externalLauncher(externalLauncher ? std::move(externalLauncher)
                                       : platform::makeExternalLauncher(_processPlacement)),
    _applicationRestart(applicationRestart ? std::move(applicationRestart)
                                           : platform::makeApplicationRestart()),
    _layoutStore(layoutStore ? std::move(layoutStore) : std::make_unique<config::FileLayoutStore>()),
    _commandHistoryStore(commandHistoryStore ? std::move(commandHistoryStore)
                                             : std::make_unique<command::FileCommandHistoryStore>()),
    _windowPositionStore(windowPositionStore ? std::move(windowPositionStore)
                                             : std::make_unique<window::FileWindowPositionStore>()),
    _speechSynthesizer(std::move(speechSynthesizer)),
    _commandOutputFiles(commandOutputFiles ? std::move(commandOutputFiles)
                                           : makeCommandOutputFiles(env, _processLiveness)),
    _terminalClocks(terminalClocks),
    // Read once, for every session, rather than by each one on the GUI thread as it is built.
    _hostIdentity(hostIdentity ? std::move(*hostIdentity) : thisMachineIdentity()),
    _fileSystem(fileSystem ? *fileSystem : core::platform::NativeFileSystem::instance()),
    // In the config directory, as ssh keeps known_hosts: never under the state directory, which falls back
    // to a shared temporary directory that another local user could have planted a pin in.
    _knownDaemons(_fileSystem, vthost::knownDaemonsPath(config::configHome()), core::systemRandom()),
    _startupLister(std::move(startupLister)),
    _taskbarProgressSink(taskbarProgress ? std::move(taskbarProgress) : platform::makeTaskbarProgressSink()),
    _titleBandMaterial(appearanceServices.titleBand()),
    _secureInput(appearanceServices.secureInput()),
    _userNotificationCenter(appearanceServices.userNotifications),
    _captionButtons(appearanceServices.caption()),
    _framePlatform(appearanceServices.framePlatform),
    _dragEndProbe(std::move(dragEndProbe)),
    // The real switcher unless a test injected its double. Built on the manager not yet constructed, which it
    // does not use until a request: the cyclic wiring of the manager and what it dispatches to.
    _windowSwitching(windowSwitching
                         ? std::move(windowSwitching)
                         : std::make_unique<remote::WindowSwitcher>(
                               _sessionManager,
                               *this,
                               remote::makeControlWorkspaceLister(SwitchListingTimeout),
                               // A pick is the switcher popup, scoped to the daemon; it re-enters the switch.
                               [this](remote::SwitchPickRequest const& pick) { return showSwitchPick(pick); },
                               _switchDeadline)),
    _switchDirectory(switchDirectory ? std::move(switchDirectory) : makeSwitchDirectory()),
    // The one read of the home directory: QDir's form (`C:/Users/...` on Windows) is the one the tab
    // tooltip's paths arrive in, which the environment's native separators would not match.
    _sessionManager(*this,
                    *_sessionFactory,
                    *_layoutStore,
                    *_commandHistoryStore,
                    QDir::homePath().toStdString(),
                    *_taskbarProgressSink,
                    *_secureInput,
                    *_windowSwitching,
                    *_switchDirectory),
    _gpuInventory { display::makePlatformGpuInventory() },
    _hostPlatform(appearanceServices.hostPlatform.value_or(config::detectHostPlatform(env))),
    _shellHostPlatform(shellIntegration.host.value_or(_hostPlatform)),
    _chromeStyle(config::resolveChromeStyle(_config.uiStyle.value(), _hostPlatform)),
    _guiState(std::move(shellIntegration.state)),
    _settingsWindowFactory { settingsWindowFactory ? std::move(settingsWindowFactory)
                                                   : engineSettingsWindowFactory() },
    _makeAccessibilitySource { [this, build = appearanceServices.accessibility] {
        return build([this](config::AccessibilityPreferences preferences) {
            onAccessibilityPreferencesChanged(preferences);
        });
    } },
    _makePlatformAppearance { [this, build = appearanceServices.appearance] {
        return build([this](platform::PlatformAppearance const& appearance) {
            onPlatformAppearanceChanged(appearance);
        });
    } },
    _nativeMenus(appearanceServices.menus()),
    _menuBarPlacement(command::menuBarPlacementFor(_hostPlatform)),
    _menuBarIntegration(appearanceServices.menuBar()),
    _knownFolders(platform::makePlatformKnownFolders()),
    _registry(platform::makePlatformRegistry()),
    _executionPolicy(platform::makePlatformExecutionPolicySource(*_registry, _fileSystem, *_knownFolders)),
    _shellRcEditor(shellIntegration.editor
                       ? std::move(shellIntegration.editor)
                       : std::make_unique<platform::FileShellRcEditor>(_fileSystem,
                                                                       env,
                                                                       *_knownFolders,
                                                                       *_executionPolicy,
                                                                       _shellHostPlatform,
                                                                       platform::currentRunningAs())),
    _contourBinary(std::move(shellIntegration.contourBinary)),
    _binaryEnvironment(window::binaryEnvironmentOf(env)),
    _globalHotkey(appearanceServices.globalHotkey()),
    _quickPanel(appearanceServices.quickPanel())
{
    // One filesystem for the paths a session shows, opens and detects (Terminal's contract): injected link
    // services carry the app's.
    Require(!_injectedLinkServices || &_injectedLinkServices->fileSystem == &_fileSystem);
    link("contour.terminal", bind(&ContourGuiApp::terminalGuiAction, this));
    link("contour.font-locator", bind(&ContourGuiApp::fontConfigAction, this));
    link("contour.info.config", bind(&ContourGuiApp::checkConfig, this));
    link("contour.client", bind(&ContourGuiApp::clientAction, this));
    link("contour.connect", bind(&ContourGuiApp::connectAction, this));
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

input::KeyboardLayout const& ContourGuiApp::keyboardLayout() const
{
    // The layout reads the keymap through the running Qt application, so asking before it exists would
    // latch the fallback for the life of the process.
    Require(QGuiApplication::instance() != nullptr);
    std::call_once(_keyboardLayoutOnce, [this] { _keyboardLayout = input::makePlatformKeyboardLayout(); });
    return *_keyboardLayout;
}

window::LocatedBinary ContourGuiApp::locatedContourBinary() const
{
    if (!_contourBinary.empty())
        return { .path = _contourBinary, .reach = window::BinaryReach::Reachable };
    return window::locateBinary(
        _binaryEnvironment,
        std::filesystem::path { QCoreApplication::applicationFilePath().toStdU16String() });
}

std::optional<vtbackend::GitHeadServices> ContourGuiApp::gitHeadServices() noexcept
{
    // NativeGitFileSystem probes this machine's disk: a test's injected files are not to be paired with it.
    if (&_fileSystem != &core::platform::NativeFileSystem::instance())
        return std::nullopt;
    return vtbackend::GitHeadServices { .executor = _linkProbeExecutor, .fileSystem = _gitFileSystem };
}

ContourGuiApp::~ContourGuiApp()
{
    // Before any member is destroyed: cancels the status-line commands still running or queued, so the
    // command pool joins without waiting out their timeouts.
    _commandShutdown.request_stop();
    // Ends the ssh prompts first: a link stopped below may be inside a prompt, and the transport factories
    // keep the broker alive past this destructor.
    // The router even without a broker: it ends the hooks that read the windows, which are going.
    _askpassRouter->shutdown();
    if (_askpassBroker)
        _askpassBroker->shutdown();
    // While the session manager they release their windows from still exists: the links outlive it.
    for (auto const& link: _daemonLinks)
        link->stop();
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
    auto* const controller = focused ? _sessionManager.controllerDisplaying(*focused) : nullptr;
    auto* const terminalDisplay = controller ? controller->activeDisplay() : nullptr;
    auto* const window = terminalDisplay ? terminalDisplay->window() : nullptr;
    if (!window)
    {
        // Nothing is due while the prompt is awaited; asking settles the prompt timeout.
        std::ignore = _keystrokeProbe->takeDueKey(vthost::latency::KeyboardFocus::Absent);
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
    _exitEventLoop(exitCode);
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

    auto const inheritedStore = currentEnvironmentEntries();
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
    _exitEventLoop(EXIT_SUCCESS);
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
    // Through the centre where there is one, so a GPU notice and a session's never come from two delegates.
    if (!_gpuNotifier)
        _gpuNotifier = platform::makeDesktopNotifier(
            std::chrono::seconds(10),
            // No window to show a tray message through at this point: where the platform's notifier is the
            // tray, the message is dropped, as it was before the centre.
            [](QString const& /*title*/,
               QString const& /*body*/,
               std::function<void()> const& /*onClicked*/) {},
            _userNotificationCenter,
            std::string { platform::ApplicationRequestPrefix },
            _hostPlatform);
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

std::optional<std::expected<void, std::string>> ContourGuiApp::pollNativeAttach(remote::DaemonLink& link,
                                                                                vthost::AttachBudget budget)
{
    auto attached = link.pollAttached(budget);
    if (!attached)
        return std::nullopt;
    if (!*attached)
        return attached;
    // After the connect: the drift offer reads what the handshake negotiated. Only a daemon this client
    // can restart -- the local one it launched on (DaemonLinkConfig::upkeep), which is what _restartDaemon is
    // set for -- is offered anything.
    link.offerDaemonRestartIfDrifted();
    return attached;
}

remote::DaemonLink& ContourGuiApp::startDaemonLink(remote::DaemonLinkConfig config)
{
    if (config.address.kind == vthost::proto::SwitchDaemonKind::Host)
        _hostsReached.insert(config.address.name);
    auto& link = *_daemonLinks.emplace_back(std::make_unique<remote::DaemonLink>(
        remote::DaemonLinkId { _nextDaemonLinkId++ }, std::move(config), *this, _sessionManager));
    // The first link to a daemon is the one to take back what it restores after a restart.
    remote::passOnLaunchResume(switchableLinks());
    return link;
}

remote::DaemonLink* ContourGuiApp::daemonLink(remote::DaemonLinkId id) const noexcept
{
    auto const found = std::ranges::find(_daemonLinks, id, &remote::DaemonLink::id);
    return found != _daemonLinks.end() ? found->get() : nullptr;
}

void ContourGuiApp::linkEnded()
{
    // At once: another link of its daemon takes back what a restart of it restores, should the one that
    // ended have been that link.
    remote::passOnLaunchResume(switchableLinks());
    // Not from inside the link, which is still running the call that ended it.
    QMetaObject::invokeMethod(this, [this] { destroyEndedLinks(); }, Qt::QueuedConnection);
}

remote::DaemonLinkConfig ContourGuiApp::daemonLinkConfig(std::shared_ptr<vthost::ITransportFactory> transport,
                                                         vthost::proto::AttachIntent intent,
                                                         std::string hostLabel,
                                                         window::DaemonLocality locality,
                                                         remote::DaemonAddress address,
                                                         askpass::AskpassRouteId askpassRoute)
{
    // The emulation settings the daemon gives the sessions this client creates, from the configuration
    // loaded already; every session created later takes them from the configuration in memory, exactly
    // as a local new tab does, so a reload reaches remote and local tabs alike. A failure is a warning:
    // hosting under the daemon's own resolved profile is a sane thing to do meanwhile.
    auto attachSettings = std::optional<vtbackend::Settings> {};
    if (auto resolved = currentEmulationSettings(); resolved)
        attachSettings = *std::move(resolved);
    else
        errorLog()("attach: {}; the daemon's own profile will be used for new sessions.", resolved.error());
    return remote::DaemonLinkConfig {
        .controller = remote::NativeController::Config {
            .transport = std::move(transport),
            .intent = intent,
            .profileName = profileName(),
            .workingDirectory = {},
            .attachSettings = std::move(attachSettings),
            .sessionSettings = [this] { return currentEmulationSettings(); },
            // Asked as each hello goes out: this config is built before the OS's scheme is read, and the
            // links a switch or a hand-over clone from it go on connecting long after.
            .colorPreference = [this] { return _shownColorPreference.load(); },
            .clock = core::platform::defaultSteadyClock(),
            .timeline = _startupTimeline,
            .powerMonitor = _powerMonitor.get(),
            .backoff = {},
        },
        .hostLabel = std::move(hostLabel),
        .locality = locality,
        .clientRelease = CONTOUR_VERSION_STRING,
        .opening = std::nullopt,
        .address = std::move(address),
        .arrival = std::nullopt,
        .askpassRoute = askpassRoute,
    };
}

std::vector<remote::DaemonLink*> ContourGuiApp::switchableLinks()
{
    return std::ranges::to<std::vector>(_daemonLinks
                                        | std::views::transform([](auto const& link) { return link.get(); }));
}

std::expected<remote::DaemonLinkConfig, std::string> ContourGuiApp::linkConfigFor(
    remote::DaemonAddress const& daemon, vtworkspace::WorkspaceId window)
{
    using vthost::proto::SwitchDaemonKind;
    // The prompts of the attach are shown in the window that asked: routed by its id, which stays the same
    // as it shows one workspace after another.
    auto const askpassRoute = [&]() -> askpass::AskpassRouteId {
        auto const* const owner = _sessionManager.controllerOwning(window);
        return owner != nullptr ? owner->windowId().value : askpass::AnyWindow;
    }();
    switch (daemon.kind)
    {
        case SwitchDaemonKind::Label: {
            // A label comes from a program or another daemon as well as from the user: it is joined into a
            // socket path only once it is one a daemon of its own could have.
            if (auto const valid = vthost::validateSocketLabel(daemon.name); !valid)
                return std::unexpected(
                    std::format("\"{}\" names no daemon: {}", daemon.name, vthost::describe(valid.error())));
            // A running daemon only: a switch starts none (New daemon… does, before it switches). Local, but
            // not this client's to restart or upgrade from the badge -- _restartDaemon and _upgradeDaemon
            // serve the launch's daemon alone, and a restart relaunches this process onto that one -- unless
            // its socket is the launch's (which --socket and $CONTOUR_MUX may move off the label's).
            auto const controlPath =
                vthost::muxSocketPath(vthost::socketLabelOf(daemon.name),
                                      cli::resolveSocketOption("", _config.daemon.value().socket));
            auto config =
                daemonLinkConfig(std::make_shared<vthost::UnixTransportFactory>(controlPath, nullptr),
                                 {},
                                 {},
                                 window::DaemonLocality::Local,
                                 daemon);
            if (_upgradeDaemon)
                config.upkeep = window::upkeepOfReachedDaemon(_launchControlPath, controlPath);
            return config;
        }
        case SwitchDaemonKind::Host:
            // Attach to Host's lookup: a saved host's name, or an ssh:// URL.
            return openHost(core::platform::normalizePath(_config.configFile), daemon.name, askpassRoute)
                .transform([&](OpenedHost opened) {
                    auto label = opened.transport.label;
                    return daemonLinkConfig(std::move(opened.transport.factory),
                                            {},
                                            std::move(label),
                                            window::DaemonLocality::Remote,
                                            daemon,
                                            askpassRoute);
                });
        case SwitchDaemonKind::Same: break;
    }
    return std::unexpected(std::string { "no daemon named" });
}

remote::DaemonLink& ContourGuiApp::startSwitchLink(remote::DaemonLinkConfig config)
{
    // Awaited by nobody: it tells the switch that started it how it landed.
    return startDaemonLink(std::move(config));
}

void ContourGuiApp::startWindowLink(remote::DaemonLinkConfig config)
{
    // Awaited by nobody: it opens its window once its connection shows the workspace it selects.
    std::ignore = startDaemonLink(std::move(config));
}

remote::ElsewhereNotices ContourGuiApp::elsewhereNotices()
{
    // Through the centre where there is one, as a session's notices are; where the platform's notifier is the
    // tray, the window the user last focused shows it.
    if (!_elsewhereNotifier)
        _elsewhereNotifier = platform::makeDesktopNotifier(
            _config.notificationCloseTimeout.value(),
            [this](QString const& title, QString const& body, std::function<void()> onClicked) {
                if (auto* const controller = _sessionManager.applicationController())
                    controller->requestNotification(title, body, std::move(onClicked));
            },
            _userNotificationCenter,
            std::format("{}finished/", platform::ApplicationRequestPrefix),
            _hostPlatform);
    auto const* const found = _config.findProfile(profileName());
    auto const& profile = found != nullptr ? *found : _config.profile();
    return remote::ElsewhereNotices {
        .policy = profile.notifyOnCommandFinish.value(),
        .bell = profile.bell.value(),
        .homeDirectory = _sessionManager.homeDirectory(),
        .notifier = *_elsewhereNotifier,
    };
}

void ContourGuiApp::destroyEndedLinks()
{
    // A link whose closed windows still hold a pane waits: it asks again as its last pane lets it go.
    // Each window's drift badge reads its own link's model (WindowController::daemonDrift).
    std::erase_if(_daemonLinks,
                  [this](auto const& link) { return link->isFinished() && _awaitedLinkId != link->id(); });
}

std::expected<vthost::client::DaemonUpgradeSuccess, vthost::client::DaemonUpgradeFailure> ContourGuiApp::
    upgradeLocalDaemon(std::filesystem::path const& controlPath, std::stop_token stop)
{
    // Never starts a daemon: an upgrade asks the one that runs. The answer waits on the new image's
    // readiness.
    auto const transport = std::make_shared<vthost::UnixTransportFactory>(controlPath, nullptr);
    auto const control = vthost::client::makeDaemonControl(transport, vthost::client::DefaultUpgradeTimeout);
    // The status reads before and after wait on nothing: a short timeout bounds how long one in flight holds
    // up a shutdown.
    auto const settling = vthost::client::makeDaemonControl(transport, vthost::client::DefaultControlTimeout);
    // Staged from this process's own copy, as a daemon this client starts is.
    auto const binary = vthost::makeOwnDaemonBinary(
        processEnvironment(), fileSystem(), CONTOUR_VERSION_STRING, std::filesystem::path { programPath() });
    auto pacer = vthost::client::SystemSettlePacer { std::move(stop) };
    return vthost::client::upgradeDaemonTo(*control, *settling, *binary, pacer);
}

remote::UpgradeStart ContourGuiApp::upgradeDaemon(remote::DaemonLink& link)
{
    if (_daemonWorker == DaemonWorker::Busy)
        return remote::UpgradeStart::AlreadyUnderWay;
    if (!_upgradeDaemon)
    {
        // The model shows the request in flight: it must hear that nothing will answer it.
        link.drift().noteUpgradeFailed(tr("this run has no local daemon to upgrade"));
        return remote::UpgradeStart::Started;
    }
    // No worker runs (see above), so the assignment joins none on the GUI thread. On shutdown the settling
    // stops waiting at once; a control call already under way is not interrupted, and ends within its
    // timeout (DefaultControlTimeout for the status reads, DefaultUpgradeTimeout for the upgrade request
    // itself). The outcome is told to the link by its id: it may have ended meanwhile, with its last window.
    _daemonWorker = DaemonWorker::Busy;
    _daemonRestartThread = std::jthread { [this, id = link.id()](std::stop_token const& stop) {
        auto const upgraded = _upgradeDaemon(stop);
        QMetaObject::invokeMethod(
            this,
            [this,
             id,
             reason = upgraded ? QString {}
                               : QString::fromStdString(vthost::client::describeFailure(upgraded.error()))] {
                _daemonWorker = DaemonWorker::Idle;
                auto* const upgradedLink = daemonLink(id);
                if (!upgradedLink)
                    return;
                // On success the connection comes back to the upgraded daemon, and the badge follows it.
                if (reason.isEmpty())
                    upgradedLink->drift().noteUpgradeRequestDone();
                else
                    upgradedLink->drift().noteUpgradeFailed(reason);
            },
            Qt::QueuedConnection);
    } };
    return remote::UpgradeStart::Started;
}

void ContourGuiApp::restartDaemon(remote::DaemonLink& link)
{
    if (!_restartDaemon)
        return;
    if (_daemonWorker == DaemonWorker::Busy)
    {
        link.drift().noteRestartFailed(
            QString::fromUtf8(window::UpgradeAlreadyUnderWay.data(), window::UpgradeAlreadyUnderWay.size()));
        return;
    }
    _daemonWorker = DaemonWorker::Busy;
    // Stopping the daemon drops this client's connection, which closes every window. Without this
    // the last window closing would quit the app before the queued hand-off below ever ran.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    link.stopForDaemonRestart();
    // The outcome is told to the link by its id: the restart closed its windows, which ended it.
    _daemonRestartThread = std::jthread { [this, id = link.id()] {
        auto result = _restartDaemon();
        QMetaObject::invokeMethod(
            this,
            [this, id, result = std::move(result)] {
                _daemonWorker = DaemonWorker::Idle;
                QGuiApplication::setQuitOnLastWindowClosed(true);
                auto* const restarted = daemonLink(id);
                if (!result)
                {
                    if (restarted)
                        restarted->drift().noteRestartFailed(QString::fromStdString(result.error()));
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
                    if (restarted)
                        restarted->drift().noteRestartFailed(
                            tr("the daemon restarted, but this window could not be reopened"));
                    quitIfWindowless();
                    return;
                }
                QCoreApplication::quit();
            },
            Qt::QueuedConnection);
    } };
}

void ContourGuiApp::openWindowFor(QScreen* screen)
{
    // The window's Main.qml claims the daemon workspace the link staged, through the attach-window binder.
    newWindow(screen);
}

std::expected<void, std::string> ContourGuiApp::restartLocalDaemon(std::string const& label,
                                                                   std::filesystem::path const& controlPath,
                                                                   vthost::DaemonSpawnOptions const& options)
{
    auto const endpoint = vthost::UnixEndpoint { .socketPath = controlPath };
    auto start = LocalDaemonStart { processEnvironment(), fileSystem(), programPath(), _daemonEnvironment };
    auto const starter = start.starter(label, endpoint, *_processPlacement, options);
    // Never starts a daemon itself: the restart below starts the new one through the starter. The stop waits
    // for the old daemon to write the workspace the new one restores.
    auto const control = vthost::client::makeDaemonControl(
        std::make_shared<vthost::UnixTransportFactory>(controlPath, nullptr),
        vthost::client::DefaultStopTimeout);
    return vthost::client::restartDaemon(*control, *starter)
        .transform_error([](vthost::client::DaemonRestartFailure const& failure) { return failure.detail; });
}

std::expected<ContourGuiApp::OpenedHost, std::string> ContourGuiApp::openHost(std::string const& configPath,
                                                                              std::string const& nameOrUrl,
                                                                              askpass::AskpassRouteId route,
                                                                              cli::SshPrompts prompting,
                                                                              std::string daemonFingerprint)
{
    auto host = config::resolveHost(configPath, nameOrUrl);
    if (!host)
        return std::unexpected { host.error() };
    return transportOf(nameOrUrl, *std::move(host), route, prompting, std::move(daemonFingerprint));
}

void ContourGuiApp::showBootstrapPrompt()
{
    if (_bootstrapPrompt)
        return;
    // Ended once its prompts are answered, rather than kept as a hidden window: the window that carries the
    // attach on, or the next prompt's own stand-in, takes its place.
    auto onDrained = [this] {
        // Not from inside the answer that drained it, and not if another prompt came in the meantime.
        QMetaObject::invokeMethod(
            this,
            [this] {
                if (_bootstrapPrompt && _bootstrapPrompt->host().pendingPrompt().isEmpty())
                    _bootstrapPrompt.reset();
            },
            Qt::QueuedConnection);
    };
    if (_bootstrapPromptFactory)
        _bootstrapPrompt = _bootstrapPromptFactory(_askpassRouter, std::move(onDrained));
    else if (_qmlEngine)
        _bootstrapPrompt = window::BootstrapPrompt::create(_askpassRouter, *_qmlEngine, std::move(onDrained));
}

void ContourGuiApp::refreshWindowAttention()
{
    // A quick terminal parked off screen is hidden for the user though the platform may still count its
    // window as visible.
    auto const isOnScreen = [this](askpass::AskpassRouteId route) {
        if (route == askpass::BootstrapRoute)
            return true; // the stand-in shows itself as a prompt asks
        // A route is the id of the window the prompts are shown in.
        auto const* const controller = _sessionManager.controllerOf(window::WindowId { route });
        if (controller == nullptr || !controller->isExposed())
            return false;
        return controller->role() != window::WindowRole::QuickTerminal
               || (_quickTerminal && window::quickTerminalKeepsAppAlive(_quickTerminal->state()));
    };
    auto sinks = std::vector<askpass::SinkExposure> {};
    for (auto const route: _askpassRouter->attachedRoutes())
        sinks.push_back(askpass::SinkExposure { .route = route,
                                                .state = isOnScreen(route)
                                                             ? askpass::SinkExposure::State::OnScreen
                                                             : askpass::SinkExposure::State::Hidden });
    _askpassRouter->updateExposure(sinks);
}

void ContourGuiApp::refreshAttachAttention()
{
    // A link whose attach has a prompt held, for want of a window on screen to show it, says so; and says no
    // more once it is shown.
    for (auto const& link: _daemonLinks)
        if (auto* const controller = link->liveController())
            controller->setUserAttention(_askpassRouter->isHolding(link->askpassRoute())
                                             ? remote::NativeController::UserAttention::Needed
                                             : remote::NativeController::UserAttention::NotNeeded);
}

std::shared_ptr<askpass::AskpassBroker> const& ContourGuiApp::askpassBroker()
{
    // Its servers and its prompts live on the GUI thread, and so does whoever asks for it.
    Q_ASSERT(std::this_thread::get_id() == _guiThread);
    if (!_askpassBroker)
    {
        // Qt-free, as the broker may be made before the application object exists (`contour client --host`).
        auto const executable = vtpty::currentExecutablePath();
        _askpassBroker = std::make_shared<askpass::AskpassBroker>(
            _askpassRouter,
            QString::fromStdString(executable ? core::pathToUtf8(*executable) : programPath()),
            core::systemRandom());
    }
    return _askpassBroker;
}

std::shared_ptr<vthost::IPinPrompt> ContourGuiApp::pinPromptFor(askpass::AskpassRouteId route,
                                                                cli::SshPrompts prompting)
{
    // A batch transport asks the user nothing: a probe must not put a dialog up.
    if (prompting == cli::SshPrompts::Batch)
        return nullptr;
    auto const& broker = askpassBroker();
    return std::make_shared<askpass::AskpassPinPrompt>(
        _askpassRouter, route, *this, [broker] { return broker->nextPromptId(); });
}

std::expected<ContourGuiApp::OpenedHost, std::string> ContourGuiApp::transportOf(
    std::string const& nameOrUrl,
    config::HostEntry host,
    askpass::AskpassRouteId route,
    cli::SshPrompts prompting,
    std::string daemonFingerprint)
{
    // A saved host by its name, an ad-hoc one by its URL, canonically spelled.
    auto label = nameOrUrl.contains("://") ? config::toString(host.url) : nameOrUrl;
    return cli::makeHostTransport(
               std::move(label),
               host,
               fileSystem(),
               vthost::makeSystemStdioSpawner(vtpty::ProgramSearch {
                   .path = _sshSearchPath, .workingDirectory = {}, .fileSystem = fileSystem() }),
               cli::SshPrompting {
                   .broker = prompting == cli::SshPrompts::Askpass
                                 ? std::make_shared<askpass::RoutedAskpassBroker>(askpassBroker(), route)
                                 : nullptr,
                   .probe = _sshVersionProbe },
               cli::TlsTrust { .knownDaemons = &_knownDaemons,
                               .pinPrompt = pinPromptFor(route, prompting),
                               .daemonFingerprint = std::move(daemonFingerprint),
                               .sshAgentEndpoint = core::net::defaultSshAgentEndpoint(processEnvironment()) })
        .transform([&](cli::HostTransport transport) {
            return OpenedHost { .entry = std::move(host), .transport = std::move(transport) };
        });
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
    request.daemonFingerprint = parameters().get<string>("contour.client.daemon-fingerprint");
    request.sizePolicy = parameters().get<string>("contour.client.size-policy");
    // Resolved against the daemon's workspaces once its transport is known (resolveClientWorkspace()).
    request.workspace = parameters().get<string>("contour.client.workspace");
    auto token = vthost::resolveToken(parameters().get<string>("contour.client.token"),
                                      parameters().get<string>("contour.client.token-file"));
    if (!token)
    {
        cerr << std::format("contour client: {}\n", token.error());
        return EXIT_FAILURE;
    }
    request.token = *std::move(token);
    auto const host = parameters().get<string>("contour.client.host");
    if (!host.empty())
        if (auto const given = firstGivenOption("contour.client", cli::HostExclusions))
        {
            cerr << std::format("contour client: pass either --host or --{}, not both\n", *given);
            return EXIT_FAILURE;
        }
    return launchClient(std::move(request), host);
}

int ContourGuiApp::launchClient(ClientRequest request, std::string const& host)
{
    if (!host.empty())
    {
        auto opened = openHost(
            request.config, host, askpass::AnyWindow, cli::SshPrompts::Askpass, request.daemonFingerprint);
        if (!opened)
        {
            cerr << std::format("contour client: {}\n", opened.error());
            return EXIT_FAILURE;
        }
        // The host's profile, unless --profile names one: the GUI renders with it and the daemon creates with
        // it.
        if (request.profile.empty())
            request.profile = opened->entry.profile;
        request.host = std::move(opened->transport);
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
    // The picker decides where the sessions live, in a process of its own: nothing is attached here.
    if (firstWindow() == FirstWindow::ConnectPicker)
        return {};

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
    // Every window opened from here on shows tmux's panes; it opens its first tab as usual, through the tmux
    // controller, and adopts the panes tmux already has once the GUI booted.
    _sessionManager.setAttachWindowBinder(
        _tmuxController->windowBinding(), [this](window::WindowController* controller) {
            if (controller != nullptr)
                _sessionManager.bindWindow(controller->displayedWorkspace(),
                                           _tmuxController->windowBinding());
            return false;
        });
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
    /// `Contents/Resources/fonts` in the macOS bundle, `<prefix>/share/contour/fonts` on Linux and Windows
    /// (the executable being `<prefix>/bin/contour`). Windows ships no terminal font of its own, it uses
    /// Cascadia Mono, but it ships the Nerd Fonts symbols font there like every other platform.
    [[nodiscard]] text::BundledFontDirectories bundledFontDirectories(fs::path const& executableDirectory)
    {
#ifdef __APPLE__
        return { .paths = { (executableDirectory / ".." / "Resources" / "fonts").lexically_normal() } };
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
    /// @return The TLS transport to a daemon over TCP, attached as a launch is.
    [[nodiscard]] std::expected<NativeAttachTarget, std::string> tcpAttachTarget(
        std::string const& connectTcp,
        std::string const& token,
        std::string const& tlsCaPath,
        std::string const& daemonFingerprint,
        core::platform::FileSystem const& fileSystem,
        vthost::KnownDaemons& knownDaemons,
        std::shared_ptr<vthost::IPinPrompt> pinPrompt,
        std::string const& sshAgentEndpoint,
        std::shared_ptr<vthost::IStdioSpawner> const& spawner)
    {
        auto const hostPort = vthost::parseHostPort(connectTcp);
        if (!hostPort)
            return std::unexpected(
                std::format("invalid --connect-tcp '{}' (expected HOST:PORT)", connectTcp));
        auto tcp = vthost::TcpEndpoint {
            .host = hostPort->first,
            .port = hostPort->second,
            .token = token,
            .caPem = {},
            .expectedFingerprint = daemonFingerprint,
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
            // The agent's keys sign in; there is no key file to name here.
            .transport = std::make_shared<vthost::TlsTransportFactory>(
                tcp,
                knownDaemons,
                std::move(pinPrompt),
                vthost::client::makeClientKeySourceProvider(
                    sshAgentEndpoint,
                    spawner,
                    nullptr,
                    fileSystem,
                    {},
                    vthost::SshTarget { .user = {}, .host = tcp.host, .port = tcp.port })),
            .intent = cli::launchIntent(std::nullopt),
        };
    }

    /// How long resolving `--workspace` may ask the daemon for its workspaces.
    constexpr auto ClientWorkspaceListingTimeout = std::chrono::milliseconds { std::chrono::seconds { 10 } };
} // namespace

namespace
{
    /// `working-directory`, as a file manager's "open terminal here" passes it, for a daemon attach: the
    /// daemon spawns the attach's session, so the directory travels in the hello rather than in the profile
    /// this process resolved.
    /// @param given The directory the command line names, UTF-8; empty for none.
    /// @return @p given made absolute against this process's own directory, which the daemon does not
    ///         share; empty for none, or for a path that cannot be made absolute.
    [[nodiscard]] std::filesystem::path attachWorkingDirectory(std::string const& given)
    {
        if (given.empty())
            return {};
        auto error = std::error_code {};
        auto const path = core::pathFromUtf8(given);
        auto absolute = path ? std::filesystem::absolute(*path, error) : std::filesystem::path {};
        if (!path || error)
        {
            errorLog()("attach: ignoring working directory '{}': not a usable path", given);
            return {};
        }
        return absolute;
    }
} // namespace

void ContourGuiApp::releaseHosting()
{
    _startupAttachPlan.reset();
    _deferredNativeAttach.reset();
    _onGuiBooted = nullptr;
    // Each stops serving its windows as it stops.
    for (auto const& link: _daemonLinks)
        link->stop();
    if (_tmuxController)
    {
        // Its attach-window binder goes with its windows.
        _sessionManager.releaseBinding(_tmuxController->windowBinding());
        _tmuxController->stop();
    }
}

void ContourGuiApp::ensurePowerMonitor()
{
    // A wake has the daemon probed instead of the connection given up, which would reconnect and shrink
    // the panes to the profile's size. Without a monitor only the steady clock can tell: on macOS it
    // counts the sleep, so the first liveness tick after it runs late and has the daemon probed; on Linux
    // and the BSDs it stops while the system is suspended, so there is nothing to detect -- and no false
    // loss either. A platform with no backend (a BSD, a Flatpak sandbox, a build without systemd) is
    // expected and logged where attaching is; an OS that refused the registration is not, and is said
    // where the user sees it. Created once per process: a failure is remembered, not retried, so either
    // is said once.
    if (!_powerMonitor && !_powerMonitorFailure)
    {
        if (auto monitor = core::platform::makeNativeSystemPowerMonitor(); monitor)
            _powerMonitor = *std::move(monitor);
        else
        {
            _powerMonitorFailure = monitor.error();
            if (monitor.error() == core::platform::PlatformError::NotImplemented)
                attachLog()("attach: no system sleep/wake notifications on this platform; {}",
                            NoPowerMonitorNote);
            else
                errorLog()("attach: no system sleep/wake notifications ({}); {}",
                           core::platform::toString(monitor.error()),
                           NoPowerMonitorNote);
        }
    }
}

std::expected<void, std::string> ContourGuiApp::startNativeAttach()
{
    auto const& settings = _config.daemon.value();
    auto target = std::expected<NativeAttachTarget, std::string> {};
    // Ten seconds for the local daemon: the first connect starts it too, when none serves, and that waits up
    // to five seconds for it to accept before the connect's own share of the budget begins.
    auto budget = vthost::AttachBudget { std::chrono::seconds(10) };
    auto hostLabel = std::string {};
    // A remote daemon is not ours to restart or upgrade; the local one is.
    auto locality = window::DaemonLocality::Remote;
    // How a window switch names the daemon; `--connect-tcp` names none.
    auto address = remote::DaemonAddress {};
    if (_clientRequest && _clientRequest->host)
    {
        // A remote daemon is started by the remote relay, never from here; it is attached as a launch is.
        auto& host = *_clientRequest->host;
        budget = host.attach;
        hostLabel = host.label;
        address = remote::DaemonAddress { .kind = vthost::proto::SwitchDaemonKind::Host, .name = host.label };
        target = NativeAttachTarget {
            .transport = host.factory,
            .intent = cli::launchIntent(std::nullopt),
        };
    }
    else if (_clientRequest && !_clientRequest->connectTcp.empty())
    {
        // A daemon met for the first time is asked about in a dialog, and the user takes the time they take:
        // no clock runs through the question (the transport bounds its dialling itself).
        budget = vthost::AttachBudget::promptsOpen(budget.handshake);
        target = tcpAttachTarget(
            _clientRequest->connectTcp,
            _clientRequest->token,
            _clientRequest->tlsCaPath,
            _clientRequest->daemonFingerprint,
            fileSystem(),
            _knownDaemons,
            pinPromptFor(askpass::AnyWindow, cli::SshPrompts::Askpass),
            core::net::defaultSshAgentEndpoint(processEnvironment()),
            vthost::makeSystemStdioSpawner(vtpty::ProgramSearch {
                .path = _sshSearchPath, .workingDirectory = {}, .fileSystem = fileSystem() }));
    }
    else
    {
        // The local daemon: its socket and size policy are the client's options, else the `daemon:` section.
        auto const socketLabel = vthost::socketLabelOf(_clientRequest ? _clientRequest->label : "");
        auto const& label = socketLabel.name;
        auto const controlPath = vthost::muxSocketPath(
            socketLabel,
            cli::resolveSocketOption(_clientRequest ? _clientRequest->socket : "", settings.socket));
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
        locality = window::DaemonLocality::Local;
        address = remote::launchDaemonAddress(controlPath, vthost::labelSocketDirectory());
        _launchControlPath = controlPath;
        _restartDaemon = [this, label, controlPath, daemonOptions] {
            return restartLocalDaemon(label, controlPath, daemonOptions);
        };
        _upgradeDaemon = [this, controlPath](std::stop_token stop) {
            return upgradeLocalDaemon(controlPath, std::move(stop));
        };
        // Before every connect, the first and each reconnect alike, so a daemon that crashed or was stopped
        // is started again. Spawned, if it must be, from this version's own copy where an upgrade would
        // disturb the installed binary, and that copy held leased until the daemon serves. Runs on the
        // transport's worker thread, one call at a time, so the attach's reactor goes on serving -- and
        // stop() does not wait -- while a spawned daemon comes up; nothing else uses what it captures.
        auto ensureDaemon = [start = std::make_shared<LocalDaemonStart>(
                                 processEnvironment(), fileSystem(), programPath(), _daemonEnvironment),
                             placement = _processPlacement,
                             repairOnce = std::make_shared<std::once_flag>(),
                             label,
                             controlPath,
                             daemonOptions]() -> std::expected<void, vthost::TransportError> {
            if (start->spawner->probe(controlPath) == vthost::DaemonPresence::Serving)
            {
                // Once per start, here on the transport's worker and not on the GUI thread: an upgrade that
                // moved the installation leaves the daemon serving, and its logon task naming a program that
                // is gone, which nothing else would notice until the next logon failed.
                std::call_once(*repairOnce, [&] { start->repairRegistration(label); });
                return {};
            }
            // A registered service is started before a daemon is spawned on demand.
            auto const starter = start->starter(
                label, vthost::UnixEndpoint { .socketPath = controlPath }, *placement, daemonOptions);
            if (auto const started = starter->start(); !started)
                // Transient: a reconnect tries again, and a first connect is not retried anyway.
                return std::unexpected(vthost::TransportError {
                    .kind = vthost::TransportErrorKind::Unreachable, .message = started.error() });
            return {};
        };
        target = NativeAttachTarget {
            .transport = std::make_shared<vthost::UnixTransportFactory>(controlPath, std::move(ensureDaemon)),
            .intent = cli::launchIntent(std::nullopt),
        };
    }
    if (!target)
        return std::unexpected(target.error());
    return prepareLaunchAttach(LaunchAttach {
        .transport = std::move(target->transport),
        .budget = budget,
        .hostLabel = std::move(hostLabel),
        .locality = locality,
        .address = std::move(address),
        .workspace = _clientRequest ? _clientRequest->workspace : std::string {},
    });
}

std::expected<void, std::string> ContourGuiApp::prepareLaunchAttach(LaunchAttach attach)
{
    // `--workspace` names a workspace as the control verbs do; what a connection follows is its native id.
    // The listing is a control call, and a daemon met for the first time is asked about in a dialog, which
    // needs the GUI up: so such a launch lists the workspaces once the GUI has booted, as the first step of
    // its attach wait (beginDeferredAttach()), and any other starts its link now.
    if (!attach.workspace.empty())
    {
        _deferredNativeAttach = std::move(attach);
        return {};
    }
    return startLaunchLink(std::move(attach), std::nullopt);
}

std::expected<void, std::string> ContourGuiApp::startLaunchLink(LaunchAttach attach,
                                                                std::optional<uint64_t> workspace)
{
    ensurePowerMonitor();

    // The connect, hello, first spawn and first snapshot run on the reactor while the GUI boots;
    // terminalGuiAction() waits for them through _awaitAttach just before the first window, by when booting
    // has already spent part of the budget, and settles the hosting there. Only this link, the launch's own,
    // states the directory contour was started in: a link a switch opens later to another daemon has no tab
    // to open there.
    auto config = daemonLinkConfig(std::move(attach.transport),
                                   cli::launchIntent(workspace),
                                   std::move(attach.hostLabel),
                                   attach.locality,
                                   std::move(attach.address));
    config.controller.workingDirectory =
        attachWorkingDirectory(parameters().get<string>("contour.terminal.working-directory"));
    // The launch's daemon is this client's to restart and upgrade when it is local: _restartDaemon and
    // _upgradeDaemon are set for it.
    if (attach.locality == window::DaemonLocality::Local)
        config.upkeep = window::DaemonUpkeep::ThisClient;
    auto const id = startDaemonLink(std::move(config)).id();
    // Attach mode adopts the daemon's workspaces into the windows once the first one booted, and on every
    // layout change after (which the link wires itself). Both find the link by its id, which no window has
    // ended before the first one opens.
    _onGuiBooted = [this, id] {
        if (auto* const link = daemonLink(id))
            link->followLayout();
    };
    // The budget runs from here, where the attach begins: a passphrase the listing asked for is not this
    // attach's to be extended by.
    _startupAttachPlan = StartupAttachPlan { .link = id, .budget = attach.budget.startingNow() };
    return {};
}

void ContourGuiApp::beginDeferredAttach(remote::StartupAttachReady ready)
{
    Require(_deferredNativeAttach.has_value());
    if (!_startupLister)
        _startupLister = remote::makeControlWorkspaceLister(ClientWorkspaceListingTimeout);
    _startupLister->list(
        remote::ListTarget { .address = _deferredNativeAttach->address,
                             .transport = _deferredNativeAttach->transport },
        [this, ready = std::move(ready)](remote::WorkspaceListing listed) {
            auto deferred = std::exchange(_deferredNativeAttach, std::nullopt);
            if (!deferred)
                return;
            // Said as startNativeAttach()'s caller says it: this runs later, with no caller to add the verb.
            auto const spec = deferred->workspace;
            auto const failed = [&spec](std::string const& why) {
                return std::unexpected(std::format("contour client: --workspace '{}': {}", spec, why));
            };
            if (!listed)
            {
                ready(failed(std::format("cannot list the daemon's workspaces: {}", listed.error().reason)));
                return;
            }
            auto const workspace = cli::resolveWorkspaceSpec(spec, *listed);
            if (!workspace)
            {
                ready(failed(workspace.error()));
                return;
            }
            auto started = startLaunchLink(*std::move(deferred), *workspace);
            if (started)
                followStartupLink();
            ready(std::move(started));
        });
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
            // The attach is given up (awaitNativeAttach() released the controller and its attach-window
            // binder): nothing waits for the daemon's workspaces, and the first window opens a tab of its
            // own, which runs here.
            errorLog()("Hosting this window's sessions in-process: {}", attached.error());
            _onGuiBooted = nullptr;
            _restartDaemon = nullptr;
            _upgradeDaemon = nullptr;
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
            "connect",
            "Opens a window on a chosen place for its sessions. TARGET is `daemon` (this computer's daemon; "
            "`daemon:LABEL` for another one), `local` (this window only, no daemon), the name of a saved "
            "host, "
            "or an `ssh://` or `tls://` address. Without a TARGET the Connect picker asks.",
            CLI::OptionList {
                CLI::Option { "config",
                              CLI::Value { ""s },
                              "Path to configuration file the GUI loads; it names the saved hosts.",
                              "FILE" },
                CLI::Option { "list",
                              CLI::Value { false },
                              "Prints the targets the picker would show, with the local daemon's state, and "
                              "exits." },
                CLI::Option { "json", CLI::Value { false }, "With --list: prints a JSON array." },
                CLI::Option { "log",
                              CLI::Value { ""s },
                              "Enables logging for a comma (,) separated list of tags, or `all` "
                              "(see `contour list-debug-tags`). Overrides $LOG.",
                              "TAGS" },
                CLI::Option { "log-file",
                              CLI::Value { ""s },
                              "Appends log output to FILE instead of standard error ('-' means "
                              "standard error).",
                              "FILE" },
            },
            CLI::CommandList {},
            CLI::CommandSelect::Explicit,
            CLI::Verbatim { "TARGET",
                            "Where the sessions live: daemon, daemon:LABEL, local, a saved host's name, or "
                            "an ssh:// or tls:// address. Omitted: the Connect picker asks." } });

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
                              "($CONTOUR_MUX when no --label is given).",
                              "PATH" },
                CLI::Option { "label",
                              CLI::Value { ""s },
                              "Socket label distinguishing daemon instances (default: `default`). Given, "
                              "it beats $CONTOUR_MUX.",
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
                              "Connect to a TCP daemon at HOST:PORT (TLS-encrypted; signs in with "
                              "your SSH agent's key, or the token) instead of the local socket.",
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
                              "for --connect-tcp. Omitted, the daemon is pinned on first use: "
                              "you are asked to trust its fingerprint, which is then kept in "
                              "known_daemons and must not change.",
                              "FILE" },
                CLI::Option { "daemon-fingerprint",
                              CLI::Value { ""s },
                              "The fingerprint a --connect-tcp or --host daemon without a CA must "
                              "present on first contact, as `contour daemon status` prints it there; "
                              "it is then pinned, and nobody is asked.",
                              "SHA256:..." },
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
                CLI::Option { "workspace",
                              CLI::Value { ""s },
                              "Shows only this daemon workspace -- its id or an id suffix (see "
                              "`contour ls`), its label or its 1-based position -- instead of what a "
                              "launch brings back; fails when it names no workspace, or several.",
                              "WORKSPACE" },
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
                    CLI::Deprecated { "Only supported for compatibility with very old KDE desktops." },
                    CLI::VerbatimStart::AfterValue },
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

    // default threshold is config::DefaultEarlyExitThreshold seconds
    if (parameterThreshold >= 0)
        return std::chrono::seconds(parameterThreshold);

    if (configThreshold != config::DefaultEarlyExitThreshold)
        return std::chrono::seconds(configThreshold);

    return std::chrono::seconds(config::DefaultEarlyExitThreshold);
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
    // std::nullopt for system -- and for every theme while the OS runs a contrast theme.
    _appliedContrastTheme = platformAppearance().contrastTheme;
    _effectiveGuiTheme = platform::effectiveGuiTheme(theme, _appliedContrastTheme);
    auto const scheme = platform::qtColorSchemeFor(_effectiveGuiTheme, colors, liveBackground);

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

    // The title band's appearance is the scheme just forced, or the OS's again. The palette change above
    // reaches every window too, but not when the palette came out the same (the OS already showed that
    // scheme), and the band must not keep asking for the old one then.
    _sessionManager.refreshChromeDependents();

    // A pinned dark/light chrome carries the terminal with it; leaving the pin hands it back to the OS.
    refreshColorPreference();
}

QString ContourGuiApp::applicationVersion() const
{
    return QStringLiteral(CONTOUR_VERSION_STRING);
}

void ContourGuiApp::followLiveBackground(vtbackend::RGBColor liveBackground)
{
    // Only `theme: terminal` follows the terminal's background; any other theme would only repaint the
    // whole application for the scheme it already shows. Likewise when the background moved without
    // crossing the light/dark threshold.
    // Kept even when nothing repaints now: leaving a contrast theme re-applies it.
    _liveBackground = liveBackground;
    auto const theme = _config.theme.value();
    if (theme != config::GuiTheme::Terminal)
        return;
    if (guiSchemeFor(theme, liveBackground) == _appliedGuiScheme)
        return;
    applyGuiTheme(theme, defaultProfileColors(), liveBackground);
}

std::optional<Qt::ColorScheme> ContourGuiApp::guiSchemeFor(
    config::GuiTheme theme, std::optional<vtbackend::RGBColor> liveBackground) const
{
    return platform::qtColorSchemeFor(platform::effectiveGuiTheme(theme, platformAppearance().contrastTheme),
                                      defaultProfileColors(),
                                      liveBackground);
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
    return config::resolveWindowControlStyle(configured, _hostPlatform);
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
        _windowControlStyleProvider->update(resolved, windowControlPlacement(resolved));
}

config::WindowButtonLayout ContourGuiApp::windowControlPlacement(config::WindowControlStyle resolved) const
{
    return config::windowButtonPlacement(resolved, platformAppearance().buttonLayout);
}

platform::IAccessibilityPreferenceSource const* ContourGuiApp::accessibilitySource() const
{
    if (!_accessibilitySource)
        _accessibilitySource = _makeAccessibilitySource();
    if (!_accessibilitySource)
        logUnbuiltAppearanceSource();
    return _accessibilitySource.get();
}

platform::IPlatformAppearance const* ContourGuiApp::platformAppearanceSource() const
{
    if (!_platformAppearance)
        _platformAppearance = _makePlatformAppearance();
    if (!_platformAppearance)
        logUnbuiltAppearanceSource();
    return _platformAppearance.get();
}

void ContourGuiApp::logUnbuiltAppearanceSource() const
{
    std::call_once(_unbuiltAppearanceSourceLogged, [] {
        startupLog()("The OS appearance sources cannot be built yet; their defaults stand in until a later "
                     "read builds them.");
    });
}

config::AccessibilityPreferences ContourGuiApp::accessibilityPreferences() const
{
    auto const* source = accessibilitySource();
    return source != nullptr ? source->current() : config::AccessibilityPreferences {};
}

platform::PlatformAppearance ContourGuiApp::platformAppearance() const
{
    auto const* source = platformAppearanceSource();
    return source != nullptr ? source->current() : platform::PlatformAppearance {};
}

config::WindowButtonLayout ContourGuiApp::desktopButtonLayout() const
{
    return platformAppearance().buttonLayout.value_or(config::fallbackButtonLayout());
}

platform::INativeMenuPresenter& ContourGuiApp::nativeMenuPresenter() noexcept
{
    return *_nativeMenus;
}

platform::ITitleBandMaterial& ContourGuiApp::titleBandMaterial() noexcept
{
    return *_titleBandMaterial;
}

platform::INativeCaptionButtons& ContourGuiApp::captionButtons() noexcept
{
    return *_captionButtons;
}

config::MenuPresentation ContourGuiApp::menuPresentation() const noexcept
{
    return config::ChromeTokenRows[static_cast<size_t>(_chromeStyle)].layout.menu;
}

window::TitleBandTokens ContourGuiApp::titleBandTokens(window::BarCentre centre) const
{
    if (_chromeStyleHost)
        return _chromeStyleHost->provider().titleBandTokens(centre);
    // No provider yet: an empty band is its bare-band token, the Pill rows' band their title-bar token; the
    // Flat rows' is their measured strip, which nobody has measured, so 0 leaves the band to its own height.
    auto const density = _config.chromeDensity.value();
    auto const row = resolvedChromeTokens();
    auto const height = [&] {
        if (centre == window::BarCentre::Empty)
            return static_cast<double>(row.size.bareBand);
        return row.layout.tabShape == config::TabShape::Pill ? static_cast<double>(row.size.titleBar) : 0.0;
    }();
    return window::TitleBandTokens {
        .style = _chromeStyle,
        .material = row.material.titleBand,
        .height = height,
        .cellHeight = 0.0,
        .density = density,
    };
}

window::TitleBand ContourGuiApp::titleBandOf(window::TitleBandInputs const& inputs) const
{
    if (_chromeStyleHost)
        return _chromeStyleHost->provider().titleBandOf(inputs);
    // No provider yet: the row as the provider would resolve it.
    return window::resolveTitleBand(
        window::titleBandRowOf(resolvedChromeTokens().layout, _config.chromeDensity.value()), inputs);
}

config::ChromeTokenRow ContourGuiApp::resolvedChromeTokens() const
{
    return config::resolveChromeTokens(_chromeStyle,
                                       accessibilityPreferences(),
                                       _config.chromeDensity.value(),
                                       platformAppearance().materials);
}

window::ChromeInputs ContourGuiApp::quickTerminalChromeInputs() const
{
    auto inputs = chromeInputs();
    inputs.density = window::chromeDensityFor(window::WindowRole::QuickTerminal, inputs.density);
    return inputs;
}

window::ChromeInputs ContourGuiApp::chromeInputs() const
{
    auto const* palette = config::preferredColorPalette(defaultProfileColors(), _colorPreference);
    return window::ChromeInputs {
        .row = _chromeStyle,
        .font = window::resolveChromeFont(_config, _chromeStyle, profileName()),
        .accessibility = accessibilityPreferences(),
        .appearance = window::chromeAppearanceOf(platformAppearance()),
        .scheme = config::chromeSchemeColorsOf(palette ? *palette : vtbackend::ColorPalette {}),
        .density = _config.chromeDensity.value(),
        .secureInput = _secureInput->support(),
    };
}

vtworkspace::SplitChrome ContourGuiApp::splitChrome(double devicePixelRatio) const
{
    if (_chromeStyleHost)
    {
        auto const& provider = _chromeStyleHost->provider();
        return window::splitChromeOf(
            provider.resolvedRow(), provider.units().horizontal, provider.units().vertical, devicePixelRatio);
    }
    return window::splitChromeOf(resolvedChromeTokens(), 1.0, 1.0, devicePixelRatio);
}

std::chrono::milliseconds ContourGuiApp::headerRevealDelay() const
{
    auto const milliseconds = _chromeStyleHost
                                  ? _chromeStyleHost->provider().resolvedRow().motion.headerRevealMs
                                  : resolvedChromeTokens().motion.headerRevealMs;
    return std::chrono::milliseconds { milliseconds };
}

int ContourGuiApp::windowRadius() const
{
    return _chromeStyleHost ? _chromeStyleHost->provider().resolvedRow().radii.window
                            : config::resolveChromeTokens(
                                  _chromeStyle, accessibilityPreferences(), _config.chromeDensity.value())
                                  .radii.window;
}

void ContourGuiApp::refreshChromeStyle()
{
    // Guarded as applyWindowControlStyle() is: the host is built partway through terminalGuiAction(),
    // and a reload or an OS change may arrive before it (or without it, in a headless test).
    if (_chromeStyleHost)
        _chromeStyleHost->update(chromeInputs());
    if (_quickChromeStyleHost)
    {
        _quickChromeStyleHost->update(quickTerminalChromeInputs());
    }
    // The quick terminal follows a changed hotkey, its focus-loss rule and the motion preference.
    if (_quickTerminal)
        _quickTerminal->reconfigure(_config.quickTerminal.value(), resolvedChromeTokens().motion);
    // Not behind the guard: which pane headers show (the density) and each window's title band (its material
    // under Reduce Transparency) are decided headless too.
    _sessionManager.refreshChromeDependents();
}

window::WindowController* ContourGuiApp::focusedController() const noexcept
{
    auto const window = _sessionManager.focusedWindow();
    return window ? _sessionManager.controllerDisplaying(*window) : nullptr;
}

window::AppMenuBarSources ContourGuiApp::appMenuBarSources()
{
    return window::AppMenuBarSources {
        .state =
            [this] {
                if (auto const* const controller = focusedController())
                    return controller->menuBarState();
                return command::menuBarStateOf(_config);
            },
        .chords = [this] { return command::chordIndex(_config); },
        .refreshSwitching =
            [this] {
                if (auto const* const controller = focusedController())
                    controller->refreshSwitching();
            },
        .run = [this](actions::Action const& action,
                      command::ActionScope scope) { runMenuBarAction(action, scope); },
    };
}

void ContourGuiApp::runMenuBarAction(actions::Action const& action, command::ActionScope scope)
{
    switch (scope)
    {
        case command::ActionScope::Window:
            if (auto* const controller = focusedController())
                controller->runMenuBarAction(action);
            return;
        case command::ActionScope::Application:
            if (auto* const controller = _sessionManager.applicationController();
                controller != nullptr && controller->activeSession() != nullptr)
                controller->runMenuBarAction(action);
            else
                runWithoutWindow(action, controller != nullptr ? controller->screen() : nullptr);
            return;
    }
}

void ContourGuiApp::warnOfMenuOwnedBindings() const
{
    for (auto const& chord: window::menuOwnedBindings(_config.inputMappings.value(), _menuBarPlacement))
        errorLog()(
            "input_mapping binds {}, which the macOS menu bar owns: the menu takes it, and the binding "
            "never runs.",
            command::shortcutText(chord, command::ShortcutStyle::Text));
}

void ContourGuiApp::runWithoutWindow(actions::Action const& action, QScreen* anchor)
{
    if (isStartingUp() && remote::duringStartup(action) == remote::DuringStartup::Refused)
    {
        attachLog()("Menu bar action \"{}\" ignored: the launch still waits for its attach.",
                    actions::name(action));
        return;
    }
    if (std::holds_alternative<actions::Quit>(action))
    {
        // The prompts that are open end with the app: ssh's helpers are told, and the attach is given up.
        _askpassRouter->shutdown();
        // As TerminalSession's Quit does: unwind the event loop rather than exit().
        _exitEventLoop(EXIT_SUCCESS);
        return;
    }
    if (std::holds_alternative<actions::NewTerminal>(action))
    {
        // As TerminalSession::spawnNewTerminal() does, less the working directory no session can give. No
        // window shows a session to open the new one beside: a daemon link ends with the last window it
        // served, and a link still booting adopts the window that opens now as its boot window.
        newWindow();
        return;
    }
    if (std::holds_alternative<actions::NewWindowTo>(action))
    {
        // With no terminal window to hang from, the picker is a window of its own.
        openConnectPicker(PickerPresentation::Launcher, anchor);
        return;
    }
    if (auto const* const configuration = std::get_if<actions::OpenConfiguration>(&action);
        configuration != nullptr && configuration->target == actions::ConfigurationTarget::SettingsWindow)
    {
        // The Settings window is the app's own, and opens with no terminal window at all.
        openSettings(anchor);
        return;
    }
    // About, Open Configuration File and the rest show in a window, or act on a session's configuration.
    errorLog()("Menu bar action \"{}\" needs a window, and none is open.", actions::name(action));
}

void ContourGuiApp::onConfigurationReloaded()
{
    refreshChromeStyle();
    // Every link states the reloaded session settings to its daemon, not only the launch's.
    for (auto const& link: _daemonLinks)
        link->onConfigurationReloaded();
    // The app's Settings reads the configuration the reload replaced; built on first use, so maybe not yet.
    if (_settingsController)
        _settingsController->refresh();
    // Guarded as refreshChromeStyle() is: the notice is built with the QML engine, which a headless
    // app never creates.
    if (_newDefaultsNotice)
        _newDefaultsNotice->refresh();
    // Hosts may have come or gone; the Dock menu and the jump list list them. Not before the first window
    // exists, which builds the menu.
    if (_launcherMenu)
        refreshLauncherMenu();
    // Guarded likewise: the bar is built with the engine, under a global placement alone. Its rows carry
    // the profiles, hosts, layouts and chords the reload may have changed.
    if (_appMenuBar)
    {
        _appMenuBar->rebuild();
        warnOfMenuOwnedBindings();
    }
}

void ContourGuiApp::onAccessibilityPreferencesChanged(config::AccessibilityPreferences preferences)
{
    refreshChromeStyle();
    // The terminal's own animations follow the motion preference too, in every running session.
    sessionsManager().updateMotionPreference(preferences.motion);
}

void ContourGuiApp::onPlatformAppearanceChanged(platform::PlatformAppearance const& appearance)
{
    // A contrast theme coming or going hands the chrome to the OS palette, or back to the configured
    // theme's (R-l). Only then: an accent or light/dark change must not undo a live pin -- the settings
    // page's, or a live (OSC 11) background's, which is followLiveBackground()'s. Applied over the focused
    // session's live background, if it set one, as followLiveBackground() would.
    if (appearance.contrastTheme != _appliedContrastTheme)
        applyGuiTheme(_config.theme.value(), defaultProfileColors(), _liveBackground);
    // Under a contrast theme the close button washes like its neighbours instead of turning red.
    if (_windowControlStyleProvider)
        _windowControlStyleProvider->setContrastTheme(appearance.contrastTheme);
    refreshChromeStyle();
    // The window controls follow the desktop's button layout live.
    if (_windowControlStyleProvider)
    {
        auto const resolved = resolvedWindowControlStyle(_config.windowControlStyle.value());
        _windowControlStyleProvider->update(resolved,
                                            config::windowButtonPlacement(resolved, appearance.buttonLayout));
    }
    // And so does the Settings window's own close button, where it draws one.
    if (_settingsWindow)
        _settingsWindow->refreshWindowControls();
    // Every overlay scrollbar follows the system's scroller style live.
    sessionsManager().updateSystemScrollBars(appearance.scrollBars);
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    // The terminal's light/dark follows what the OS publishes, over Qt's own reading (ruling R-h).
    applyOsColorPreference(
        platform::osColorPreference(appearance.colorScheme, QGuiApplication::styleHints()->colorScheme()));
#endif
}

void ContourGuiApp::applyOsColorPreference(vtbackend::ColorPreference preference)
{
    _osColorPreference = preference;
    refreshColorPreference();
}

void ContourGuiApp::rememberColorPreference(vtbackend::ColorPreference preference)
{
    if (auto const saved = guiState()->update(
            [preference](config::GuiState& state) { state.lastColorPreference = preference; });
        !saved)
        errorLog()("Could not remember the color preference: {}.", config::describe(saved.error()));
}

void ContourGuiApp::refreshColorPreference()
{
    auto const preference = platform::terminalColorPreference(_effectiveGuiTheme, _osColorPreference);
    if (_colorPreference == preference)
        return;

    _colorPreference = preference;
    // Before the sessions state it, so a hello going out meanwhile does not state the old one.
    auto const previous = _shownColorPreference.exchange(preference);
    display::displayLog()("Color preference changed to {} mode\n", _colorPreference);
    sessionsManager().updateColorPreference(_colorPreference);
    // The settings page previews a light/dark profile in the appearance the terminals now use.
    if (_settingsController)
        _settingsController->setColorPreference(_colorPreference);
    // The Terminal row paints with the scheme the new preference selects.
    refreshChromeStyle();
    // Last, so the file write never holds up the terminals' recoloring. Until the OS's scheme is first read
    // the value was what the GUI state remembered, so a launch that shows what the last one showed writes
    // nothing.
    if (previous != preference)
        rememberColorPreference(preference);
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
            auto const verbatim = classifyVerbatimCommand(
                frontCommand,
                isReachableProgram,
                vtpty::Process::programHost(vtpty::sandboxEscapeFromConfig(profile->escapeSandbox.value())));

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
    auto const executable = vtpty::currentExecutablePath();
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

text::FontLocator& ContourGuiApp::fontLocator(vtrasterizer::FontLocatorEngine engine)
{
    auto& platformLocator = vtrasterizer::createFontLocator(engine);
    if (engine == vtrasterizer::FontLocatorEngine::Mock)
        return platformLocator;

    std::call_once(_fontLocatorBuilt, [&] {
        // Found beside the executable like the fonts registered above. Where there is none (a build tree that
        // was never installed and has no staged copy, or one built without the symbols font), the locator
        // wraps nothing and the platform's own is what is asked.
        auto symbolsFont = fs::path {};
        if (auto const executable = vtpty::currentExecutablePath())
            if (auto const directories = bundledFontDirectories(executable->parent_path());
                !directories.paths.empty())
                symbolsFont = directories.paths.front() / text::NerdFontSymbolsFileName;
        _nativeFontLocator = std::make_unique<text::NerdFontSymbolsLocator>(platformLocator, symbolsFont);
        startupLog()("Nerd Fonts symbols font {}: {}.",
                     _nativeFontLocator->hasSymbolsFont() ? "bundled" : "not found",
                     core::pathToUtf8(symbolsFont));
    });
    return *_nativeFontLocator;
}

int ContourGuiApp::fontConfigAction()
{
    if (!loadConfig("font-locator"))
        return EXIT_FAILURE;

    registerBundledFonts();

    vtrasterizer::FontDescriptions const& fonts =
        _config.profile(_config.defaultProfileName.value())->fonts.value();
    text::FontDescription const& fontDescription = fonts.regular;
    text::FontLocator& fontLocator = this->fontLocator(fonts.fontLocator);
    text::FontSourceList const fontSources = fontLocator.locate(fontDescription);

    std::cout << std::format("Matching fonts using  : {}\n", fonts.fontLocator);
    std::cout << std::format("Font description      : {}\n", fontDescription);
    std::cout << std::format("Number of fonts found : {}\n", fontSources.size());
    for (text::FontSource const& fontSource: fontSources)
        std::cout << std::format("  {}\n", fontSource);

    return EXIT_SUCCESS;
}

void ContourGuiApp::registerForRestart()
{
    auto const& argv = commandLine();
    if (auto const registered = _applicationRestart->registerRestart(
            platform::restartCommandLine(platform::restartArguments(argv)));
        !registered)
        errorLog()("Could not register to be restarted after an update: {}", registered.error().message());
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
    auto const hostingReleased = core::Finally { [this] { releaseHosting(); } };
    // The one environment a daemon this launch spawns, and the service commands that look for or start its
    // unit, run under: snapshotted first, while this is the only thread, as the attach runs them on a worker
    // beside the boot, which goes on changing the environment -- the GPU edits below, a QApplication
    // constructor that unsets the activation tokens, later a quick-terminal panel that sets and unsets
    // XDG_ACTIVATION_TOKEN. What those edits remove is taken out of the snapshot here instead
    // (bootRemovedGpuVariables(), vthost::ClientActivationVariables); what they set was not set before, so
    // the snapshot never had it.
    {
        auto const entries = currentEnvironmentEntries();
        auto const inherited = std::vector<std::string_view>(entries.begin(), entries.end());
#ifdef __linux__
        // Only Linux chooses its OpenGL GPU through the environment, and takes back what a parent set.
        auto const parentSelfSet = vtpty::environmentValue(
            entries, display::SelfSetEnvironmentMarker, vtpty::PlatformEnvironmentNames);
#else
        auto const parentSelfSet = std::optional<std::string_view> {};
#endif
        _daemonEnvironment =
            vthost::daemonEnvironment(inherited, display::bootRemovedGpuVariables(parentSelfSet));
    }
    // The daemon link connects before the OS's scheme can be read, so its first hello states what the
    // terminals showed when this app last ran (none on a first launch); corrected once the scheme is read.
    if (auto const state = guiState()->load())
        _shownColorPreference = state->lastColorPreference;
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

    // The driver stack reads its GPU-selection variables when the display connection opens. After the
    // attach has started, so that enumerating the GPUs does not delay it; its threads read none of what
    // these change (the daemon's environment is the snapshot above).
    takeGpuFallbackRequest();
    applyOpenGlGpuSelection();
    // Before the multimedia warm-up below loads the backend.
    keepMultimediaOffTheGpus();

    // NB: We use QApplication over QGuiApplication because we want to use SystemTrayIcon.
    // NOTE: Cannot use ScopedTimer here because QApplication must live until end of function.
    auto const qtInitStart = std::chrono::steady_clock::now();
    // Before the QApplication: Qt (6.11+) registers this id with the host portal itself, and must do so
    // before its own platform theme makes the first portal call -- the portal associates the bus connection
    // with an application on that first call and refuses a later Register on it.
    // The same name setupQCoreApplication settles on later (on Wayland, the profile's wm_class), so Qt does
    // not try a second Register when that call changes it. The platform is not chosen yet; Qt picks Wayland
    // when QT_QPA_PLATFORM asks for it or, unset, when a Wayland display is there.
    QGuiApplication::setDesktopFileName([&] {
        auto const* const profile = _config.profile(profileName());
        auto const wmClass = profile ? QString::fromStdString(profile->wmClass.value()) : QString {};
        auto const requested = qEnvironmentVariable("QT_QPA_PLATFORM");
        auto const likelyWayland = requested.isEmpty() ? qEnvironmentVariableIsSet("WAYLAND_DISPLAY")
                                                       : requested.startsWith(QStringLiteral("wayland"));
        return likelyWayland && !wmClass.isEmpty() ? wmClass : QStringLiteral(CONTOUR_APP_ID);
    }());
    QApplication const app(qtArgsCount, (char**) qtArgsPtr.data());
    // The platform Qt runs on is known now; a test injected its own probe at construction.
    if (!_dragEndProbe)
        _dragEndProbe = window::makeDragEndProbe(QGuiApplication::platformName());
    // As early as it can be: the synthesizer probes for a voice on a thread of its own from here on, so
    // the answer is usually in before the first menu asks. A test injected its own.
    if (!_speechSynthesizer)
        _speechSynthesizer = platform::makeSpeechSynthesizer();
    // So that Windows Installer, replacing a running Contour, can close it and open it again as it was
    // started. Failing to register only costs that reopening, so it is logged and nothing more.
    registerForRestart();
    // Every Qt object this app owns goes before QApplication, on every return from here on -- the
    // failed-attach one included, which leaves before the event loop ever runs. Declared right after
    // `app`, so it runs before `app` is destroyed.
    auto const releaseQtObjects = core::Finally { [this] {
        // The menu bar first of all: its sources read the windows the engine owns, and it is Qt Widgets.
        _appMenuBar.reset();
        // The Settings window next, built on the engine, and then the controller its host borrowed.
        _settingsWindow.reset();
        // Built on the engine, and the Dock menu is a widget: both go before the engine and the application.
        _connectPickerWindow.reset();
        _connectPicker.reset();
        _launcherMenu.reset();
        _settingsController.reset();
        // The QML engine next: its context properties point at the style providers, and the windows
        // the helpers below served are gone with it.
        _qmlEngine.reset();
        // Its window went with the engine; the host cancels the prompts it still holds, so that none targets
        // a sink that is gone.
        _bootstrapPrompt.reset();
        // The GPU-selection helpers hold Qt objects (a timer, a D-Bus notifier, a Vulkan instance).
        _latencyProbeDriver.reset();
        _firstFrameWatchdog.reset();
        _gpuNotifier.reset();
#ifdef CONTOUR_WITH_GPU_SELECTION
        _graphicsDeviceSelector.reset();
#endif
        // Its engine lives on a QThread; from here on speechSynthesizer() is the voice that cannot speak.
        _speechSynthesizer.reset();
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
    // before applyGuiTheme() may override the application color scheme below, which would make the
    // style hints report our own pin; applyGuiTheme() then lets a pinned theme (theme: dark|light) carry
    // the terminal with it.
    // The platform appearance source is built here, on this first read, now that the QGuiApplication
    // exists; what it publishes outranks Qt's own reading (ruling R-h).
    _osColorPreference = platform::osColorPreference(platformAppearance().colorScheme,
                                                     QGuiApplication::styleHints()->colorScheme());
    _colorPreference = _osColorPreference;

    display::displayLog()("Color theme mode at startup: {}", _colorPreference);

    connect(
        QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, [this](Qt::ColorScheme newScheme) {
            applyOsColorPreference(platform::osColorPreference(platformAppearance().colorScheme, newScheme));
        });
#endif

    // Apply the configured GUI chrome theme (dark/light/system). Must run after the OS scheme has
    // been captured above and before the first QQuickWindow is created, so the chrome opens themed.
    applyGuiTheme(_config.theme.value(), defaultProfileColors());
    // From here on the daemon links' hellos state what the terminals show; until now they stated what the
    // last launch's terminals showed, which this remembers for the next launch when it differs.
    if (_shownColorPreference.exchange(_colorPreference) != _colorPreference)
        rememberColorPreference(_colorPreference);

    // Pin the chrome row's Qt Quick Controls style app-wide, so the hand-drawn tab strip, its controls and
    // the tab context menu render, blend and stay readable on every OS. Qt otherwise picks the native style
    // per platform, or the flat Basic style, which gives the menu no opaque, themed, readable surface on our
    // transparent window. NB: set unconditionally — QQuickStyle::name() already resolves to the (non-empty)
    // native default here, so guarding on it silently skips the row's style.
    //
    // The macOS and ContourLinux rows take Fusion. The Fluent row takes FluentWinUI3, Windows 11's own:
    // its opaque button chrome and dark-only menu text no longer clash with the palette-driven title bar,
    // because every chrome control sets its own `background` and `contentItem`, and ContourMenu.qml sets its
    // palette per colour group, so the style paints only what the chrome leaves to it.
    //
    // `ui_style: terminal` swaps that for ContourTui, our own style (src/contour/styles/ContourTui),
    // which paints a control as terminal chrome: cell-quantized, square-cornered, in the terminal
    // font. Fusion stays the FALLBACK, so a control ContourTui does not implement still gets a
    // readable, themed one rather than the flat Basic default.
    //
    // This is why `ui_style` takes effect on the next start rather than live: the style is resolved
    // once, here, before the first control exists, and Qt offers no way to re-resolve it afterwards.
    _chromeStyle = config::resolveChromeStyle(_config.uiStyle.value(), _hostPlatform);
    constexpr auto FallbackControlsStyle = std::string_view { "Fusion" };
    QQuickStyle::setFallbackStyle(QString::fromUtf8(FallbackControlsStyle));

    selectGraphicsApi();

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
    qmlRegisterUncreatableType<window::NoticeModel>("Contour.Terminal", 1, 0, "NoticeModel", "Created by the window controller.");
    qmlRegisterUncreatableType<window::SettingsController>("Contour.Terminal", 1, 0, "SettingsController", "Created by the application.");
    qmlRegisterUncreatableType<window::SettingsWindowHost>("Contour.Terminal", 1, 0, "SettingsWindowHost", "Created by the application.");
    qmlRegisterUncreatableType<window::SettingsPreview>("Contour.Terminal", 1, 0, "SettingsPreview", "Created by the Settings window host.");
    session::registerPermissionQmlTypes();
    window::registerTabBarQmlTypes();
    qRegisterMetaType<session::TerminalSession*>("TerminalSession*");
    qRegisterMetaType<session::PaneProxy*>("PaneProxy*");
    qRegisterMetaType<window::WindowController*>("WindowController*");
    qRegisterMetaType<window::CommandPaletteModel*>("CommandPaletteModel*");
    qRegisterMetaType<window::NoticeModel*>("NoticeModel*");
    qRegisterMetaType<window::SettingsController*>("SettingsController*");
    qRegisterMetaType<window::SettingsWindowHost*>("SettingsWindowHost*");
    qRegisterMetaType<window::SettingsPreview*>("SettingsPreview*");
    // clang-format on

    // Constructed with the resolved style rather than default-constructed and then applied: a
    // constructed object is a usable object, and the first frame must already draw the right
    // controls. applyWindowControlStyle() below exists for the reloads that follow.
    auto const windowControlStyle = resolvedWindowControlStyle(_config.windowControlStyle.value());
    _windowControlStyleProvider =
        make_unique<window::WindowControlStyleProvider>(windowControlStyle,
                                                        windowControlPlacement(windowControlStyle),
                                                        _chromeStyle,
                                                        platformAppearance().contrastTheme,
                                                        window::fontDatabaseAvailability());

    {
        auto const timer = crispy::ScopedTimer(startupLog, "QML engine setup");
        _qmlEngine = make_unique<QQmlApplicationEngine>();
        // A prompt that arrived before the engine existed (a startup attach reaching ssh first) found no
        // stand-in to show it; it waits in the router, so show it now.
        if (_askpassRouter->waitingCount() > 0)
            showBootstrapPrompt();

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

        // The chrome row's style is pinned here, once the engine exists and before it loads any QML, so
        // that the engine itself says where styles are searched. A style this Qt build does not ship
        // (FluentWinUI3 on an older Qt, say) would fail the whole QML load with `module "<style>" is not
        // installed`, so the row falls back to Fusion, with a warning.
        auto const requestedControlsStyle = config::uiStyleTokens(_chromeStyle).quickControlsStyle;
        auto const controlsStyle = config::chooseControlsStyle(
            requestedControlsStyle, FallbackControlsStyle, [&](std::string_view style) {
                return platform::isControlsStyleInstalled(style, _qmlEngine->importPathList());
            });
        if (controlsStyle.origin == config::ControlsStyleOrigin::FellBackToDefault)
            errorLog()("The Qt Quick Controls style {} is not installed on this system; using {} instead.",
                       requestedControlsStyle,
                       controlsStyle.style);
        QQuickStyle::setStyle(QString::fromStdString(controlsStyle.style));

        QQmlContext* context = _qmlEngine->rootContext();
        context->setContextProperty("terminalSessions", &_sessionManager);
        // The new-defaults banner's decision: "Use New" writes settings.yml through the one Settings save
        // model, which reloads; "Keep Mine" remembers the answer in the GUI state file beside contour.yml.
        _newDefaultsNotice = make_unique<window::NewDefaultsNotice>(
            [this]() -> config::Config const& { return _config; },
            [this](std::string_view key, std::string_view value) {
                return settingsController()
                    .writeGlobalOverride(key, value)
                    .transform_error([](config::SettingsError const& error) { return error.message; });
            },
            guiState());
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

    // The application's menu bar, where the host has one apart from its windows. It reads the focused
    // window, or the configuration alone without one, and runs its rows on that window's active session.
    if (_menuBarPlacement == command::MenuBarPlacement::Global)
    {
        _appMenuBar = make_unique<window::AppMenuBar>(appMenuBarSources(), *_menuBarIntegration);
        warnOfMenuOwnedBindings();
        // Built before any window, so its window rows start disabled; a menu-owned key (⌘M) fires its row
        // with no menu ever opened, so the rows follow the focused window as it changes. Posted, so the
        // window controllers have taken the change first (WindowController::onOSWindowActiveChanged).
        connect(qGuiApp, &QGuiApplication::focusWindowChanged, this, [this] {
            platform::postToObject(this, [this] {
                if (_appMenuBar)
                    _appMenuBar->refreshAll();
            });
        });
    }
    refreshLauncherMenu();
    // A process that only asks where to connect has no terminal to slide down, and its hotkey would race the
    // process it starts.
    if (firstWindow() == FirstWindow::Terminal)
        startQuickTerminal();
    _startupTimeline.mark(vthost::latency::StartupMark::QmlEngineReady);

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

    // Attach mode's connect runs beside everything above and goes on beside the event loop: the first window
    // needs it done, and decides whether there is a daemon to show at all. Its ssh prompts are answered in
    // the stand-in window until then, and nothing but those can start a window or reconfigure the app
    // meanwhile.
    // A launch that names a workspace lists the daemon's workspaces first, as the wait's first step: a daemon
    // met for the first time is asked about in the stand-in window, with the engine up and nothing else able
    // to start a window meanwhile.
    if (_startupAttachPlan || _deferredNativeAttach)
        startAttachWait();
    else if (auto const booted = bootFirstWindow(); !booted)
    {
        cerr << booted.error() << '\n';
        return EXIT_FAILURE;
    }

    // Run the event loop FIRST (it populates _exitStatus via onExit during the run), THEN map that
    // status to the process exit code. The mapping is the pure exitCodeFor() (ExitCode.h), extracted
    // so it is unit-testable without the event loop. NB: sequenced explicitly — passing exec() as an
    // argument alongside _exitStatus would read _exitStatus before the loop populated it.
    auto const loopResult = QApplication::exec();
    // A finished probe has the last word: the echo child it ends exits 0 whether or not the probe succeeded.
    auto const rv = _startupExitCode.value_or(
        _latencyProbeExitCode.value_or(session::exitCodeFor(_exitStatus, loopResult)));

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

void ContourGuiApp::selectGraphicsApi()
{
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

namespace
{
    /// @return The screen under the pointer, or the primary one.
    [[nodiscard]] QScreen* targetScreen()
    {
        auto* screen = QGuiApplication::screenAt(QCursor::pos());
        return screen != nullptr ? screen : QGuiApplication::primaryScreen();
    }

    /// @return The available geometry of the screen under the pointer, or of the primary one.
    [[nodiscard]] window::ScreenRect screenUnderPointer()
    {
        auto const* const screen = targetScreen();
        if (screen == nullptr)
            return window::ScreenRect {};
        auto const area = screen->availableGeometry();
        return window::ScreenRect {
            .x = area.x(), .y = area.y(), .width = area.width(), .height = area.height()
        };
    }

    /// @return The full geometry of every screen but the one under the pointer.
    [[nodiscard]] std::vector<window::ScreenRect> otherScreens()
    {
        auto const* const target = targetScreen();
        auto others = std::vector<window::ScreenRect> {};
        for (auto const* const screen: QGuiApplication::screens())
            if (screen != target)
            {
                auto const area = screen->geometry();
                others.push_back(window::ScreenRect {
                    .x = area.x(), .y = area.y(), .width = area.width(), .height = area.height() });
            }
        return others;
    }

    /// @return Who places windows: only the compositor does under Wayland.
    [[nodiscard]] window::PlacementControl placementControl()
    {
        return QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))
                   ? window::PlacementControl::Compositor
                   : window::PlacementControl::Client;
    }
} // namespace

void ContourGuiApp::startQuickTerminal()
{
    if (_quickTerminal)
        return;
    _quickTerminal = std::make_unique<window::QuickTerminalController>(
        window::QuickTerminalHost {
            .hotkey = *_globalHotkey,
            .panel = *_quickPanel,
            .createWindow = [this] { return openQuickTerminal(); },
            .screenUnderPointer = screenUnderPointer,
            .otherScreens = otherScreens,
            .onHotkeyError = [this](platform::HotkeyError error) { reportQuickTerminalHotkeyError(error); },
            .onHotkeyBound = [this] { reportQuickTerminalHotkeyBound(); },
            .onOffScreen = [this] { quitIfNothingKeepsAppAlive(); },
            .onStateChanged = [this] { refreshWindowAttention(); },
            .isMenuOwned =
                [this](config::HotkeyChord const& chord) {
                    return display::isMenuOwnedChord(chord, _menuBarPlacement);
                },
            .cornerRadius = [this] { return resolvedChromeTokens().radii.quickTerminal; },
            .placement = placementControl(),
            // The hotkey is ignored until the launch's first window: its attach is not settled before.
            .isReady = [this] { return !isStartingUp(); } },
        _config.quickTerminal.value(),
        resolvedChromeTokens().motion);
    // The last ordinary window closing is Qt's to see on most hosts, but a quick terminal parked off screen
    // may still count as a visible window to it (a macOS panel is ordered out, not hidden): the app asks
    // for itself, with the quick terminal counted by its state.
    connect(&_sessionManager, &session::TerminalSessionManager::windowRemoved, this, [this] {
        platform::postToObject(this, [this] { quitIfNothingKeepsAppAlive(); });
    });
}

void ContourGuiApp::quitIfNothingKeepsAppAlive()
{
    // Suspended for a daemon restart, which closes every window and reopens them.
    if (!QGuiApplication::quitOnLastWindowClosed() || !_quickTerminal)
        return;
    auto const panelVisibility = window::quickTerminalKeepsAppAlive(_quickTerminal->state())
                                     ? window::WindowVisibility::Shown
                                     : window::WindowVisibility::Hidden;
    auto const facts =
        window::windowFactsOf(QGuiApplication::topLevelWindows(), _quickTerminal->window(), panelVisibility);
    if (!window::anyWindowKeepsAppAlive(facts))
        QCoreApplication::quit();
}

void ContourGuiApp::reportQuickTerminalHotkeyError(platform::HotkeyError error)
{
    errorLog()("{}", window::hotkeyUnavailableNoticeText(error));
    // Kept, because this usually runs at startup, before any window and before Settings exists: Settings
    // picks it up when it is built (settingsController()), and the first window posts the toast
    // (announceQuickTerminalHotkeyProblem()).
    _hotkeyRefusal.refused(error);
    showHotkeyProblemInSettings();
    announceQuickTerminalHotkeyProblem();
}

void ContourGuiApp::reportQuickTerminalHotkeyBound()
{
    _hotkeyRefusal.bound();
    showHotkeyProblemInSettings();
}

void ContourGuiApp::showHotkeyProblemInSettings()
{
    if (!_settingsController)
        return;
    // Beside the Settings field, which stays after a toast is gone (ruling Q2); empty clears it.
    auto const problem = _hotkeyRefusal.problem();
    auto const sentence = problem ? window::hotkeyRefusalText(*problem) : std::string_view {};
    _settingsController->setHotkeyProblem(
        QString::fromUtf8(sentence.data(), static_cast<qsizetype>(sentence.size())));
}

void ContourGuiApp::announceQuickTerminalHotkeyProblem()
{
    // Not taken while no window can show it, so the first window's call still finds it.
    auto* const controller = focusedController();
    if (controller == nullptr)
        return;
    if (auto const error = _hotkeyRefusal.takeUnannounced())
        controller->notices().post(window::Notice { .kind = window::NoticeKind::HotkeyUnavailable,
                                                    .text = window::hotkeyUnavailableNoticeText(*error) });
}

QWindow* ContourGuiApp::openQuickTerminal()
{
    if (!_qmlEngine)
        return nullptr;
    // One child context for every quick window: its `chromeStyle` is the Minimal one, and the window's own
    // controller finds `terminalSessions` and the rest in the root context behind it.
    if (!_quickContext)
    {
        auto* const root = _qmlEngine->rootContext();
        _quickContext = new QQmlContext(root, root);
        _quickChromeStyleHost =
            make_unique<window::ChromeStyleHost>(*_quickContext, quickTerminalChromeInputs());
    }
    // Consumed by the controller Main.qml mints in its Component.onCompleted, which runs inside create().
    _sessionManager.stageWindowRole(window::WindowRole::QuickTerminal);
    auto component = QQmlComponent(_qmlEngine.get(), "Contour.Ui", "Main");
    auto* const created = component.create(_quickContext);
    if (created == nullptr)
    {
        errorLog()("The quick terminal's window could not be created: {}",
                   component.errorString().toStdString());
        return nullptr;
    }
    auto* const quickWindow = qobject_cast<QWindow*>(created);
    if (quickWindow == nullptr)
    {
        delete created;
        return nullptr;
    }
    // Owned by the engine, so it goes with it; closed, which is what ending its last tab does, it goes at
    // once, and the controller sees it destroyed.
    created->setParent(_qmlEngine.get());
    if (auto* const quick = qobject_cast<QQuickWindow*>(quickWindow))
        connect(quick, &QQuickWindow::closing, quickWindow, &QObject::deleteLater);
    return quickWindow;
}

void ContourGuiApp::newWindow(QScreen* targetScreen)
{
    _pendingSpawnScreen = targetScreen;
    // The engine is always up when one window spawns another (the GUI is running); the guard makes
    // the stage/consume contract exercisable headlessly, where no QML engine exists.
    if (_qmlEngine)
    {
        _qmlEngine->loadFromModule("Contour.Ui", "Main");
        // The load is synchronous: a window it made took what was staged for it, and one that failed to
        // load must not leave it for whichever window opens next.
        _sessionManager.endWindowSpawn();
    }
}

QScreen* ContourGuiApp::takePendingSpawnScreen() noexcept
{
    auto* screen = _pendingSpawnScreen.data();
    _pendingSpawnScreen.clear();
    return screen;
}

window::DragEndProbe& ContourGuiApp::dragEndProbe()
{
    return *_dragEndProbe;
}

window::SettingsController& ContourGuiApp::settingsController()
{
    // Its collaborators: the app's live configuration, a side-file store rooted at the configuration
    // directory, a debounce timer, and a reload of every session after Settings wrote a side file. That
    // reload runs onConfigurationReloaded(), the one refresh of the page after a save as after a hand edit.
    if (!_settingsController)
    {
        _settingsController = std::make_unique<window::SettingsController>(
            [this]() -> config::Config const& { return _config; },
            window::SettingsControllerServices {
                .store = std::make_shared<config::FileGuiConfigStore>(_fileSystem,
                                                                      _config.configFile.parent_path()),
                .reload = [this]() -> std::expected<void, std::string> {
                    auto reloaded = _sessionManager.reloadAllSessions();
                    // The GUI chrome theme is re-applied by reloadAllSessions() itself. The window controls
                    // are QML bindings on their provider, so re-applying their style makes every window
                    // follow.
                    applyWindowControlStyle(_config.windowControlStyle.value());
                    return reloaded;
                },
                .gpuInventory = _gpuInventory,
                .fontFamilies = std::make_shared<platform::QtFontFamilySource>(),
                .scheduler = std::make_shared<window::QtSaveScheduler>(),
                // The Shell Integration page's Install rows go through the editor the pane's card uses.
                .shell = { .editor = _shellRcEditor.get(),
                           .contourBinary = [this] { return locatedContourBinary(); },
                           .host = _shellHostPlatform,
                           .homeDirectory = core::pathFromTrustedUtf8(QDir::homePath().toStdString()) },
                .menuBar = _menuBarPlacement,
                // A host page's Attach runs as the menu bar's commands do: in the window the app would use.
                .runAction =
                    [this](actions::Action const& action) {
                        runMenuBarAction(action, command::ActionScope::Application);
                    },
                .chooseFile = [](std::string const& title,
                                 std::filesystem::path const& start) -> std::optional<std::filesystem::path> {
                    auto const chosen = QFileDialog::getOpenFileName(
                        nullptr, QString::fromStdString(title), platform::toQString(start));
                    if (chosen.isEmpty())
                        return std::nullopt;
                    return std::filesystem::path { chosen.toStdWString() };
                },
            },
            _colorPreference);
        // A hotkey the desktop refused at startup, before this existed.
        showHotkeyProblemInSettings();
        QObject::connect(
            QCoreApplication::instance(), &QCoreApplication::aboutToQuit, this, &ContourGuiApp::flushForQuit);
    }
    return *_settingsController;
}

void ContourGuiApp::flushForQuit()
{
    if (_settingsController)
        _settingsController->flushPendingChanges();
}

window::SettingsWindowFactory ContourGuiApp::engineSettingsWindowFactory()
{
    return [this](window::SettingsWindowHost& host)
               -> std::expected<std::unique_ptr<QQuickWindow>, window::SettingsWindowError> {
        // A headless app builds no engine, so it has nothing to build on.
        if (!_qmlEngine)
            return std::unexpected { window::SettingsWindowError::Unavailable };
        return window::loadSettingsWindow(*_qmlEngine, host);
    };
}

std::shared_ptr<config::GuiStateStore> const& ContourGuiApp::guiState()
{
    if (!_guiState)
        _guiState =
            std::make_shared<config::FileGuiStateStore>(_fileSystem, _config.configFile.parent_path());
    return _guiState;
}

Qt::ColorScheme ContourGuiApp::effectiveColorScheme() const
{
    return _appliedGuiScheme.value_or(QGuiApplication::styleHints()->colorScheme());
}

window::SettingsWindowHost& ContourGuiApp::settingsWindow()
{
    if (_settingsWindow)
        return *_settingsWindow;
    // The Settings window keeps the OS's title bar (R-m), which follows the chrome's light or dark -- unless
    // the terminal window draws a Linux header bar, beside which it draws its own frame too (R-l).
    _settingsFrame = platform::makeNativeWindowFrame([this] { return effectiveColorScheme(); });
    auto const* const profile = _config.profile(profileName());
    auto const mainWindow = profile != nullptr && profile->showTitleBar.value()
                                ? platform::WindowDecoration::Server
                                : platform::WindowDecoration::Client;
    _settingsWindow = std::make_unique<window::SettingsWindowHost>(
        settingsController(),
        window::settingsWindowConventionsFor(_hostPlatform),
        _settingsWindowFactory,
        window::SettingsWindowDressing {
            .frame = *_settingsFrame,
            .material = titleBandMaterial(),
            .inputs =
                [this] {
                    return window::SettingsBackdropInputs {
                        .row = _chromeStyle,
                        .material = titleBandTokens(window::BarCentre::Empty).material,
                        .scheme = effectiveColorScheme(),
                    };
                },
            .decoration = window::settingsWindowDecoration(
                config::ChromeTokenRows[static_cast<size_t>(_chromeStyle)].layout.headerBar, mainWindow),
            .placement =
                [this] {
                    return windowControlPlacement(
                        resolvedWindowControlStyle(_config.windowControlStyle.value()));
                },
        },
        *guiState(),
        [this](config::Config const& preview, std::string const& profileName) {
            return sessionsManager().createPreviewSession(preview, profileName);
        },
        [this] {
            auto const url = QUrl::fromLocalFile(platform::toQString(_config.configFile));
            if (auto const opened = externalLauncher().openUrl(url); !opened)
                errorLog()("Could not open the configuration file \"{}\": {}.",
                           _config.configFile.string(),
                           platform::describe(opened.error()));
        },
        [this](std::string const& profileName) {
            // A command of the app's own, as the menu bar's: the tab opens in the window the app would
            // run one in, and without a window says so.
            runMenuBarAction(actions::CreateNewTab { .profileName = profileName },
                             command::ActionScope::Application);
        });
    return *_settingsWindow;
}

void ContourGuiApp::openSettings(QScreen* anchor)
{
    if (auto const shown = settingsWindow().show(anchor); !shown)
        errorLog()("Cannot open the Settings window. {}", shown.error());
}

// {{{ The Connect picker and `contour connect`
int ContourGuiApp::connectAction()
{
    if (auto const installed = installLogging("contour.connect", /*showProcessId=*/true); !installed)
    {
        cerr << std::format("contour connect: {}\n", installed.error());
        return EXIT_FAILURE;
    }
    auto const configPath = parameters().get<string>("contour.connect.config");
    auto const wantsList = parameters().get<bool>("contour.connect.list");
    auto const wantsJson = parameters().get<bool>("contour.connect.json");
    auto const targetText =
        parameters().verbatim.empty() ? string {} : string { parameters().verbatim.front() };

    if (wantsJson && !wantsList)
    {
        cerr << "contour connect: --json goes with --list\n";
        return EXIT_FAILURE;
    }

    // The saved hosts, which a bare name is looked up in and the list is made of.
    auto loaded = config::tryLoadConfigNamed(configPath);
    if (!loaded)
    {
        cerr << std::format("contour connect: {}\n", loaded.error());
        return EXIT_FAILURE;
    }

    if (wantsList)
    {
        auto const daemon = probeLocalDaemon(loaded->daemon.value().socket);
        auto const rows = cli::connectRows({ .hosts = loaded->hosts.value(), .daemon = daemon }, "");
        std::cout << cli::renderTargetList(rows, wantsJson ? cli::ListFormat::Json : cli::ListFormat::Text);
        return EXIT_SUCCESS;
    }

    // The windows this verb opens run under the terminal verb's surface, with this configuration.
    auto reparse = [&](std::vector<char const*> extra) {
        auto argv = std::vector<char const*> { "contour", "terminal" };
        if (!configPath.empty())
        {
            argv.push_back("config");
            argv.push_back(configPath.c_str());
        }
        argv.insert(argv.end(), extra.begin(), extra.end());
        return reparseParameters(static_cast<int>(argv.size()), argv.data());
    };

    if (targetText.empty())
    {
        // No target: the app opens on the picker alone, and choosing there starts another process.
        _connectLaunch = ConnectLaunch::Untargeted;
        if (!reparse({}))
            return EXIT_FAILURE;
        auto const rv = terminalGuiAction();
        _connectLaunch = ConnectLaunch::Targeted;
        return rv;
    }

    auto const target = config::parseLaunchTarget(targetText, loaded->hosts.value());
    if (!target)
    {
        cerr << std::format("contour connect: {}\n", target.error().describe());
        return EXIT_FAILURE;
    }
    auto const plan = config::attachPlanOf(*target);
    switch (plan.hosting)
    {
        case config::Hosting::InProcess:
            if (!reparse({ "daemon-mode", "in_process" }))
                return EXIT_FAILURE;
            return terminalGuiAction();
        case config::Hosting::LocalDaemon:
        case config::Hosting::RemoteHost: break;
    }

    // Every other target is what `contour client` is: the same request, filled from the plan.
    auto request = ClientRequest {};
    request.logFilter = parameters().get<string>("contour.connect.log");
    request.logFile = parameters().get<string>("contour.connect.log-file");
    request.config = configPath;
    request.label = plan.label; // Empty: the default label, which $CONTOUR_MUX outranks.
    return launchClient(std::move(request), plan.host);
}

std::expected<void, std::string> ContourGuiApp::bootFirstWindow()
{
    if (auto const opened = openFirstWindow(); !opened)
        return opened;
    _startupTimeline.mark(vthost::latency::StartupMark::WindowCreated);
    // The window that runs its shells itself, as the daemon could not be had, says so.
    if (!_hostingNotice.empty())
        if (auto const window = _sessionManager.focusedWindow())
            if (auto* controller = _sessionManager.controllerDisplaying(*window))
                controller->showHostingNotice(QString::fromStdString(_hostingNotice));

    // The hotkey the desktop refused while the app started, which no window existed to be told of.
    announceQuickTerminalHotkeyProblem();

    // Attach mode adopts the remaining remote sessions as tabs now that a
    // window exists to put them in.
    if (_onGuiBooted)
        _onGuiBooted();

    if (_latencyProbeRequest)
        armLatencyProbe();
    return {};
}

void ContourGuiApp::waitForAttach(remote::StartupAttachSteps steps)
{
    // The stand-in window closing between two prompts must not end the app: no terminal window exists yet.
    QGuiApplication::setQuitOnLastWindowClosed(false);
    _startupAttach = std::make_unique<remote::StartupAttach>(std::move(steps));
    _startupAttach->start();
}

void ContourGuiApp::startAttachWait()
{
    Require(_startupAttachPlan.has_value() || _deferredNativeAttach.has_value());
    // The plan is read when looked at, not taken now: a launch that names a workspace makes it only once the
    // listing has answered (beginDeferredAttach()), and the wait looks at nothing before.
    auto const linkOfPlan = [this]() -> remote::DaemonLink* {
        return _startupAttachPlan ? daemonLink(_startupAttachPlan->link) : nullptr;
    };
    waitForAttach(remote::StartupAttachSteps {
        .look = [this, linkOfPlan]() -> std::optional<std::expected<void, std::string>> {
            auto* const link = linkOfPlan();
            if (link == nullptr)
                return std::unexpected(std::string { "the attach was given up" });
            return pollNativeAttach(*link, _startupAttachPlan->budget);
        },
        .untilDeadline = [this, linkOfPlan]() -> std::optional<std::chrono::milliseconds> {
            auto* const link = linkOfPlan();
            auto* const controller = link != nullptr ? link->liveController() : nullptr;
            return controller != nullptr ? controller->untilAttachDeadline(_startupAttachPlan->budget)
                                         : std::nullopt;
        },
        .openFirstWindow =
            [this](std::expected<void, std::string> attached) -> std::expected<void, std::string> {
            _startupAttachPlan.reset();
            // Once it is over, a link that ended meanwhile is let go of, by a call of its own: not here, as
            // the first window goes on to use the link it waited for.
            _awaitedLinkId.reset();
            QMetaObject::invokeMethod(this, [this] { destroyEndedLinks(); }, Qt::QueuedConnection);
            if (auto const settled = settleHosting(attached); !settled)
                return settled;
            auto booted = bootFirstWindow();
            if (booted)
                QGuiApplication::setQuitOnLastWindowClosed(true);
            return booted;
        },
        .end =
            [this](std::string const& reason, int exitCode) {
                // As a launch that cannot attach has always ended: its reason on stderr, and a failure
                // status.
                cerr << reason << '\n';
                _startupExitCode = exitCode;
                _exitEventLoop(exitCode);
            },
        .prepare =
            _deferredNativeAttach
                ? std::function<void(remote::StartupAttachReady)> { [this](remote::StartupAttachReady ready) {
                      beginDeferredAttach(std::move(ready));
                  } }
                : nullptr,
    });
    if (_startupAttachPlan)
        followStartupLink();
}

void ContourGuiApp::followStartupLink()
{
    Require(_startupAttachPlan.has_value());
    // The event loop runs while the attach goes on, so a queued destroyEndedLinks() can run in it: it must
    // leave the link that is being waited for alone.
    _awaitedLinkId = _startupAttachPlan->link;
    // What the attach has to say comes from the reactor: looked at again on each piece of news.
    if (auto* const link = daemonLink(_startupAttachPlan->link);
        link != nullptr && link->liveController() != nullptr && _startupAttach)
        connect(link->liveController(),
                &remote::NativeController::attachProgressed,
                _startupAttach.get(),
                &remote::StartupAttach::progressed);
}

std::expected<void, std::string> ContourGuiApp::openFirstWindow()
{
    // Ask where the sessions should live first, or open the window at once.
    if (firstWindow() == FirstWindow::ConnectPicker)
    {
        openConnectPicker(PickerPresentation::Launcher, nullptr);
        // No terminal window instead: hosting was left to the picker, so one opened here would run its shells
        // in-process whatever `daemon.mode` says.
        if (!_connectPickerWindow)
            return std::unexpected { "contour: the Connect picker could not be opened" };
        return {};
    }
    auto const timer = crispy::ScopedTimer(startupLog, "newWindow (QML load)");
    newWindow();
    return {};
}

ContourGuiApp::FirstWindow ContourGuiApp::firstWindow() const
{
    if (_connectLaunch == ConnectLaunch::Untargeted)
        return FirstWindow::ConnectPicker;
    // A plain launch carries no arguments at all, which is all a Dock, Spotlight or launcher start can say.
    auto const isPlainLaunch = _argc <= 1 && !_clientRequest && !_cliCommand;
    return isPlainLaunch && _config.connectPickerOnStart.value() ? FirstWindow::ConnectPicker
                                                                 : FirstWindow::Terminal;
}

std::function<std::expected<void, std::string>()> ContourGuiApp::prepareCheck(
    config::LaunchTarget const& target)
{
    switch (target.kind)
    {
        // The daemon starts when the window connects, and this window needs nothing: neither can fail ahead.
        case config::LaunchTargetKind::LocalDaemon:
        case config::LaunchTargetKind::InProcess:
            return [] {
                return std::expected<void, std::string> {};
            };
        case config::LaunchTargetKind::SavedHost:
        case config::LaunchTargetKind::Address: break;
    }
    auto const& saved = _config.hosts.value();
    auto const found = saved.find(target.name);
    auto opened = found != saved.end()
                      ? transportOf(target.name, found->second, askpass::AnyWindow, cli::SshPrompts::Batch)
                      : openHost(core::platform::normalizePath(_config.configFile),
                                 target.name,
                                 askpass::AnyWindow,
                                 cli::SshPrompts::Batch);
    if (!opened)
        return [reason = opened.error()]() -> std::expected<void, std::string> {
            return std::unexpected { reason };
        };
    return [transport = std::make_shared<cli::HostTransport>(std::move(opened->transport))]() {
        return cli::checkHostReachable(*cli::makeHostControl(transport->factory, transport->attach));
    };
}

std::expected<void, std::string> ContourGuiApp::startProcess(std::vector<std::string> const& arguments)
{
    auto list = QStringList {};
    for (auto const& argument: arguments)
        list.push_back(QString::fromStdString(argument));
    return externalLauncher()
        .runDetached(QString::fromStdString(programPath()), list)
        .transform_error(
            [](platform::SpawnError const& error) { return std::string { platform::describe(error) }; });
}

std::expected<void, std::string> ContourGuiApp::launchConnect(config::LaunchTarget const& target)
{
    return startProcess(config::connectArguments(target, core::platform::normalizePath(_config.configFile)));
}

cli::LocalDaemonState ContourGuiApp::probeLocalDaemon(std::string const& socketOption)
{
    auto const spawner = vthost::makeNativeDaemonSpawner();
    return cli::probeLocalDaemon(
        *spawner, vthost::muxSocketPath({}, socketOption), cli::nativeSessionCounter());
}

void ContourGuiApp::refreshLauncherMenu()
{
    if (!_launcherMenu)
    {
        _launcherMenu =
            platform::makeLauncherMenu(programPath(), [this](command::LauncherEntry const& entry) {
                if (auto const started = startProcess(entry.arguments); !started)
                    errorLog()("Could not open \"{}\": {}.", entry.title, started.error());
            });
        if (!_launcherMenu)
            return;
    }
    auto const entries = command::launcherEntries(
        _launcherMenu->surface(), _config.hosts.value(), core::platform::normalizePath(_config.configFile));
    _launcherMenu->set(entries);
}

std::shared_ptr<cli::IDaemonDiscovery> ContourGuiApp::localDaemonDiscovery()
{
    if (!_localDaemonDiscovery)
    {
        _daemonSpawner = vthost::makeNativeDaemonSpawner();
        _localDaemonDiscovery = cli::makeLocalDaemonDiscovery(
            vthost::labelSocketDirectory(), *_daemonSpawner, cli::nativeWorkspaceCounter());
    }
    return _localDaemonDiscovery;
}

std::unique_ptr<session::SwitchDirectory> ContourGuiApp::makeSwitchDirectory()
{
    return std::make_unique<remote::DaemonSwitchDirectory>(
        localDaemonDiscovery(), remote::makeControlWorkspaceLister(SwitchListingTimeout), [this] {
            return linkedDaemons();
        });
}

std::vector<remote::LinkedDaemon> ContourGuiApp::linkedDaemons()
{
    auto daemons = std::vector<remote::LinkedDaemon> {};
    for (auto const& link: _daemonLinks)
    {
        // A link still connecting, given up, stopped or ended is on no daemon a window here shows.
        if (!link->isAttached())
            continue;
        auto daemon = remote::LinkedDaemon {
            .link = link->id(),
            .address = link->address(),
            .transport = link->siblingConfig().controller.transport,
            .windows = {},
        };
        for (auto const window: _sessionManager.windowsBoundTo(*link))
            if (auto const workspace = link->workspaceOf(window))
                daemon.windows.push_back(
                    remote::WindowOnDaemon { .window = window, .workspace = *workspace });
        daemons.push_back(std::move(daemon));
    }
    return daemons;
}

window::ConnectPickerServices ContourGuiApp::connectPickerServices()
{
    return window::ConnectPickerServices {
        .config = [this]() -> config::Config const& { return _config; },
        .daemon =
            [this] {
                // The socket is read here, on the GUI thread; the job holds a copy.
                return [socket = _config.daemon.value().socket] {
                    return probeLocalDaemon(socket);
                };
            },
        .discovery = localDaemonDiscovery(),
        .check = [this](config::LaunchTarget const& target) { return prepareCheck(target); },
        .launch = [this](config::LaunchTarget const& target) { return launchConnect(target); },
        .showSettingsPage = [this](std::string const& page) { openSettingsPage(page, nullptr); },
        .setStartPrompt =
            [this](window::StartPrompt prompt) {
                if (auto const set =
                        settingsController().setGlobalField(QStringLiteral("connect_picker_on_start"),
                                                            QVariant { prompt == window::StartPrompt::Ask });
                    !set)
                    errorLog()("Could not store the Connect picker's start-up switch.");
            },
        .state = *guiState(),
        .quickKey = _hostPlatform == config::HostPlatform::MacOS ? cli::QuickSelectKey::Command
                                                                 : cli::QuickSelectKey::Alt,
    };
}

void ContourGuiApp::openConnectPicker(PickerPresentation presentation, QScreen* anchor)
{
    if (!_qmlEngine)
        return;
    if (!_connectPicker)
    {
        _connectPicker = std::make_unique<window::ConnectPickerController>(connectPickerServices());
        _connectPickerWindow = window::loadConnectPickerWindow(*_qmlEngine, *_connectPicker);
        if (!_connectPickerWindow)
        {
            _connectPicker.reset();
            return;
        }
        // Cancelling closes the window, and Qt ends the app when it was the last one; a connection that was
        // started ends a launcher's process, which has nothing left to show, and only closes a sheet.
        connect(_connectPicker.get(), &window::ConnectPickerController::cancelled, this, [this] {
            if (_connectPickerWindow)
                _connectPickerWindow->close();
        });
        connect(_connectPicker.get(), &window::ConnectPickerController::connected, this, [this] {
            if (_connectPickerPresentation == PickerPresentation::Launcher)
                QCoreApplication::quit();
            else if (_connectPickerWindow)
                _connectPickerWindow->close();
        });
        connect(_connectPickerWindow.get(), &QWindow::visibleChanged, this, [this](bool isVisible) {
            if (!isVisible)
                returnFocusTo(_connectPickerReturn);
        });
    }
    else
        _connectPicker->refresh();

    _connectPickerPresentation = presentation;
    _connectPickerReturn = focusReturnPoint();
    switch (presentation)
    {
        case PickerPresentation::Launcher:
            _connectPickerWindow->setTransientParent(nullptr);
            _connectPickerWindow->setModality(Qt::NonModal);
            break;
        case PickerPresentation::Sheet:
            _connectPickerWindow->setTransientParent(QGuiApplication::focusWindow());
            _connectPickerWindow->setModality(
                _connectPickerWindow->transientParent() != nullptr ? Qt::WindowModal : Qt::NonModal);
            break;
    }
    if (anchor != nullptr)
        _connectPickerWindow->setScreen(anchor);
    _connectPickerWindow->show();
    _connectPickerWindow->raise();
    _connectPickerWindow->requestActivate();
}

window::SwitcherServices ContourGuiApp::switcherServices()
{
    return window::SwitcherServices {
        .discovery = localDaemonDiscovery(),
        .savedHosts =
            [this] {
                auto hosts = std::vector<window::SwitcherDaemon> {};
                auto const shownHere = _sessionManager.switchDirectory().hostsShownHere();
                for (auto const& [name, entry]: _config.hosts.value())
                    hosts.push_back(window::SwitcherDaemon {
                        .address = { .kind = vthost::proto::SwitchDaemonKind::Host,
                                     .daemon = name,
                                     .workspace = {} },
                        .workspaceCount = std::nullopt,
                        .tag = cli::schemeTag(entry.url.scheme),
                        .expansion = window::SwitcherExpansion::Collapsed,
                        .listing = window::SwitcherListing::NotListed,
                        .failure = {},
                        .workspaces = {},
                        .link = window::switcherHostLinkOf(name, _hostsReached, shownHere),
                    });
                return hosts;
            },
        .list =
            [this](vtworkspace::WorkspaceId window,
                   session::SwitchTarget const& daemon,
                   window::SwitcherListed done) {
                _windowSwitching->listWorkspaces(
                    window, daemon, [this, window, daemon, done = std::move(done)](auto listed) {
                        // A saved host that answered a listing was connected to, whatever the popup does
                        // next.
                        if (listed && daemon.kind == vthost::proto::SwitchDaemonKind::Host)
                            _hostsReached.insert(daemon.daemon);
                        done(listed.transform([window](std::vector<remote::WorkspaceRow> const& rows) {
                            return remote::switcherWorkspacesOf(rows, window);
                        }));
                    });
            },
        .request =
            [this](vtworkspace::WorkspaceId window, session::SwitchTarget target, session::SwitchMode mode) {
                _windowSwitching->request(
                    window,
                    std::move(target),
                    mode,
                    session::NoLandingDeadline,
                    [](std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome) {
                        if (!outcome)
                            attachLog()("switch from the switcher failed: {}", outcome.error());
                    });
            },
        .startDaemon =
            [this](std::string const& label) {
                // Read here, on the GUI thread; the job holds copies, and a start of its own, so it outlives
                // nothing it uses.
                auto const socket =
                    vthost::muxSocketPath(vthost::socketLabelOf(label),
                                          cli::resolveSocketOption("", _config.daemon.value().socket));
                auto options = vthost::DaemonSpawnOptions {
                    .filter = {},
                    .logFile = {},
                    .configPath = core::platform::normalizePath(_config.configFile),
                    .profileName = {},
                    .sizePolicy = _config.daemon.value().sizePolicy,
                };
                return [start = std::make_shared<LocalDaemonStart>(
                            processEnvironment(), fileSystem(), programPath(), _daemonEnvironment),
                        placement = _processPlacement,
                        socket,
                        options = std::move(options)] {
                    return cli::startNamedDaemon(socket,
                                                 { .spawner = *start->spawner,
                                                   .binary = *start->binary,
                                                   .placement = *placement,
                                                   .options = options,
                                                   .timeout = NamedDaemonStartTimeout });
                };
            },
    };
}

ContourGuiApp::FocusReturn ContourGuiApp::focusReturnPoint() const
{
    return FocusReturn { .window = QGuiApplication::focusWindow(),
                         .workspace = _sessionManager.focusedWindow() };
}

void ContourGuiApp::returnFocusTo(FocusReturn const& target)
{
    if (target.window)
        target.window->requestActivate();
    if (!target.workspace)
        return;
    if (auto* const controller = _sessionManager.controllerDisplaying(*target.workspace))
        controller->focusActiveDisplay();
}

std::expected<void, std::string> ContourGuiApp::openSwitcher(window::SwitcherOpening opening, QScreen* anchor)
{
    if (!_qmlEngine)
        return std::unexpected(std::string { "there is no window to show the switcher in" });
    if (!_switcher)
    {
        _switcher = std::make_unique<window::SwitcherController>(switcherServices());
        _switcherWindow = window::loadSwitcherWindow(*_qmlEngine, *_switcher);
        if (!_switcherWindow)
        {
            _switcher.reset();
            return std::unexpected(std::string { "the switcher popup could not be loaded" });
        }
        // Both a choice and a cancel close it; the choice was handed to the window switcher already.
        connect(_switcher.get(), &window::SwitcherController::finished, this, [this] {
            if (_switcherWindow)
                _switcherWindow->close();
        });
        connect(_switcher.get(), &window::SwitcherController::cancelled, this, [this] {
            if (_switcherWindow)
                _switcherWindow->close();
        });
        connect(_switcherWindow.get(), &QWindow::visibleChanged, this, [this](bool isVisible) {
            if (!isVisible)
                returnFocusTo(_switcherReturn);
        });
    }
    _switcherReturn = focusReturnPoint();
    _switcher->open(std::move(opening));
    _switcherWindow->setTransientParent(QGuiApplication::focusWindow());
    _switcherWindow->setModality(_switcherWindow->transientParent() != nullptr ? Qt::WindowModal
                                                                               : Qt::NonModal);
    if (anchor != nullptr)
        _switcherWindow->setScreen(anchor);
    _switcherWindow->show();
    _switcherWindow->raise();
    _switcherWindow->requestActivate();
    return {};
}

std::expected<void, std::string> ContourGuiApp::showSwitchPick(remote::SwitchPickRequest const& pick)
{
    auto const daemon = _sessionManager.daemonTargetOf(pick.window);
    if (!daemon)
        return std::unexpected(std::string { "the window went away" });
    auto const* const controller = _sessionManager.controllerDisplaying(pick.window);
    return openSwitcher(
        window::SwitcherOpening {
            .window = pick.window,
            .windowDaemon = *daemon,
            .mode = pick.mode,
            .prompt = window::SwitcherPrompt::Browse,
            .pick =
                window::SwitcherDaemon {
                    .address = { .kind = pick.target.kind, .daemon = pick.target.daemon, .workspace = {} },
                    .workspaceCount = pick.workspaces.size(),
                    .tag = {},
                    .expansion = window::SwitcherExpansion::Expanded,
                    .listing = window::SwitcherListing::Listed,
                    .failure = {},
                    .workspaces = remote::switcherWorkspacesOf(pick.workspaces, pick.window),
                },
        },
        controller != nullptr ? controller->screen() : nullptr);
}

void ContourGuiApp::openSettingsPage(std::string const& page, QScreen* anchor)
{
    settingsWindow().requestPage(QString::fromStdString(page));
    openSettings(anchor);
}
// }}}

display::ForcedFontDpiProvider* ContourGuiApp::forcedFontDpiProvider()
{
    // Lazy: the provider inspects QGuiApplication::platformName(), so it must not be constructed before
    // the Qt application. First use is a display's setSession(), which is well past that point.
    if (!_forcedFontDpiProvider && QCoreApplication::instance() != nullptr)
        _forcedFontDpiProvider = std::make_unique<display::ForcedFontDpiProvider>();
    return _forcedFontDpiProvider.get();
}

} // namespace contour
