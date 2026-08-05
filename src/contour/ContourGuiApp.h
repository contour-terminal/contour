// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/UiStyleProvider.h>
#include <contour/cli/ContourApp.h>
#include <contour/command/CommandHistoryStore.h>
#include <contour/config/Config.h>
#include <contour/config/LayoutStore.h>
#include <contour/display/Logging.h>
#include <contour/platform/ExternalLauncher.h>
#include <contour/platform/SpeechSynthesizer.h>
#include <contour/session/ExitCode.h>
#include <contour/session/TerminalSessionManager.h>

#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <QtCore/QPointer>
#include <QtDBus/QDBusVariant>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace contour
{

namespace config
{
    struct Config;
}

namespace display
{
    class ForcedFontDpiProvider;
}

class NativeController;
class RoutingSessionFactory;
namespace session
{
    class TerminalSession;
}
class TmuxController;
class WindowController;

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
    ///                       wires the production QtExternalLauncher; tests pass a recording launcher
    ///                       to assert open-document / follow-hyperlink / spawn routing without
    ///                       actually launching anything.
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
    /// @param env            The process environment every part of the application reads through.
    explicit ContourGuiApp(crispy::environment const& env,
                           std::unique_ptr<session::SessionFactory> sessionFactory = nullptr,
                           std::unique_ptr<platform::ExternalLauncher> externalLauncher = nullptr,
                           std::unique_ptr<config::LayoutStore> layoutStore = nullptr,
                           std::unique_ptr<command::CommandHistoryStore> commandHistoryStore = nullptr,
                           std::unique_ptr<platform::SpeechSynthesizer> speechSynthesizer = nullptr);
    ~ContourGuiApp() override;

    static ContourGuiApp* instance() { return static_cast<ContourGuiApp*>(cli::ContourApp::instance()); }

    int run(int argc, char const* argv[]) override;
    [[nodiscard]] crispy::cli::command parameterDefinition() const override;

    /// Opens a new OS window (loads a fresh Main.qml root).
    /// @param targetScreen The screen the new window should open on (the spawning window's screen),
    ///                     or nullptr when unknown (first window); consumed by the window's
    ///                     WindowController::bindWindow() via takePendingSpawnScreen().
    void newWindow(QScreen* targetScreen = nullptr);

    /// Consumes the pending spawn target screen staged by newWindow().
    /// @return The staged screen, or nullptr (already consumed / never staged / screen unplugged).
    [[nodiscard]] QScreen* takePendingSpawnScreen() noexcept;

    /// Attach mode: authors a new window on the daemon (routed through the session factory's
    /// requestRemoteWindow) instead of opening a purely local one. The daemon's new-window layout push
    /// then spawns and binds the matching OS window (B4). Called by the NewTerminalWindow action.
    /// @return true if the request was routed to the daemon (do NOT open a local window); false for a
    ///         local factory.
    [[nodiscard]] bool requestRemoteWindow();

    /// The app-wide forced-font-DPI provider (single instance; see display/ContentScale.h), created
    /// lazily on first use (requires a constructed QGuiApplication for platform detection).
    /// @return The provider; never nullptr once a Qt application exists.
    [[nodiscard]] display::ForcedFontDpiProvider* forcedFontDpiProvider();

    /// The app-wide keyboard layout (single instance; see KeyboardLayout.h). The active layout is
    /// process-global ambient state, and the macOS implementation registers an observer for changes
    /// to it, so one instance is shared rather than one per pane.
    /// @return The layout; never nullptr.
    [[nodiscard]] input::KeyboardLayout const& keyboardLayout() const noexcept { return *_keyboardLayout; }

    [[nodiscard]] std::string profileName() const;

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

    [[nodiscard]] vtbackend::ColorPreference colorPreference() const noexcept { return _colorPreference; }

    /// Applies the configured GUI chrome theme (dark/light/system) to the application's color
    /// scheme, recoloring the Qt chrome (title bar, tab strip, command palette, settings, dialogs).
    ///
    /// @c GuiTheme::System defers to the operating system's color scheme; @c Dark / @c Light force
    /// the appearance regardless of the OS. This affects the GUI chrome only: the terminal grid's
    /// light/dark preference is derived from the OS independently of this setting, so a forced GUI
    /// theme never drags the grid with it. Note that forcing a theme pins the shared @c QStyleHints
    /// color scheme, so while pinned the grid reflects the OS scheme captured at startup; a live OS
    /// switch is tracked only in @c System mode (see the @c colorSchemeChanged handler in @c run()).
    /// Safe to call at startup and live (e.g. from the settings page).
    /// @param theme The GUI theme to apply.
    /// @note On Qt older than 6.8 (no @c QStyleHints::setColorScheme) this is a no-op and the GUI
    ///       follows the OS unconditionally.
    void applyGuiTheme(config::GuiTheme theme);

  private:
    static void ensureTermInfoFile();
    void setupQCoreApplication();
    bool loadConfig(std::string const& target);
    int terminalGuiAction();
    int fontConfigAction();
    int checkConfig();

    /// The `contour client` verb: boots the QML machinery connected to a
    /// daemon or tmux server.
    int clientAction();

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
    bool bindPendingAttachWindow(WindowController* controller);

    /// Records the daemon-window → OS-window mapping and immediately reconciles that daemon window's
    /// tab/pane tree into @p osWindow — the one place the "a mapping always implies a reconcile"
    /// invariant lives (shared by the primary-window and spawned-window paths).
    /// @param daemonWindow The daemon window id.
    /// @param osWindow The OS window that mirrors it.
    void bindDaemonWindow(std::uint64_t daemonWindow, vtworkspace::WindowId osWindow);

    /// The attach engines, declared FIRST so they are destroyed LAST: remote-
    /// backed sessions hold ptys that unregister from them on destruction.
    std::unique_ptr<NativeController> _nativeController;
    std::unique_ptr<TmuxController> _tmuxController;

    /// Invoked by terminalGuiAction right after the first window booted —
    /// attach mode adopts the remaining remote sessions as tabs here.
    std::function<void()> _onGuiBooted;

    /// Native attach (B4): each daemon window id mapped to the OS window that mirrors it. The primary
    /// daemon window maps to the boot window; additional ones map to spawned windows.
    std::unordered_map<uint64_t, vtworkspace::WindowId> _attachWindowMap;
    /// The daemon window whose OS window is being spawned right now: reconcileAttachWindows stages it
    /// just before newWindow(), and that window's Main.qml claims it via bindPendingAttachWindow.
    /// Single-slot (like _pendingTransplant / _pendingSpawnScreen): the QML load is synchronous, so a
    /// stage is always consumed before the next is made.
    std::optional<uint64_t> _pendingAttachWindow;

    config::Config _config;
    // Declared before _sessionManager: the manager holds a reference to the factory.
    // Always a RoutingSessionFactory wrapping the injected/default factory,
    // so attach mode can switch the route without touching the manager.
    std::unique_ptr<session::SessionFactory> _sessionFactory;
    RoutingSessionFactory* _routingFactory = nullptr; ///< The concrete view of _sessionFactory.
    // The external-resource launcher (URL open / process spawn), reached by sessions via _app.
    std::unique_ptr<platform::ExternalLauncher> _externalLauncher;
    // Declared before _sessionManager: the manager holds a reference to the store.
    std::unique_ptr<config::LayoutStore> _layoutStore;
    // Likewise: the manager holds a reference to the command-history store.
    std::unique_ptr<command::CommandHistoryStore> _commandHistoryStore;
    // Shared by every session, reached via _app; @see speechSynthesizer().
    std::unique_ptr<platform::SpeechSynthesizer> _speechSynthesizer;
    session::TerminalSessionManager _sessionManager;
    std::unique_ptr<display::ForcedFontDpiProvider> _forcedFontDpiProvider;
    // Shared by every display, reached via the session; @see keyboardLayout(). Unlike the DPI
    // provider this needs no Qt application, so it is built eagerly with the app.
    std::unique_ptr<input::KeyboardLayout> _keyboardLayout = input::makePlatformKeyboardLayout();
    // Spawn context: the screen the next window should open on (QPointer: screens can be unplugged
    // between staging and consumption).
    QPointer<QScreen> _pendingSpawnScreen;

    int _argc = 0;
    char const** _argv = nullptr;
    ExitStatus _exitStatus;

    // The CLI-verbatim/--execute command of this run, if any (see cliCommand()).
    std::optional<vtpty::Process::ExecInfo> _cliCommand;

    vtbackend::ColorPreference _colorPreference = vtbackend::ColorPreference::Dark;

    /// The OS-provided application palette captured just before the first forced (dark/light) GUI
    /// theme is applied, so @c GuiTheme::System can restore it. Only meaningful while
    /// @c _guiPaletteOverridden is @c true (see @c applyGuiTheme).
    QPalette _guiSystemPalette;

    /// Whether a forced (dark/light) GUI theme currently overrides the OS application palette. Guards
    /// the one-time capture of @c _guiSystemPalette and its restoration when returning to System.
    bool _guiPaletteOverridden = false;

    /// The chrome's design tokens, handed to QML as the `chromeStyle` context property. Declared
    /// BEFORE the engine so it outlives it: a context property is a borrowed pointer the engine keeps
    /// dereferencing, and members are destroyed in reverse declaration order.
    std::unique_ptr<UiStyleProvider> _uiStyleProvider;

    std::unique_ptr<QQmlApplicationEngine> _qmlEngine;
};

} // namespace contour
