// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/cli/ContourApp.hpp>
#include <contour/cli/HostTransport.hpp>
#include <contour/command/CommandHistoryStore.hpp>
#include <contour/command/MenuBarTable.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/DesktopButtonLayout.hpp>
#include <contour/config/LayoutStore.hpp>
#include <contour/config/WindowControlStyle.hpp>
#include <contour/display/GpuSelection.hpp>
#include <contour/display/Logging.hpp>
#include <contour/platform/AppearanceServices.hpp>
#include <contour/platform/ExternalLauncher.hpp>
#include <contour/platform/ShellRcEditor.hpp>
#include <contour/platform/SpeechSynthesizer.hpp>
#include <contour/platform/TaskbarProgressSink.hpp>
#include <contour/session/CommandOutputFiles.hpp>
#include <contour/session/ExitCode.hpp>
#include <contour/session/RestoreSummaryPolicy.hpp>
#include <contour/session/SessionFactory.hpp>
#include <contour/session/TerminalSessionManager.hpp>
#include <contour/window/DaemonDriftModel.hpp>
#include <contour/window/HotkeyRefusals.hpp>
#include <contour/window/SettingsWindowHost.hpp>
#include <contour/window/ShellIntegrationOffer.hpp>
#include <contour/window/SplitChrome.hpp>
#include <contour/window/TitleBandGeometry.hpp>
#include <contour/window/TitleBandLayout.hpp>
#include <contour/window/UiStyleProvider.hpp>
#include <contour/window/WindowPositionStore.hpp>

#include <vtbackend/core/TerminalContext.hpp>
#include <vtbackend/screen/GitHead.hpp>
#include <vtbackend/screen/LinkDetector.hpp>
#include <vtbackend/screen/StatusLineCommandCache.hpp>
#include <vtbackend/screen/TerminalClocks.hpp>

#include <vtpty/CommandCapture.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/SshSession.hpp>

#include <core/async/StopToken.hpp>
#include <core/async/ThreadPoolExecutor.hpp>
#include <core/platform/Clock.hpp>
#include <core/platform/FileSystem.hpp>

#include <QtCore/QPointer>
#include <QtGui/QPalette>
#include <QtQml/QQmlApplicationEngine>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <vthost/DaemonSpawn.hpp>
#include <vthost/latency/KeystrokeProbe.hpp>
#include <vthost/latency/StartupTimeline.hpp>

class QQmlContext;
class QQuickWindow;
class QScreen;
class QWindow;

namespace vtextension
{
class ExtensionRuntime;
class IGrantStore;
} // namespace vtextension

namespace vtextension::wasm
{
class IWasmBackend;
} // namespace vtextension::wasm

namespace contour
{

namespace cli
{
    struct LocalDaemonState;
}

namespace config
{
    struct Config;
    class GuiStateStore;
    struct LaunchTarget;
} // namespace config

namespace display
{
    class FirstFrameWatchdog;
    class ForcedFontDpiProvider;
    class GraphicsDeviceSelector;
    class IGpuInventory;
    class LatencyProbeDriver;
    enum class SwapPacing : uint8_t;
} // namespace display

namespace platform
{
    class IAccessibilityPreferenceSource;
    class IMenuBarIntegration;
    class INativeMenuPresenter;
    class IPlatformAppearance;
    class ISecureInput;
    class LauncherMenu;
    class Notifier;
} // namespace platform

namespace remote
{
    class NativeController;
    class RoutingSessionFactory;
    class TmuxController;
} // namespace remote

namespace session
{
    class TerminalSession;
} // namespace session

namespace window
{
    class AppMenuBar;
    struct AppMenuBarSources;
    class ChromeStyleHost;
    class ConnectPickerController;
    struct ConnectPickerServices;
    class NewDefaultsNotice;
    class QuickTerminalController;
    class SettingsController;
    class WindowControlStyleProvider;
    class WindowController;
} // namespace window

/// What the shell-integration offer reads from outside the app. Every member defaults to what production
/// uses, so a test names only what it replaces.
struct ShellIntegrationServices
{
    /// Reads and appends to the shells' startup files. Null (the default) wires a
    /// platform::FileShellRcEditor over the app's file system, environment and host.
    std::unique_ptr<platform::IShellRcEditor> editor {};

    /// What the GUI remembers, shared with the new-defaults banner and the Settings window. Null (the
    /// default) is a config::FileGuiStateStore in the configuration directory, made by guiState() on first
    /// use.
    std::shared_ptr<config::GuiStateStore> state {};

    /// The running binary, which the line added to an rc file runs. Empty (the default) is
    /// `QCoreApplication::applicationFilePath()`.
    std::filesystem::path contourBinary {};

    /// The remote host the app is attached to, for a test of a remote-hosted window. Empty (the default and
    /// production) until awaitNativeAttach() fills in attachedHostLabel().
    std::string attachedHostLabel {};

    /// The host the offer and the Install rows decide on (installBlockOf()). Empty (the default and
    /// production) is the detected host; a test sets it so the Linux behaviour and the Windows refusal both
    /// run on every machine. The chrome keeps using the detected host.
    std::optional<config::HostPlatform> host {};
};

/// The daemon window the boot (first) OS window should adopt when attaching, rather than authoring
/// a fresh tab. In attach mode the boot window mirrors the daemon's primary (lowest-id) window; a
/// later OS window is not a boot window (@p anyWindowMapped is then true). Pure so the boot-adoption
/// rule is unit-testable without the QML boot flow.
/// @param anyWindowMapped True if at least one OS window is already bound to a daemon window.
/// @param daemonWindowIds The daemon's known window ids, ascending (windowIds() ordering).
/// @return The daemon window to adopt, or nullopt if this is not the boot window, or the daemon has
///         not reported a window yet.
[[nodiscard]] std::optional<std::uint64_t> primaryDaemonWindowToAdopt(
    bool anyWindowMapped, std::vector<std::uint64_t> const& daemonWindowIds);

/// Whether @p configHome holds QML overrides in the pre-module layout, which nothing loads.
///
/// The loader that shipped with the loose qrc files probed `<configHome>/ui/<Name>.qml` file by file.
/// The QML module that replaced it is overridden the way any module is: the config directory joins
/// the import path, and a `<configHome>/Contour/Ui` module with its own qmldir shadows the built-in
/// one. A user who had customized a component before that change therefore sees the customization
/// simply stop applying, and neither the old location nor the new one is guessable from the other --
/// so this is what lets the boot path say so out loud. @see ContourGuiApp::terminalGuiAction.
///
/// @param configHome The configuration directory, i.e. what becomes the QML import path.
/// @return @c true when the legacy directory holds at least one .qml file AND no module override
///         exists, which is exactly the case where the user has lost something without being told.
[[nodiscard]] bool hasStrandedQmlOverrides(std::filesystem::path const& configHome);

/// The directory a user's own `Contour.Ui` module is loaded from, under @p configHome.
[[nodiscard]] std::filesystem::path uiModuleOverrideDirectory(std::filesystem::path const& configHome);

/// What the leading token of a verbatim `contour [terminal] PROGRAM ARGS...` invocation names.
enum class VerbatimCommandKind : uint8_t
{
    Unknown,          ///< Nothing under that name: not on disk, and not on PATH either.
    Program,          ///< An executable to run instead of the configured shell.
    WorkingDirectory, ///< A directory to open the configured shell in.
};

/// What classifyVerbatimCommand() made of the token.
struct VerbatimCommand
{
    VerbatimCommandKind kind; ///< What the token names.
    /// Where to open the configured shell, for WorkingDirectory; empty for the other two, whose
    /// token the caller runs as the program exactly as it was typed.
    std::filesystem::path workingDirectory;
};

/// Whether a token names something this machine can actually run -- the PATH search the spawn
/// itself performs. Injected so a verdict does not depend on what happens to be installed on the
/// machine running the tests. @see contour::platform::isReachableProgram.
using ProgramReachablePredicate = bool (*)(std::string_view);

/// Decides what the leading token of `contour [terminal] PROGRAM ARGS...` asks for. Pure but for
/// @p isReachable and the file-system probes, so the rule is unit-testable against a temporary
/// directory and a stub PATH.
///
/// A token naming an executable file, or a program the machine can reach, is a program to run. A
/// directory is a place to open the shell in, and so is a plain (non-executable) file -- its parent
/// directory. Anything else is Unknown: the caller has nothing better to do than run it and let the
/// spawn fail, but the distinction is what keeps `contour terminal echo hello` from being reported
/// as a mistake.
///
/// @param token       The first verbatim argument, as typed.
/// @param isReachable The reachability test to use.
/// @return What the token names, and where to start the shell if that is what it names.
[[nodiscard]] VerbatimCommand classifyVerbatimCommand(std::string_view token,
                                                      ProgramReachablePredicate isReachable);

/// Whether a local daemon may be spawned yet. @see ContourGuiApp::_daemonSpawnGate
enum class DaemonSpawnGate : std::uint8_t
{
    Closed = 0, ///< The GUI is still constructing its QApplication.
    Open,       ///< Spawning is safe.
};

/// Extends ContourApp with terminal GUI capability.
class ContourGuiApp: public QObject, public cli::ContourApp
{
    Q_OBJECT

  public:
    /// @param sessionFactory The PTY factory for new sessions. The app is the composition root:
    ///                       null (the default) wires the production AppSessionFactory; tests pass
    ///                       an in-memory factory (e.g. MockPty-backed) to run session-creation
    ///                       paths headlessly.
    /// @param externalLauncher Opens URLs / spawns child processes for a session. Null (the default)
    ///                       wires whatever makeExternalLauncher() offers, placing what it starts as the
    ///                       sessions are placed; tests pass a recording launcher to assert
    ///                       open-document / follow-hyperlink / spawn routing without actually
    ///                       launching anything.
    /// @param layoutStore    Persistence for named layouts (SaveLayout). Null (the default) wires the
    ///                       production FileLayoutStore (an atomically-replaced `layouts.yml`); tests
    ///                       pass an in-memory store to drive SaveLayout without touching the disk.
    /// @param commandHistoryStore Persistence for the command palette's most-recently-used list. Null
    ///                       (the default) wires the production FileCommandHistoryStore (an atomically-
    ///                       replaced `command-history.yml`); tests pass an in-memory store to drive the
    ///                       record -> persist -> reload cycle without touching the disk.
    /// @param speechSynthesizer The voice every session in this app reads selections through. Null
    ///                       (the default) wires the production one; tests pass a
    ///                       NullSpeechSynthesizer, which keeps the suite off the platform's speech
    ///                       service (on Linux that is a speech-dispatcher connection per use).
    /// @param commandOutputFiles Where OpenCommandOutput writes the files a pager pane shows. Null (the
    ///                       default) wires the native filesystem under $XDG_RUNTIME_DIR or the system
    ///                       temporary directory; tests pass one over an in-memory filesystem.
    /// @param terminalClocks The clocks every session's terminal reads. Borrowed: they must outlive the
    ///                       app. The default is the system's; tests pass manual ones to drive time.
    /// @param hostIdentity   This machine, as every session judges a working directory's or a link's
    ///                       locality by it. Null (the default) reads it once, here: the machine id and
    ///                       the host name. Tests pass a fixed one, so what counts as this machine does
    ///                       not depend on the machine they run on.
    /// @param fileSystem     Where every session looks up the paths it shows and opens. Borrowed: it must
    ///                       outlive the app. Null (the default) is this machine's; tests pass an in-memory
    ///                       one, so that what a path names does not depend on the machine they run on.
    /// @param processPlacement Places every process a session starts -- its shell, the programs it starts,
    ///                       the daemon and the tmux client `contour client` spawns -- in a resource domain
    ///                       of its own. Null (the default) wires vtpty::makeDefaultProcessPlacement();
    ///                       tests pass a fake to see what is placed.
    /// @param linkServices   What every session's link detection runs on. Injected by tests (manual
    ///                       executors); its filesystem must be the app's, @p fileSystem or this machine's,
    ///                       which the constructor requires. Absent (the default), the app's own pool,
    ///                       probe thread and fileSystem(). Borrowed: it must outlive the app.
    /// @param windowPositionStore Last-window location persistence; null uses the atomic file adapter.
    /// @param env            The process environment every part of the application reads through.
    /// @param taskbarProgress Native taskbar adapter; null wires the production one.
    /// @param appearanceServices Builds the OS accessibility and appearance sources the chrome follows.
    ///                     The default is the native ones; tests pass platform::nullAppearanceServices()
    ///                     so they never read the host's settings.
    /// @param processStart When the process began, sampled first thing in main(): the origin of the
    ///                     startup timeline. The default is the moment of construction, for tests.
    /// @param settingsWindowFactory Builds the Settings window on its first show. Null (the default) loads
    ///                     SettingsWindow on the app's own QML engine, and gives Unavailable while there
    ///                     is none; tests pass one over an offscreen engine of their own.
    /// @param shellIntegration What the offer to install shell integration reads and writes; see
    ///                     ShellIntegrationServices. Tests pass a recording editor and an in-memory state.
    explicit ContourGuiApp(
        core::Environment const& env,
        std::unique_ptr<session::SessionFactory> sessionFactory = nullptr,
        std::unique_ptr<platform::ExternalLauncher> externalLauncher = nullptr,
        std::unique_ptr<config::LayoutStore> layoutStore = nullptr,
        std::unique_ptr<command::CommandHistoryStore> commandHistoryStore = nullptr,
        std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer = nullptr,
        std::unique_ptr<session::CommandOutputFiles> commandOutputFiles = nullptr,
        vtbackend::TerminalClocks terminalClocks = vtbackend::TerminalClocks::system(),
        std::optional<vtbackend::HostIdentity> hostIdentity = std::nullopt,
        core::platform::FileSystem const* fileSystem = nullptr,
        std::shared_ptr<vtpty::ProcessPlacement> processPlacement = nullptr,
        std::optional<vtbackend::LinkDetectionServices> linkServices = std::nullopt,
        std::unique_ptr<platform::TaskbarProgressSink> taskbarProgress = nullptr,
        std::unique_ptr<window::WindowPositionStore> windowPositionStore = nullptr,
        platform::AppearanceServices const& appearanceServices = {},
        core::platform::SteadyTimePoint processStart = core::platform::defaultSteadyClock().now(),
        window::SettingsWindowFactory settingsWindowFactory = nullptr,
        ShellIntegrationServices shellIntegration = {});
    ~ContourGuiApp() override;

    /// The shared persistence service for normal-window locations.
    [[nodiscard]] window::WindowPositionStore& windowPositionStore() noexcept
    {
        return *_windowPositionStore;
    }

    static ContourGuiApp* instance() { return static_cast<ContourGuiApp*>(cli::ContourApp::instance()); }

    int run(int argc, char const* argv[]) override;
    [[nodiscard]] core::cli::Command parameterDefinition() const override;

    /// Builds the quick terminal controller, which registers the configured hotkey. Called once the QML
    /// engine exists; a second call does nothing.
    void startQuickTerminal();

    /// Tells the user the quick terminal's hotkey could not be registered: logged, kept for Settings, and
    /// shown as a toast in the focused window when there is one, else in the first window that opens.
    /// @param error Why not.
    void reportQuickTerminalHotkeyError(platform::HotkeyError error);

    /// Quits when no window keeps the app running: the quick terminal keeps it only while it is on screen
    /// (ruling D1), so hiding it after the last ordinary window closed ends the process.
    void quitIfNothingKeepsAppAlive();

    /// The quick terminal's hotkey is registered: the problem shown for an earlier refusal goes.
    void reportQuickTerminalHotkeyBound();

    /// Hands the stored hotkey problem (or its absence) to Settings, when it exists.
    void showHotkeyProblemInSettings();

    /// Posts the toast for a hotkey refusal not announced yet, when a window can show it.
    void announceQuickTerminalHotkeyProblem();

    /// Opens a new OS window (loads a fresh Main.qml root).
    /// @param targetScreen The screen the new window should open on (the spawning window's screen),
    ///                     or nullptr when unknown (first window); consumed by the window's
    ///                     WindowController::bindWindow() via takePendingSpawnScreen().
    void newWindow(QScreen* targetScreen = nullptr);

    /// Creates the quick terminal's window: a Main.qml root of the QuickTerminal role, in a child context
    /// whose `chromeStyle` is the Minimal-density one, with its first tab. The window is not shown; the quick
    /// terminal controller shows it. Needs the QML engine, so a headless app returns null.
    /// @return The window, owned by the engine; null when it could not be made.
    [[nodiscard]] QWindow* openQuickTerminal();

    /// Consumes the pending spawn target screen staged by newWindow().
    /// @return The staged screen, or nullptr (already consumed / never staged / screen unplugged).
    [[nodiscard]] QScreen* takePendingSpawnScreen() noexcept;

    /// Attach mode: authors a new window on the daemon (routed through the session factory's
    /// requestRemoteWindow) instead of opening a purely local one. The daemon's new-window layout push
    /// then spawns and binds the matching OS window (B4). Called by the NewTerminalWindow action.
    /// @param spawn What the new window's first session runs, as the daemon is asked for it.
    /// @return true if the request was routed to the daemon (do NOT open a local window); false for a
    ///         local factory.
    [[nodiscard]] bool requestRemoteWindow(session::RemoteSpawn const& spawn);

    /// The app-wide forced-font-DPI provider (single instance; see display/ContentScale.h), created
    /// lazily on first use (requires a constructed QGuiApplication for platform detection).
    /// @return The provider; never nullptr once a Qt application exists.
    [[nodiscard]] display::ForcedFontDpiProvider* forcedFontDpiProvider();

    /// The app's one settings bridge, created on first use: it edits the app's live configuration,
    /// writes the side files beside the configuration file, and applies a save by reloading every session.
    /// @return The controller; it lives as long as the app's Qt objects.
    [[nodiscard]] window::SettingsController& settingsController();

    /// What the app does as the application quits (connected to QCoreApplication::aboutToQuit): a Settings
    /// change committed within the debounce is written, not lost with the timer. Public so a test can
    /// run it without emitting the real signal, which every other listener of the process would receive.
    void flushForQuit();

    /// The GUI state beside contour.yml, over the app's file system, created on first use.
    /// @return The store; it lives as long as the app.
    [[nodiscard]] std::shared_ptr<config::GuiStateStore> const& guiState();

    /// The app's one Settings window host, created on first use over settingsController() and this
    /// host's conventions. The window itself is built on its first show.
    /// @return The host; it lives as long as the app's Qt objects.
    [[nodiscard]] window::SettingsWindowHost& settingsWindow();

    /// Shows, raises and focuses the Settings window (OpenConfiguration's default target), with or without
    /// a terminal window. Logs, and shows nothing, when the window cannot be built.
    /// @param anchor The screen of the window that asked; null for the primary screen.
    void openSettings(QScreen* anchor);

    /// Shows the Settings window on @p page (`section:hosts`, `host:NAME`), as openSettings() shows it.
    /// @param page The page's id.
    /// @param anchor The screen of the window that asked; null for the primary screen.
    void openSettingsPage(std::string const& page, QScreen* anchor);

    /// How the Connect picker presents itself.
    /// What a launch opens first.
    enum class FirstWindow : uint8_t
    {
        Terminal = 0,  ///< A terminal window, its sessions hosted as the launch decided.
        ConnectPicker, ///< The Connect picker, which decides where the sessions live.
    };

    /// Whether `contour connect` named a target.
    enum class ConnectLaunch : uint8_t
    {
        Targeted = 0, ///< It named one (or is not `contour connect` at all).
        Untargeted,   ///< It named none: the app opens on the picker alone.
    };

    enum class PickerPresentation : uint8_t
    {
        Launcher =
            0, ///< The first window of a launch: choosing a target ends this process, cancelling quits.
        Sheet, ///< File > New Window to…: modal over the window that asked, closing it leaves the app.
    };

    /// Shows the Connect picker -- built on first use, refreshed (daemon probed, filter cleared, last target
    /// selected) on every later one -- and focuses its field. Logs, and shows nothing, when it cannot be
    /// built.
    /// @param presentation What choosing a target and cancelling do.
    /// @param anchor The screen of the window that asked; null for the primary screen.
    void openConnectPicker(PickerPresentation presentation, QScreen* anchor);

    /// The app-wide keyboard layout (single instance; see KeyboardLayout.h). The active layout is
    /// process-global ambient state, and the macOS implementation registers an observer for changes
    /// to it, so one instance is shared rather than one per pane.
    /// @return The layout; never nullptr.
    [[nodiscard]] input::KeyboardLayout const& keyboardLayout() const noexcept { return *_keyboardLayout; }

    /// @return The machine's GPUs, listed without waking any; never null.
    [[nodiscard]] std::shared_ptr<display::IGpuInventory const> const& gpuInventory() const noexcept
    {
        return _gpuInventory;
    }

    /// @return Environment variables Contour set for its own rendering, which spawned shells must not
    /// inherit.
    [[nodiscard]] std::span<std::string const> selfOnlyEnvironment() const noexcept
    {
        return _selfOnlyEnvironment;
    }

    /// What `--latency-probe` asks for: a keystroke-to-pixel run against the first window.
    struct LatencyProbeRequest
    {
        std::filesystem::path reportFile; ///< Where the probe report is written.
        std::size_t keys = 0;             ///< How many keys are measured.
        display::SwapPacing pacing {};    ///< Whether swaps wait for the vertical blank.
    };

    /// Hands a not-yet-exposed window the chosen GPU (Vulkan/Direct3D); no-op for OpenGL.
    /// @param window The window, before it is first exposed.
    void applyGraphicsDevice(QQuickWindow& window);

    [[nodiscard]] std::string profileName() const;

    /// The emulation settings a session created now gets: the app's profile, resolved in the
    /// configuration this app holds in memory -- what a local new tab is built from, and what a
    /// configuration reload updates -- never re-read from the file.
    /// @return The settings, or why the profile is not in the configuration.
    [[nodiscard]] std::expected<vtbackend::Settings, std::string> currentEmulationSettings() const;

    /// The layout to open at startup: `--layout NAME` if given, else config's `default_layout`.
    /// Empty when neither is set (no startup layout).
    [[nodiscard]] std::string layoutName() const;

    /// The command the CLI asked this run to execute (`contour terminal PROGRAM ARGS...` or
    /// `--execute`), if any. It is applied by MUTATING the startup profile's shell, so every
    /// session on that profile runs it; recorded here so such sessions can report it as their
    /// launched command (e.g. for SaveLayout), which the mutated profile alone cannot reveal.
    [[nodiscard]] std::optional<vtpty::Process::ExecInfo> const& cliCommand() const noexcept
    {
        return _cliCommand;
    }

    /// The session exit status (Process/SSH exit variant, or nullopt). Single source of truth in
    /// ExitCode.h, shared with the pure exitCodeFor() mapping used by run().
    using ExitStatus = session::SessionExitStatus;

    [[nodiscard]] ExitStatus exitStatus() const noexcept { return _exitStatus; }

    [[nodiscard]] std::optional<std::filesystem::path> dumpStateAtExit() const;

    void onExit(session::TerminalSession& session);

    config::Config& config() noexcept { return _config; }
    [[nodiscard]] config::Config const& config() const noexcept { return _config; }
    [[nodiscard]] config::TerminalProfile const& profile() const noexcept
    {
        if (auto const* const profile = config().profile(profileName()))
            return *profile;
        display::displayLog()("Failed to access config profile.");
        Require(false);
    }

    [[nodiscard]] bool liveConfig() const noexcept { return _config.live.value(); }

    session::TerminalSessionManager& sessionsManager() noexcept { return _sessionManager; }

    /// The OS's secure keyboard entry, built from the appearance services. Only the session manager's gate
    /// switches it; the rest of the app asks it what it supports.
    /// @return The secure keyboard entry; it outlives the session manager.
    [[nodiscard]] platform::ISecureInput& secureInput() noexcept { return *_secureInput; }

    /// The application's user-notification centre, built from the appearance services: the null one
    /// wherever the platform has none, and in every test app unless it is given another. Every session's
    /// notifier and the GPU fallback's raise through it where it is Available. It is built on first use.
    /// @return The centre; it outlives the session manager and every notifier a session holds.
    [[nodiscard]] platform::IUserNotificationCenter& userNotificationCenter() noexcept
    {
        return _userNotificationCenter;
    }

    /// The shared capture and pool every session's status-line commands run on. Sessions are never
    /// destroyed in production, so nothing here is torn down under a terminal; at quit the destructor
    /// stops the shutdown token, which cancels every run, and the pool then drains quickly.
    /// @return References valid for the app's lifetime.
    [[nodiscard]] vtbackend::StatusLineCommandServices statusLineCommandServices() noexcept
    {
        return { .capture = _commandCapture, .executor = _commandExecutor };
    }

    /// What every session's status-line extensions share: the grants, the engine and the worker pool.
    /// Created on the first call, from the GUI thread, by the first session whose profile has extensions; a
    /// run without any never creates it, so it reads no grants file and starts no runtime and no thread.
    /// @return The runtime, valid for the app's lifetime.
    [[nodiscard]] vtextension::ExtensionRuntime& extensionRuntime();

    /// @return The extension runtime if a session has created it; null in a run without extensions.
    [[nodiscard]] vtextension::ExtensionRuntime* startedExtensionRuntime() noexcept
    {
        return _extensionRuntime.get();
    }

    /// Has the extension runtime release, a moment from now and on the GUI thread, the engines and modules
    /// that runners retired: a runner destroyed now retires its engine only once its lane ran its last job.
    void releaseRetiredExtensionsSoon();

    /// Asks the questions the attached daemon's status-line extensions ask, each in the pane its extension
    /// runs for and through the prompt that pane's own extensions ask through; the answers go back to the
    /// daemon, which keeps them. GUI thread; a no-op unless attached.
    void askHostedExtensions();

    /// What every session's link detection runs on: the status-line pool for matching, a thread of its own
    /// for existence checks (a hung mount stalls it alone), fileSystem(), and the steady clock of
    /// terminalClocks().
    /// @return References valid for the app's lifetime.
    [[nodiscard]] vtbackend::LinkDetectionServices linkDetectionServices() noexcept;

    /// What every session's `{Git}` runs on: the one path-probe thread and this machine's files. That thread
    /// is shared by `{Git}` and the link probes of every session, so a lookup on a hung mount stalls them all
    /// until the mount answers.
    /// @return References valid for the app's lifetime; nullopt for an app given a filesystem other than this
    ///         machine's, so that its `{Git}` reads nothing.
    [[nodiscard]] std::optional<vtbackend::GitHeadServices> gitHeadServices() noexcept;

    [[nodiscard]] std::chrono::seconds earlyExitThreshold() const;

    /// The path the process was launched as (argv[0]), used to re-spawn contour for a new
    /// window/tab and to open config hyperlinks. Falls back to the app name when argv is not yet
    /// available (any call before run() records it — e.g. a FollowHyperlink/NewTerminal action that
    /// fires before the event loop starts), so the accessor is total rather than dereferencing a
    /// null argv.
    [[nodiscard]] std::string programPath() const
    {
        return (_argv != nullptr && _argc > 0) ? std::string { _argv[0] } : appName();
    }

    /// The external-resource launcher (URL opening, child-process spawning) for this app's sessions.
    [[nodiscard]] platform::ExternalLauncher& externalLauncher() noexcept { return *_externalLauncher; }

    /// The voice this app's sessions read selections through.
    ///
    /// One per app rather than one per session, because a machine has one voice: two sessions
    /// speaking at once is not a thing the platform can do, and with an engine each, "Stop Speaking"
    /// in one tab could not stop what another tab had started.
    [[nodiscard]] platform::SpeechSynthesizer& speechSynthesizer() noexcept { return *_speechSynthesizer; }

    /// The private files OpenCommandOutput hands to pager panes; one per app, removed at exit.
    [[nodiscard]] session::CommandOutputFiles& commandOutputFiles() noexcept { return *_commandOutputFiles; }

    /// The clocks every session's terminal reads: multi-click timing, command durations, row times.
    [[nodiscard]] vtbackend::TerminalClocks const& terminalClocks() const noexcept { return _terminalClocks; }

    /// This machine, as every session judges what is local by it.
    /// @return The identity read, or handed in, at construction.
    [[nodiscard]] vtbackend::HostIdentity const& hostIdentity() const noexcept { return _hostIdentity; }

    /// What places every process a session starts; the default session factory, the default launcher and
    /// the daemon and tmux spawns all use it.
    /// @return The placement handed in at construction, or the default one.
    [[nodiscard]] vtpty::ProcessPlacement& processPlacement() const noexcept { return *_processPlacement; }

    /// Where every session looks up the paths printed on its screen, and the ones it opens; link detection's
    /// probes too.
    /// @return The filesystem handed in at construction; this machine's by default.
    [[nodiscard]] core::platform::FileSystem const& fileSystem() const noexcept { return _fileSystem; }

    /// The remote host this process is attached to, as `contour client --host` named it: the host's name
    /// under `hosts:`, or the URL of an ad-hoc host.
    /// @return Empty unless `contour client --host` attached; fixed once its connect succeeded, as a
    ///         process attaches to one daemon for its life.
    [[nodiscard]] std::string const& attachedHostLabel() const noexcept { return _attachedHostLabel; }

    /// The editor of the shells' startup files, behind the shell-integration card and Settings.
    [[nodiscard]] platform::IShellRcEditor& shellRcEditor() noexcept { return *_shellRcEditor; }

    /// The running binary, as the line in a shell's startup file names it.
    /// @return The injected path (Reachable), else window::locateBinary() over this process's environment
    ///         and `QCoreApplication::applicationFilePath()`: an AppImage's file, not its mount.
    [[nodiscard]] window::LocatedBinary locatedContourBinary() const;

    /// The host this process runs on, which decides where bash keeps its startup file.
    [[nodiscard]] config::HostPlatform hostPlatform() const noexcept { return _hostPlatform; }

    /// The host the shell-integration offer decides on: hostPlatform(), unless a test injected another.
    [[nodiscard]] config::HostPlatform shellHostPlatform() const noexcept { return _shellHostPlatform; }

    [[nodiscard]] vtbackend::ColorPreference colorPreference() const noexcept { return _colorPreference; }

    /// Applies the configured GUI chrome theme (dark/light/system) to the application's color
    /// scheme, recoloring the Qt chrome (title bar, tab strip, command palette, settings, dialogs).
    ///
    /// @c GuiTheme::System defers to the operating system's color scheme; @c Dark / @c Light force
    /// the appearance regardless of the OS. This affects the GUI chrome only: the terminal grid's
    /// light/dark preference is derived from the OS independently of this setting, so a forced GUI
    /// theme never drags the grid with it. Note that forcing a theme pins the shared @c QStyleHints
    /// color scheme, so while pinned the grid reflects the OS scheme captured at startup; a live OS
    /// switch is tracked whenever the chrome is not pinned (see the @c colorSchemeChanged handler in @c
    /// run()). Safe to call at startup and live (e.g. from the settings page).
    /// @c GuiTheme::Terminal forces the scheme the terminal background's luminance calls for.
    /// While the OS runs a contrast theme every @p theme acts as @c GuiTheme::System, so the contrast
    /// theme's palette reaches the chrome (@c platform::effectiveGuiTheme); the appearance handler applies
    /// the configured theme again when the contrast theme comes or goes.
    /// @param theme The GUI theme to apply.
    /// @param colors The default profile's color configuration, which @c GuiTheme::Terminal reads.
    /// @param liveBackground The focused session's runtime default background (OSC 11), which
    ///        @c GuiTheme::Terminal over a simple color configuration follows instead of @p colors'
    ///        background; @c std::nullopt for the configured one.
    /// @note On Qt older than 6.8 (no @c QStyleHints::setColorScheme) this is a no-op and the GUI
    ///       follows the OS unconditionally.
    void applyGuiTheme(config::GuiTheme theme,
                       config::ColorConfig const& colors,
                       std::optional<vtbackend::RGBColor> liveBackground);

    /// Applies @p theme over the configured background alone. @see applyGuiTheme above.
    /// @param theme The GUI theme to apply.
    /// @param colors The default profile's color configuration.
    void applyGuiTheme(config::GuiTheme theme, config::ColorConfig const& colors)
    {
        applyGuiTheme(theme, colors, std::nullopt);
    }

    /// Re-applies the chrome theme over the focused session's live background (OSC 11), but only under
    /// @c GuiTheme::Terminal and only when the resolved scheme differs from the one last applied.
    /// GUI thread only.
    /// @param liveBackground The focused session's current default background.
    void followLiveBackground(vtbackend::RGBColor liveBackground);

    /// The scheme the chrome was last forced to, or @c std::nullopt while it follows the OS.
    /// @return The applied scheme. The native window frame reads it, so it agrees with the chrome.
    [[nodiscard]] std::optional<Qt::ColorScheme> appliedGuiScheme() const noexcept
    {
        return _appliedGuiScheme;
    }

    /// The scheme the chrome shows: the one last forced onto it, else the OS's. The native window frames
    /// read it, so they agree with the chrome after an OSC 11 too.
    /// @return The effective scheme.
    [[nodiscard]] Qt::ColorScheme effectiveColorScheme() const;

    /// The application's version string, as the About dialog shows it.
    /// Defined in ContourGuiApp.cpp, the one place the build-wide version macro is visible to the GUI.
    /// @return The version, with the branch and commit it was built from.
    [[nodiscard]] QString applicationVersion() const;

    /// The color configuration of the default profile, which @c GuiTheme::Terminal follows.
    [[nodiscard]] config::ColorConfig const& defaultProfileColors() const noexcept
    {
        return _config.profile().colors.value();
    }

    /// Re-points the window controls at @p style, resolving @c Auto against this host.
    ///
    /// The companion to @c applyGuiTheme, and called from the same two places for the same reason:
    /// both are application-wide state derived from the configuration that must survive a live
    /// reload. Every open window follows at once, because one provider serves the one QML engine.
    /// Safe to call at startup and live (e.g. from the settings page) -- before the QML engine
    /// exists there is simply nothing to re-point, and the provider is built with the resolved style
    /// when it does.
    /// @param style The configured window-control style, @c Auto included.
    void applyWindowControlStyle(config::WindowControlStyle style);

    /// @return The OS accessibility preferences as the injected source reports them now.
    [[nodiscard]] config::AccessibilityPreferences accessibilityPreferences() const;

    /// What a split tree spends around its leaves, for the window-size solver: the published provider's
    /// resolved tokens in its own units, or -- before the GUI exists, or headless -- the configured row
    /// resolved against the density and the accessibility preferences, one logical pixel per unit. The
    /// gaps and the inset are counted as PaneNode.qml draws them on @p devicePixelRatio's grid.
    /// @param devicePixelRatio The ratio of the window being sized; positive.
    /// @return The gaps per axis, the pane header and the cards' inset, in logical pixels.
    [[nodiscard]] vtworkspace::SplitChrome splitChrome(double devicePixelRatio) const;

    /// Public for the display tests, which draw the pane tree with the app's own chrome to check the
    /// window-size solver against what QML lays out; nothing in the app outside this class reads it.
    /// @return What the chrome is built from now: the row, the resolved chrome font, both OS sources'
    ///         current values, and the scheme of the terminal palette in effect.
    [[nodiscard]] window::ChromeInputs chromeInputs() const;

    /// The inputs of the quick terminal's own `chromeStyle`: chromeInputs() at the density its role takes.
    [[nodiscard]] window::ChromeInputs quickTerminalChromeInputs() const;

    /// How long the reveal modifier is held alone before Minimal's pane headers show: the published
    /// provider's @c motion.headerRevealMs, or -- before the GUI exists -- the configured row's. Reduce
    /// Motion leaves it be, so it is read once, by each window at its construction.
    /// @return The delay.
    [[nodiscard]] std::chrono::milliseconds headerRevealDelay() const;

    /// The chrome row's window corner radius (@c radii.window): the published provider's once the GUI
    /// built it, before that (and in a headless test) the resolved row's.
    /// @return The radius, in logical pixels; 0 on rows that draw square windows.
    [[nodiscard]] int windowRadius() const;

    /// @return The OS appearance as the injected source reports it now.
    [[nodiscard]] platform::PlatformAppearance platformAppearance() const;

    /// Where the desktop puts the window buttons: what the platform appearance source publishes, else
    /// config::fallbackButtonLayout(). A pure read, so a headless test observes it; what the window
    /// controls draw is this, placed by the resolved window-control style.
    /// @return The desktop's button layout.
    [[nodiscard]] config::WindowButtonLayout desktopButtonLayout() const;

    /// Where @p resolved's controls go on this desktop: config::windowButtonPlacement of it and the
    /// platform appearance's button layout.
    /// @param resolved A concrete window-control style.
    /// @return The placement the window controls are drawn in.
    [[nodiscard]] config::WindowButtonLayout windowControlPlacement(
        config::WindowControlStyle resolved) const;

    /// @return The injected native menu presenter; the null one where the platform has none.
    [[nodiscard]] platform::INativeMenuPresenter& nativeMenuPresenter() noexcept;

    /// @return The injected title-band material; the null one where the platform draws none.
    [[nodiscard]] platform::ITitleBandMaterial& titleBandMaterial() noexcept;

    /// @return The injected caption adapter; the null one where the platform hands no button to the OS.
    [[nodiscard]] platform::INativeCaptionButtons& captionButtons() noexcept;

    /// @return The platform whose frame policy the windows follow: the build's own unless the appearance
    ///         services injected another.
    [[nodiscard]] platform::FramePlatform framePlatform() const noexcept { return _framePlatform; }

    /// @return How the resolved chrome style presents menus.
    [[nodiscard]] config::MenuPresentation menuPresentation() const noexcept;

    /// The chrome row, resolved from `ui_style` and the host platform at construction and once more when
    /// terminalGuiAction() has loaded the configuration -- before any session exists. A reload does not
    /// change it (`ui_style` takes effect on the next start), so a session reads it once.
    /// @return The resolved chrome row.
    [[nodiscard]] config::ChromeStyle chromeStyle() const noexcept { return _chromeStyle; }

    /// The title band's tokens: the chrome row, its resolved title-band material, the band's token height
    /// for @p centre, on the Terminal row one cell's height, and the chrome density. From the published
    /// provider once the GUI built it; before that (and in a headless test) from the row's tokens alone,
    /// with the Flat rows' height and the cell unknown (0).
    /// @param centre What the band's middle holds: an empty one is @c size.bareBand tall.
    /// @return The tokens.
    [[nodiscard]] window::TitleBandTokens titleBandTokens(window::BarCentre centre) const;

    /// Where the title band and the tabs go (window::resolveTitleBand) for the chrome row: the published
    /// provider's (UiStyleProvider::titleBandOf) once the GUI built it; before that (and in a headless test)
    /// the resolved row's tab shape and the configured density.
    /// @param inputs A window's side of the decision: frame, mode, gate, edge and the host's notice.
    /// @return The band.
    [[nodiscard]] window::TitleBand titleBandOf(window::TitleBandInputs const& inputs) const;

    /// @return Where this host's menu bar lives, resolved once from the host at construction.
    [[nodiscard]] command::MenuBarPlacement menuBarPlacement() const noexcept { return _menuBarPlacement; }

    /// Rebuilds the `chromeStyle` context property from the current inputs, if any changed.
    ///
    /// Called wherever an input may have moved: a reload (font, scheme), the OS light/dark flip
    /// (scheme), and the two OS appearance sources. Safe before the GUI exists, like
    /// applyWindowControlStyle(): there is nothing to rebuild yet.
    void refreshChromeStyle();

    /// Brings everything derived from the configuration up to date after a reload replaced it: the
    /// chrome tokens (refreshChromeStyle()), the new-defaults banner and the menu bar. Safe before the
    /// GUI exists, as refreshChromeStyle() is: an engine-less app (a headless test) reloads too.
    void onConfigurationReloaded();

    /// Runs a menu bar row's @p action, where its @p scope says.
    ///
    /// A Window-scoped row runs on the focused window's active session, and does nothing without one. An
    /// Application-scoped row runs on the active session of applicationController()'s window, so it
    /// still works with every window minimised or another application in front; with no window left,
    /// the rows that need none (Quit, New Window, Settings…) run on the application itself, and the rest
    /// do nothing -- Open Configuration File among them, which only logs.
    /// @param action The picked row's action.
    /// @param scope What the row acts on.
    void runMenuBarAction(actions::Action const& action, command::ActionScope scope);

  private:
    /// The configured row resolved as the provider would resolve it, for the paths that run before a
    /// provider exists: against the configured density, the accessibility preferences and the OS's material
    /// support.
    /// @return The resolved row.
    [[nodiscard]] config::ChromeTokenRow resolvedChromeTokens() const;

    /// Logs, once per binding, each input mapping whose chord the macOS menu bar takes from the terminal
    /// (see window::menuOwnedBindings()), so a binding that never runs does not go unexplained. For a load
    /// and a reload of the configuration, under a global menu bar.
    void warnOfMenuOwnedBindings() const;

    /// Runs @p action with no session to run it on: the Application-scoped rows that need no window --
    /// Quit, New Window, and Settings… (OpenConfiguration's Settings window target). Any other action,
    /// Open Configuration File included, only logs that it needs a window.
    /// @param action The picked row's action.
    /// @param anchor The screen of the terminal window that has no session to run it on, which Settings
    ///               opens on; null when there is no terminal window, for the primary screen.
    void runWithoutWindow(actions::Action const& action, QScreen* anchor);

    /// The Settings window factory used when none is injected: loads SettingsWindow on the app's own QML
    /// engine, and gives SettingsWindowError::Unavailable while there is none (a headless app).
    /// @return The factory; it borrows this app.
    [[nodiscard]] window::SettingsWindowFactory engineSettingsWindowFactory();

    /// @return The focused window's controller, or nullptr when no window is focused.
    [[nodiscard]] window::WindowController* focusedController() const noexcept;

    /// @return What the menu bar reads and runs through: the focused window's state, or the configuration's
    ///         alone without one; the configured chords; and runMenuBarAction() to run a row with.
    [[nodiscard]] window::AppMenuBarSources appMenuBarSources();

    /// The OS reported new accessibility preferences.
    /// @param preferences The preferences now in effect.
    void onAccessibilityPreferencesChanged(config::AccessibilityPreferences preferences);

    /// The OS reported a new appearance.
    /// @param appearance The appearance now in effect.
    void onPlatformAppearanceChanged(platform::PlatformAppearance const& appearance);

    /// Records what the OS says about light/dark and re-derives the terminal's preference from it.
    ///
    /// The one place both OS reports go through -- Qt's style hints and the platform appearance source. While
    /// the GUI theme is pinned to dark or light, the style hints report our own override and the terminal
    /// follows the pin instead (see @c refreshColorPreference), so the OS's flip is remembered, not applied.
    /// @param preference The OS's preference, from platform::osColorPreference().
    void applyOsColorPreference(vtbackend::ColorPreference preference);

    /// Derives the terminal's light/dark preference from the OS's and the GUI theme in effect
    /// (platform::terminalColorPreference) and, when it moved, re-seeds every session's palette.
    /// Runs on every OS report and every GUI theme change, so a manual dark/light switch reaches the terminal
    /// as the OS's own flip does. GUI thread only.
    void refreshColorPreference();

    /// The accessibility source, built on the first call that the injected services can build it on. GUI
    /// thread only.
    /// @return The source, or null while the services cannot build it yet (asked again on the next call).
    [[nodiscard]] platform::IAccessibilityPreferenceSource const* accessibilitySource() const;

    /// The platform appearance source, built on the first call that the injected services can build it on.
    /// GUI thread only.
    /// @return The source, or null while the services cannot build it yet (asked again on the next call).
    [[nodiscard]] platform::IPlatformAppearance const* platformAppearanceSource() const;

    /// Logs, once per app, that an OS appearance source could not be built yet and the defaults stand in.
    void logUnbuiltAppearanceSource() const;

    /// The scheme @c applyGuiTheme would force for @p theme now, the OS's contrast theme considered.
    /// @param theme The configured GUI theme.
    /// @param liveBackground The focused session's runtime default background, if known.
    /// @return The scheme to force, or @c std::nullopt to follow the OS.
    [[nodiscard]] std::optional<Qt::ColorScheme> guiSchemeFor(
        config::GuiTheme theme, std::optional<vtbackend::RGBColor> liveBackground) const;

    /// @p configured with @c Auto resolved against this host.
    ///
    /// The one place that resolves, so the style the first frame draws and the style a later reload
    /// applies cannot disagree about what the host is.
    ///
    /// @param configured The configured style, @c Auto included.
    /// @return A concrete style; never @c Auto.
    [[nodiscard]] config::WindowControlStyle resolvedWindowControlStyle(
        config::WindowControlStyle configured) const;

    static void ensureTermInfoFile();
    /// Registers the fonts shipped with the application, once, before the first font lookup. A failure
    /// is logged and never fatal. Used by every verb that resolves fonts, so they all agree.
    void registerBundledFonts() const;

    void setupQCoreApplication();
    /// Selects the Qt RHI graphics API, and the GPU it runs on, from the configured renderer backend.
    /// Must run once the QApplication exists and before the first QQuickWindow is created.
    void selectGraphicsApi();
    bool loadConfig(std::string const& target);
    int terminalGuiAction();
    int fontConfigAction();
    int checkConfig();

    /// The `contour connect [TARGET]` verb: with a target, boots the window the way `contour client` or
    /// `contour terminal` would for it (config::attachPlanOf); without one, boots the app on the Connect
    /// picker alone. `--list` prints the picker's targets and ends.
    int connectAction();

    /// The `contour client` verb: collects its options into @ref _clientRequest, requires a daemon (or
    /// tmux server), and boots the GUI through terminalGuiAction(), whose establishHosting() acts on them.
    int clientAction();

    /// Which engine a `contour client` attaches through.
    enum class ClientEngine : uint8_t
    {
        Native = 0, ///< A Contour daemon, over its native protocol.
        Tmux = 1,   ///< A tmux server, in control mode (`--tmux`).
    };

    /// What `contour client` asked for, read before the verb re-parses into the terminal verb's surface,
    /// where its own options are gone. establishHosting() acts on it.
    struct ClientRequest
    {
        ClientEngine engine = ClientEngine::Native;
        std::string tmuxSocket;         ///< `--tmux-socket`.
        std::string socket;             ///< `--socket`; empty for `daemon.socket`.
        std::string label;              ///< `--label`; empty when not given.
        std::string profile;            ///< `--profile`.
        std::string config;             ///< `--config`.
        std::string connectTcp;         ///< `--connect-tcp`; empty for the local socket.
        std::string tlsCaPath;          ///< `--tls-ca`.
        std::string token;              ///< `--token` or `--token-file`, resolved.
        std::string sizePolicy;         ///< `--size-policy`; empty for `daemon.size_policy`.
        std::string logFilter;          ///< `--log`, forwarded to a daemon it starts.
        std::string logFile;            ///< `--log-file`, likewise.
        std::optional<uint64_t> window; ///< `--window`: the one daemon window to show.
        std::optional<cli::HostTransport>
            host; ///< `--host`, resolved: a remote daemon instead of the local one.
    };

    /// Boots a `contour client` window for @p request: resolves @p host into its transport when one is named,
    /// re-parses into the terminal verb's surface and runs it. What `contour client` and `contour connect`
    /// both end in.
    /// @param request What the window attaches with.
    /// @param host The `--host` option: a saved host's name or an ssh:// URL; empty for the local daemon.
    /// @return The process exit status.
    int launchClient(ClientRequest request, std::string const& host);

    /// A host and the transport that reaches its daemon.
    struct OpenedHost
    {
        config::HostEntry entry;      ///< The host as configured.
        cli::HostTransport transport; ///< How to reach its daemon.
    };

    /// Looks @p nameOrUrl up (`contour client --host`'s lookup) and maps it onto its transport.
    /// @param configPath The configuration file naming the saved hosts; empty for the default one.
    /// @param nameOrUrl A saved host's name or an ssh:// URL.
    /// @return The host and its transport, or the line saying why it cannot be reached as configured.
    [[nodiscard]] std::expected<OpenedHost, std::string> openHost(std::string const& configPath,
                                                                  std::string const& nameOrUrl);

    /// Maps an already resolved host onto its transport.
    /// @param nameOrUrl What named the host: its saved name, or its URL; the window's label.
    /// @param host The host.
    /// @return The host and its transport, or why it cannot be reached as configured.
    [[nodiscard]] std::expected<OpenedHost, std::string> transportOf(std::string const& nameOrUrl,
                                                                     config::HostEntry host);

    /// Prepares the Connect picker's check of whether @p target answers, before it spends a window on it.
    /// Reads the configuration here, on the GUI thread, so the job it returns owns everything it needs.
    /// @param target The chosen target.
    /// @return The job to run on a worker: nothing for a target that cannot fail ahead of time, else the
    ///         transport's classified words.
    [[nodiscard]] std::function<std::expected<void, std::string>()> prepareCheck(
        config::LaunchTarget const& target);

    /// Starts `contour ARGUMENTS...` as a process of its own.
    /// @param arguments The arguments after the program.
    /// @return Nothing once started, or why not.
    [[nodiscard]] std::expected<void, std::string> startProcess(std::vector<std::string> const& arguments);

    /// Probes this machine's daemon for the picker and `contour connect --list`.
    /// @param socketOption `daemon.socket`; empty for the default path.
    /// @return Whether it runs, and how many sessions it hosts.
    [[nodiscard]] static cli::LocalDaemonState probeLocalDaemon(std::string const& socketOption);

    /// Starts `contour connect TARGET` as a process of its own: a GUI process attaches to one daemon for
    /// life.
    /// @param target The chosen target.
    /// @return Nothing once started, or why not.
    [[nodiscard]] std::expected<void, std::string> launchConnect(config::LaunchTarget const& target);

    /// Lists the picker and the saved hosts in the OS's launcher menu (the Dock menu, the jump list), when it
    /// has one; built on first call and re-listed on every configuration reload.
    void refreshLauncherMenu();

    /// What the Connect picker reaches the app through.
    [[nodiscard]] window::ConnectPickerServices connectPickerServices();

    /// @return What this launch opens first: the Connect picker for `contour connect` without a target, or
    /// for a
    ///         plain launch while `connect_picker_on_start` is on; else a terminal window.
    [[nodiscard]] FirstWindow firstWindow() const;

    /// Opens the first window of the launch: the Connect picker when the launch asks
    /// (opensOnConnectPicker()), else a terminal window.
    /// @return Nothing once it is open, or why the launch ends: the picker could not be built, which never
    /// falls
    ///         back to a terminal window that would host its shells without the hosting the launch asked for.
    [[nodiscard]] std::expected<void, std::string> openFirstWindow();

    /// Decides where this launch hosts its sessions (`daemon.mode`, forced to `required` by `contour
    /// client`, else `--daemon-mode`, else the configuration) and, for a mode that attaches, starts the
    /// connect: the attach is awaited, and the in-process fallback taken, once the QML engine exists
    /// (settleHosting()), so the connect runs beside the GUI's boot. Runs right after the configuration
    /// loaded.
    /// @return Nothing to continue the launch; the line to print, ending it, otherwise.
    [[nodiscard]] std::expected<void, std::string> establishHosting();

    /// Lets a daemon spawned on demand fork. @see _daemonSpawnGate
    void openDaemonSpawnGate() noexcept;

    /// Starts the native attach establishHosting() decided on: builds the transport and the controller,
    /// wires the controller's signals and starts its connect.
    /// @return Nothing once connecting; the line to print otherwise.
    [[nodiscard]] std::expected<void, std::string> startNativeAttach();

    /// Attaches to a tmux server, as `contour client --tmux` asks, before the GUI boots.
    /// @param request What the client asked for.
    /// @return Nothing once attached; the line to print otherwise.
    [[nodiscard]] std::expected<void, std::string> startTmuxAttach(ClientRequest const& request);

    /// Maps how the attach ended through cli::decideHosting(): attached, hosted in-process with the
    /// first window telling the user why, or the launch ends.
    /// @param attached How the attach ended.
    /// @return Nothing to continue the launch; the line to print, ending it, otherwise.
    [[nodiscard]] std::expected<void, std::string> settleHosting(
        std::expected<void, std::string> const& attached);

    /// Native attach: brings the local OS windows in line with the daemon's windows (B4). Maps the
    /// primary daemon window onto the boot window and, for each additional daemon window, spawns an OS
    /// window (which binds itself via bindPendingAttachWindow) then reconciles each window's tab/pane
    /// tree. Idempotent — re-run on every layoutChanged. No-op when not attached.
    void reconcileAttachWindows();

    /// Attach-window binder installed on the manager: called from a freshly-spawned window's Main.qml
    /// (via consumeAttachWindow). If a daemon window is staged for it, records the daemon→OS-window
    /// mapping, reconciles that window's layout into it, and returns true so the window does NOT create
    /// its own first tab.
    /// @param controller The freshly-created controller of the just-spawned OS window.
    /// @return true if @p controller's window was bound to a pending daemon window.
    bool bindPendingAttachWindow(window::WindowController* controller);

    /// Waits for the native attach started before the GUI booted, with what remains of its budget, then
    /// routes session creation to the controller and offers a restart to a drifted local daemon.
    /// @param budget How long the attach may take since it started; never less than MinimumConnectAwait.
    /// @param hostLabel The remote host it attaches to (`--host`), which attachedHostLabel() reports once
    ///        attached; empty for the local daemon or `--connect-tcp`.
    /// @return Nothing once attached; why not, the controller given up, otherwise.
    [[nodiscard]] std::expected<void, std::string> awaitNativeAttach(std::chrono::milliseconds budget,
                                                                     std::string hostLabel);

    /// After a reconnect: closes the OS windows of the daemon windows the daemon no longer has for this
    /// client, once the windows that came instead are shown, so that closing them never closes the last one.
    /// @param daemonWindows The lost daemon windows.
    void closeLostAttachWindows(std::vector<std::uint64_t> const& daemonWindows);

    /// Connects what the native controller says about a lost connection: every pane's reconnecting state,
    /// and the windows a reconnect did not bring back.
    void connectReconnectSignals();

    /// Shows the daemon-drift badge when the daemon @p controller met runs another release than this
    /// client, and wires the badge's restart action to restartLocalDaemon().
    /// @param controller The controller connected to the local daemon.
    void offerDaemonRestartIfDrifted(remote::NativeController const& controller);

    /// Stops the local daemon for an upgrade and starts one on this release, the way a client would.
    /// Runs off the GUI thread.
    /// @param label The socket label.
    /// @param controlPath The daemon's control socket.
    /// @param options What a daemon spawned on demand is told.
    /// @return Nothing once a daemon serves, or why none does.
    [[nodiscard]] std::expected<void, std::string> restartLocalDaemon(
        std::string const& label,
        std::filesystem::path const& controlPath,
        vthost::DaemonSpawnOptions const& options);

    /// Records the daemon-window → OS-window mapping and immediately reconciles that daemon window's
    /// tab/pane tree into @p osWindow — the one place the "a mapping always implies a reconcile"
    /// invariant lives (shared by the primary-window and spawned-window paths).
    /// @param daemonWindow The daemon window id.
    /// @param osWindow The OS window that mirrors it.
    void bindDaemonWindow(std::uint64_t daemonWindow, vtworkspace::WindowId osWindow);

    /// Forgets a restore summary no window took, once a layout has been reconciled and no window of it is
    /// still to be bound: a toast about a restore must not surface in a window opened later.
    void expireRestoreSummaryAfterLayout();

    /// The installed log destination for the GUI verbs. Categories hold a reference into it, so it
    /// must outlive every one of them -- hence a member rather than a local of the verb handler.
    /// Separate from app::_logOutput, which is private to the base and serves the client/daemon
    /// verbs that call installLogging() with their own option prefix.
    std::unique_ptr<core::log::ScopedOutput> _guiLogOutput;

    /// When each step from process start to the first frame happened. Declared before the attach
    /// engines, which stamp it, so it outlives them.
    vthost::latency::StartupTimeline _startupTimeline;

    /// The attach engines, declared FIRST so they are destroyed LAST: remote-
    /// backed sessions hold ptys that unregister from them on destruction.
    std::unique_ptr<remote::NativeController> _nativeController;
    std::unique_ptr<remote::TmuxController> _tmuxController;

    /// Invoked by terminalGuiAction right after the first window booted —
    /// attach mode adopts the remaining remote sessions as tabs here.
    std::function<void()> _onGuiBooted;

    /// Invoked by terminalGuiAction just before the first window: attach mode waits here for the
    /// connect it started before the GUI booted, and adopts the controller once it is up.
    /// @return Nothing once attached; the message to report, the controller given up, otherwise.
    std::function<std::expected<void, std::string>()> _awaitAttach;

    /// Native attach (B4): each daemon window id mapped to the OS window that mirrors it. The primary
    /// daemon window maps to the boot window; additional ones map to spawned windows.
    std::unordered_map<uint64_t, vtworkspace::WindowId> _attachWindowMap;
    /// The daemon window whose OS window is being spawned right now: reconcileAttachWindows stages it
    /// just before newWindow(), and that window's Main.qml claims it via bindPendingAttachWindow.
    /// Single-slot (like _pendingTransplant / _pendingSpawnScreen): the QML load is synchronous, so a
    /// stage is always consumed before the next is made.
    std::optional<uint64_t> _pendingAttachWindow;

    /// Whether the boot window has adopted the daemon's primary window yet.
    /// Not "the map is empty": after a reconnect lost every window, the windows that came instead are new
    /// OS windows, never the stale one about to be closed.
    session::AttachBoot _attachBoot = session::AttachBoot::Pending;

    /// What `contour client` asked for; empty for a plain launch. @see establishHosting
    std::optional<ClientRequest> _clientRequest;
    /// Whether `contour connect` was run without a target, which opens the app on the Connect picker.
    ConnectLaunch _connectLaunch = ConnectLaunch::Targeted;
    /// The `daemon.mode` this launch runs under, as establishHosting() resolved it.
    config::DaemonMode _daemonMode = config::DaemonMode::InProcess;
    /// What the first window tells the user when the daemon this launch wanted could not be had and its
    /// sessions run in-process instead (cli::hostingNotice); empty otherwise.
    std::string _hostingNotice;
    /// @see attachedHostLabel
    std::string _attachedHostLabel;
    /// The PATH `contour client --host` looks ssh up on. A member fixed at construction, as every spawner's
    /// vtpty::ProgramSearch holds a view of it for as long as its attach reconnects.
    std::string _sshSearchPath;

    config::Config _config;
    // Sessions are heap-allocated with C++ ownership and never destroyed in production, so their command
    // caches outlive these members and never cancel their own runs. ~ContourGuiApp() therefore stops
    // _commandShutdown first, which cancels every run in flight and makes later ones return at once, so
    // ~ThreadPoolExecutor drains and joins quickly. Declared in this order: the source before the capture
    // that holds its token; the executor after the capture, so it is destroyed (and joins) first and the
    // capture outlives every run.
    core::async::StopSource _commandShutdown;
    vtpty::SystemCommandCapture _commandCapture;
    core::async::ThreadPoolExecutor _commandExecutor;
    // Moves every process a session starts into a resource domain of its own: its shell, through the
    // default session factory, and the programs it starts detached, through the default launcher.
    // Declared before both.
    std::shared_ptr<vtpty::ProcessPlacement> _processPlacement;
    /// What every session's `{Git}` lookups read: this machine's files, through fileSystem(). Declared before
    /// _linkProbeExecutor, which runs those lookups, so it outlives every lookup the executor joins.
    vtbackend::NativeGitFileSystem _gitFileSystem;
    /// The one thread every session's path probes run on. Its jobs reach only the filesystem, the clock and a
    /// terminal's redraw request (atomics, and a session's mutex-guarded notify target), which is why it may
    /// be drained and joined while sessions, never destroyed in production, live on. A probe blocked on a
    /// hung mount delays that join.
    core::async::ThreadPoolExecutor _linkProbeExecutor;
    /// The link detection services a test handed in; absent, linkDetectionServices() assembles the app's own.
    std::optional<vtbackend::LinkDetectionServices> _injectedLinkServices;
    // Declared before _sessionManager: the manager holds a reference to the factory.
    // Always a RoutingSessionFactory wrapping the injected/default factory,
    // so attach mode can switch the route without touching the manager.
    std::unique_ptr<session::SessionFactory> _sessionFactory;
    remote::RoutingSessionFactory* _routingFactory = nullptr; ///< The concrete view of _sessionFactory.
    // The external-resource launcher (URL open / process spawn), reached by sessions via _app.
    std::unique_ptr<platform::ExternalLauncher> _externalLauncher;
    // Declared before _sessionManager: the manager holds a reference to the store.
    std::unique_ptr<config::LayoutStore> _layoutStore;
    // Likewise: the manager holds a reference to the command-history store.
    std::unique_ptr<command::CommandHistoryStore> _commandHistoryStore;
    std::unique_ptr<window::WindowPositionStore> _windowPositionStore;
    // Shared by every session, reached via _app; @see speechSynthesizer().
    std::unique_ptr<platform::SpeechSynthesizer> _speechSynthesizer;
    // Declared before _commandOutputFiles, whose production instance borrows it to tell whose leftover
    // directories it may remove.
    session::NativeProcessLiveness _processLiveness;
    // Declared before _sessionManager: a pager pane's session releases its file here when it closes,
    // which during teardown happens while the manager is being destroyed.
    std::unique_ptr<session::CommandOutputFiles> _commandOutputFiles;
    // Declared before _sessionManager, whose sessions read it as they are built.
    vtbackend::TerminalClocks _terminalClocks;
    // Likewise.
    vtbackend::HostIdentity _hostIdentity;
    // Likewise; borrowed.
    core::platform::FileSystem const& _fileSystem;
    std::unique_ptr<platform::TaskbarProgressSink> _taskbarProgressSink;
    // The WebAssembly backend the extension runtime's engines are created from and its lanes prepared with;
    // created with the runtime (@see extensionRuntime()). Declared before _extensionRuntime, which borrows it
    // and so must go first. Opaque for the reason _extensionRuntime is.
    std::unique_ptr<vtextension::wasm::IWasmBackend, void (*)(vtextension::wasm::IWasmBackend*)>
        _extensionBackend { nullptr, nullptr };
    // Created on first use (@see extensionRuntime()). Declared after what it borrows -- _extensionBackend,
    // _commandCapture, _terminalClocks, _fileSystem and the environment -- and before _sessionManager, whose
    // sessions, and so every runner, are destroyed first. Its own pool is joined before the grants and the
    // capture it reaches. Its deleter is chosen where it is made, so that this class is laid out the same in
    // a translation unit without CONTOUR_EXTENSIONS, which never makes one.
    std::unique_ptr<vtextension::ExtensionRuntime, void (*)(vtextension::ExtensionRuntime*)>
        _extensionRuntime { nullptr, nullptr };
    // Where the answers to the attached daemon's extensions are recorded, and so sent; made with the first
    // question. Declared before _sessionManager, whose sessions' prompts hold it, and after
    // _nativeController, which it sends through. Opaque for the reason _extensionRuntime is.
    std::unique_ptr<vtextension::IGrantStore, void (*)(vtextension::IGrantStore*)> _daemonGrants { nullptr,
                                                                                                   nullptr };
    // The material the OS draws behind each window's title band, built from the appearance services.
    // Declared before _sessionManager and _qmlEngine, unlike the appearance sources below, so it is destroyed
    // after them: the manager's window controllers, and the windows they drive, borrow it.
    std::unique_ptr<platform::ITitleBandMaterial> _titleBandMaterial;
    // The OS's secure keyboard entry, built from the appearance services. Declared before _sessionManager,
    // whose gate calls it and turns it off as the manager is destroyed: so it is destroyed after the gate.
    std::unique_ptr<platform::ISecureInput> _secureInput;
    // The OS's user-notification centre, built from the appearance services on first use, so a CLI verb
    // (which constructs this app but never raises a notification) never touches Notification Center.
    // Declared before _sessionManager, whose sessions' notifiers withdraw their notifications from it as
    // they go.
    platform::LazyUserNotificationCenter _userNotificationCenter;
    // The adapter that hands each client-drawn window's maximise button to the OS, built from the appearance
    // services. Declared before _sessionManager for the reason _titleBandMaterial is: the window controllers
    // detach from it as they go.
    std::unique_ptr<platform::INativeCaptionButtons> _captionButtons;
    // The platform whose frame policy the windows follow, from the appearance services. Declared before
    // _sessionManager, whose window controllers read it.
    platform::FramePlatform _framePlatform = platform::currentFramePlatform();
    session::TerminalSessionManager _sessionManager;
    std::unique_ptr<display::ForcedFontDpiProvider> _forcedFontDpiProvider;
    // Shared by every display, reached via the session; @see keyboardLayout(). Unlike the DPI
    // provider this needs no Qt application, so it is built eagerly with the app.
    std::unique_ptr<input::KeyboardLayout> _keyboardLayout = input::makePlatformKeyboardLayout();
    // Spawn context: the screen the next window should open on (QPointer: screens can be unplugged
    // between staging and consumption).
    QPointer<QScreen> _pendingSpawnScreen;

    /// Applies `renderer.gpu` to OpenGL through the driver's environment (Linux only).
    void applyOpenGlGpuSelection();

    /// Keeps Qt Multimedia from probing the GPUs for hardware video when it loads for the bell
    /// (@see platform::MultimediaHardwareProbeVariables), leaving any variable the user exported alone.
    /// Like the GPU selection, it is the environment of this process only, which shells do not inherit.
    void keepMultimediaOffTheGpus();

    /// Logs the GPU the first window's first frame was drawn on.
    /// @param window The window, before it is first exposed.
    void logGpuInUse(QQuickWindow& window);

    /// Watches @p window's first frame, unless a window is already watched.
    /// @param window The window, before it is first exposed.
    /// @param onFailure Called with the window when it shows no frame in time or its scene graph fails.
    void watchFirstFrame(QQuickWindow& window, std::function<void(QQuickWindow&)> onFailure);

    /// @param verb The verb whose `latency-probe*` options are read: `contour.terminal` or `contour.client`.
    /// @return The probe the options ask for, or nullopt without `latency-probe`.
    [[nodiscard]] std::optional<LatencyProbeRequest> readLatencyProbeRequest(std::string const& verb) const;

    /// Starts the latency probe once the first window shows a terminal display, retrying until it does or
    /// the probe's prompt timeout, counted from the first attempt, ends the run.
    void armLatencyProbe();

    /// Writes the probe report and ends the event loop with the probe's exit code.
    void finishLatencyProbe();

#ifdef CONTOUR_WITH_GPU_SELECTION
    /// Moves @p window to the automatically chosen GPU, after the configured one failed to render on it.
    /// @param window The window that never showed a frame.
    void onGpuFailure(QQuickWindow& window);
#endif

    /// Restarts Contour once on the automatic GPU, after the OpenGL GPU chosen through the driver's
    /// variables failed to render, and quits this process.
    void relaunchOnAutomaticGpu();

    /// Reads (and removes) the restart request relaunchOnAutomaticGpu() leaves in the environment.
    void takeGpuFallbackRequest();

    /// @return `renderer.gpu`, or `auto` when this process is the restart after a failed GPU.
    [[nodiscard]] config::GpuSelector effectiveGpuSelector() const;

    /// Reports a pending fallback from a failed GPU once @p window shows its first frame.
    /// @param window The window.
    void reportGpuFallbackOnceUp(QQuickWindow& window);

    /// Logs and notifies that @p failed could not render and @p used took over, for this session only.
    /// @param failed Title of the GPU that failed.
    /// @param used Title of the GPU now in use.
    void reportGpuFallback(std::string_view failed, std::string_view used);

    std::shared_ptr<display::IGpuInventory const> _gpuInventory;
    std::vector<std::string> _selfOnlyEnvironment;
    QMetaObject::Connection _gpuInUseLog; ///< Set once the first window's GPU report is wired.
    /// The OpenGL GPU renderer.gpu chose explicitly (Linux); unset when the driver decides.
    std::optional<std::string> _openGlGpuTitle;
    /// The GPU that could not render, when this process is Contour's restart without it.
    std::optional<std::string> _gpuFallbackFrom;
    /// A fallback to another GPU, until it has been reported.
    std::optional<display::GpuFallback> _pendingGpuFallback;
    /// The latency probe `--latency-probe` asked for; set by the verb before the GUI boots.
    std::optional<LatencyProbeRequest> _latencyProbeRequest;
    std::unique_ptr<vthost::latency::KeystrokeProbe> _keystrokeProbe;
    std::unique_ptr<display::LatencyProbeDriver> _latencyProbeDriver;
    /// The exit status the finished probe asks for; overrides the session's.
    std::optional<int> _latencyProbeExitCode;
    /// Watches the first window's first frame when `renderer.gpu` names a GPU explicitly.
    std::unique_ptr<display::FirstFrameWatchdog> _firstFrameWatchdog;
    /// Raises the "could not render" notification; created on first use.
    std::unique_ptr<platform::Notifier> _gpuNotifier;

    int _argc = 0;
    char const** _argv = nullptr;

    /// What the daemon-drift badge shows; fed after the native controller connects to a local daemon.
    window::DaemonDriftModel _daemonDrift;
    /// Restarts the local daemon on this release (stop, then start the way a client would); empty
    /// unless this run attaches to a local daemon, since a remote one is not ours to restart. Runs
    /// on @ref _daemonRestartThread, so it touches nothing the GUI thread owns.
    std::function<std::expected<void, std::string>()> _restartDaemon;
    /// Whether a daemon spawned on demand may fork yet. The local attach starts beside the boot, but Apple's
    /// libc deadlocks a fork() made while another thread is inside setlocale(), which QApplication's
    /// construction calls: its at-fork handler and setlocale() take the locale and malloc locks in opposite
    /// orders. So the attach's worker waits, before it spawns, until the QApplication is built.
    std::shared_ptr<std::atomic<DaemonSpawnGate>> _daemonSpawnGate =
        std::make_shared<std::atomic<DaemonSpawnGate>>(DaemonSpawnGate::Closed);
    /// The thread running @ref _restartDaemon; declared after what it reaches so it is joined first.
    std::jthread _daemonRestartThread;
    ExitStatus _exitStatus;

    // The CLI-verbatim/--execute command of this run, if any (see cliCommand()).
    std::optional<vtpty::Process::ExecInfo> _cliCommand;

    /// What the terminal's palette follows: @c _osColorPreference unless the GUI theme pins one.
    vtbackend::ColorPreference _colorPreference = vtbackend::ColorPreference::Dark;
    /// What the OS last said, whatever the GUI theme makes of it.
    vtbackend::ColorPreference _osColorPreference = vtbackend::ColorPreference::Dark;
    /// The GUI theme applyGuiTheme() last applied, after the contrast-theme override.
    config::GuiTheme _effectiveGuiTheme = config::GuiTheme::System;

    /// The OS-provided application palette captured just before the first forced (dark/light) GUI
    /// theme is applied, so @c GuiTheme::System can restore it. Only meaningful while
    /// @c _appliedGuiScheme holds a scheme (see @c applyGuiTheme).
    QPalette _guiSystemPalette;

    /// The scheme the last @c applyGuiTheme forced onto the chrome, or @c std::nullopt while the chrome
    /// follows the OS palette. Guards the one-time capture of @c _guiSystemPalette and its restoration
    /// when returning to System, and lets @c followLiveBackground skip a repaint that changes nothing.
    std::optional<Qt::ColorScheme> _appliedGuiScheme = std::nullopt;

    /// The focused session's live background (OSC 11) as @c followLiveBackground last saw it, or
    /// @c std::nullopt before any. Leaving a contrast theme re-applies the chrome over it.
    std::optional<vtbackend::RGBColor> _liveBackground = std::nullopt;

    /// The contrast theme the last @c applyGuiTheme decided under, so the appearance handler applies the
    /// configured theme again only when a contrast theme comes or goes -- not on an accent or light/dark
    /// change, which must leave a live pin (the settings page's, an OSC 11 background's) in place.
    platform::ContrastTheme _appliedContrastTheme = platform::ContrastTheme::None;

    // The host this process runs on, detected once from the environment at construction unless the appearance
    // services injected one. Declared before
    // the chrome row, which is resolved from it.
    config::HostPlatform _hostPlatform = config::HostPlatform::Other;
    /// The host the shell-integration offer, its editor and Settings' Install rows decide on.
    config::HostPlatform _shellHostPlatform = config::HostPlatform::Other;

    /// The chrome token row, resolved from `ui_style` and the host platform at construction -- so the title
    /// band's tokens are answerable before the GUI exists -- and again once terminalGuiAction() has loaded
    /// the configuration.
    config::ChromeStyle _chromeStyle = config::ChromeStyle::ContourLinux;

    /// The chrome's design tokens, handed to QML as the `chromeStyle` context property and rebuilt
    /// by refreshChromeStyle(). Declared BEFORE the engine so its provider outlives it: a context
    /// property is a borrowed pointer the engine keeps dereferencing, and members are destroyed in
    /// reverse declaration order.
    std::unique_ptr<window::ChromeStyleHost> _chromeStyleHost;

    /// The quick terminal's own `chromeStyle`: the same inputs as _chromeStyleHost's but Minimal density,
    /// published into _quickContext, which every quick-terminal window is created in. Created by the first
    /// openQuickTerminal() and updated by refreshChromeStyle(). Declared BEFORE the engine, as
    /// _chromeStyleHost is, for the same borrowed-context reason.
    std::unique_ptr<window::ChromeStyleHost> _quickChromeStyleHost;

    /// The child of the engine's root context the quick terminal's windows are created in; owned by that
    /// context, so by the engine.
    QPointer<QQmlContext> _quickContext;

    /// What the GUI remembers beside contour.yml (gui-state.yml), shared by the new-defaults banner and the
    /// Settings window; created on first use by guiState(). Declared before both, which borrow it.
    std::shared_ptr<config::GuiStateStore> _guiState;

    /// Whether to offer the newest defaults, handed to QML as the `newDefaults` context property. Built
    /// with the engine, and declared before it for the same borrowed-pointer reason as above.
    std::unique_ptr<window::NewDefaultsNotice> _newDefaultsNotice;

    /// What the window controls are and where they go, handed to QML as the `windowControls` context
    /// property. Declared before the engine for the same borrowed-pointer reason as above.
    std::unique_ptr<window::WindowControlStyleProvider> _windowControlStyleProvider;

    /// Chooses the GPU for Vulkan/Direct3D windows; null for OpenGL. Absent without Qt 6.10. Declared before
    /// _qmlEngine: the windows go first, then the Vulkan instance they borrowed.
#ifdef CONTOUR_WITH_GPU_SELECTION
    std::unique_ptr<display::GraphicsDeviceSelector> _graphicsDeviceSelector;
#endif

    std::unique_ptr<QQmlApplicationEngine> _qmlEngine;

    /// Builds the Settings window on its first show: the injected factory, or one over _qmlEngine.
    window::SettingsWindowFactory _settingsWindowFactory;
    /// The app's one settings bridge, created on first use. Declared after _qmlEngine, so the Settings
    /// window goes before the engine it was built on, and before _settingsWindow, so it outlives the host
    /// that borrows it.
    std::unique_ptr<window::SettingsController> _settingsController;
    /// The Settings window's OS frame, made with the host; declared before _settingsWindow, which borrows it.
    std::unique_ptr<platform::NativeWindowFrame> _settingsFrame;
    /// The app's one Settings window host, created on first use; it owns the window.
    std::unique_ptr<window::SettingsWindowHost> _settingsWindow;
    /// The OS's launcher menu (Dock menu, jump list); null where there is none. Destroyed with the app's Qt
    /// objects.
    std::unique_ptr<platform::LauncherMenu> _launcherMenu;
    /// The Connect picker, created on first use. The window is declared after the controller it binds to, so
    /// it goes first; both go before the engine they were built on.
    std::unique_ptr<window::ConnectPickerController> _connectPicker;
    std::unique_ptr<QQuickWindow> _connectPickerWindow;
    /// How the picker was last shown, which decides what a started connection does to the app.
    PickerPresentation _connectPickerPresentation = PickerPresentation::Launcher;

    // Build the two OS appearance sources, each bound to its change handler. The app keeps these
    // factories and builds each source the first time it is read, not at construction (ruling R-i): the
    // constructor runs for every verb, before any QCoreApplication, and a source on the session bus built
    // there would open the bus in `contour daemon` and before the QGuiApplication. This is memoisation,
    // not a second phase: the app is usable with zero calls, and the first read builds what it reads.
    std::function<std::unique_ptr<platform::IAccessibilityPreferenceSource>()> _makeAccessibilitySource;
    std::function<std::unique_ptr<platform::IPlatformAppearance>()> _makePlatformAppearance;
    // The OS appearance sources the chrome follows, built by the two factories above on first read. Declared
    // LAST -- after _sessionManager, _chromeStyleHost and _qmlEngine -- so they are destroyed FIRST: a source
    // stops reporting once destroyed, so no late change can reach a destroyed host, its engine's
    // QQmlContext, or the session manager. Nothing rebuilds the chrome after the event loop has exited in
    // any case: a provider the host deleteLater()s then is never deleted, because no DeferredDelete pass
    // runs. Mutable: building one on first read does not change what the app reports.
    mutable std::unique_ptr<platform::IAccessibilityPreferenceSource> _accessibilitySource;
    mutable std::unique_ptr<platform::IPlatformAppearance> _platformAppearance;
    // Latches logUnbuiltAppearanceSource(): a read before the sources can be built repeats on every read.
    mutable std::once_flag _unbuiltAppearanceSourceLogged;
    // The native menu presenter, built from the same services; it holds no callback into this app.
    std::unique_ptr<platform::INativeMenuPresenter> _nativeMenus;
    // Where the menu bar lives on this host; it decides which chords a menu may take from the terminal.
    command::MenuBarPlacement _menuBarPlacement = command::MenuBarPlacement::None;
    // Hands the menu bar's Window and Help menus to the OS, built from the same services. Declared before
    // the bar, which borrows it.
    std::unique_ptr<platform::IMenuBarIntegration> _menuBarIntegration;

    /// The shells' startup files. Declared after _hostPlatform and _fileSystem, which the default editor
    /// reads.
    std::unique_ptr<platform::IShellRcEditor> _shellRcEditor;

    /// @see ShellIntegrationServices::contourBinary
    std::filesystem::path _contourBinary;
    /// Where the binary lives, read once from the environment at construction.
    window::BinaryEnvironment _binaryEnvironment;

    // The application's menu bar, under a global placement alone. Built with the QML engine, once a
    // QApplication exists, and released with it.
    std::unique_ptr<window::AppMenuBar> _appMenuBar;
    // The application-wide hotkey and the native side of the quick terminal's window, built from the same
    // services. Declared before the controller, which borrows both.
    std::unique_ptr<platform::IGlobalHotkey> _globalHotkey;
    std::unique_ptr<platform::IQuickTerminalPanel> _quickPanel;
    // The quick terminal. Built with the QML engine, once windows can be made, and released first.
    std::unique_ptr<window::QuickTerminalController> _quickTerminal;
    // The desktop's last refusal of the hotkey, held for the Settings window and the first window to come.
    window::HotkeyRefusalRelay _hotkeyRefusal;
};

} // namespace contour
