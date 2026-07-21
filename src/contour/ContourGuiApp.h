// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/CommandHistoryStore.h>
#include <contour/Config.h>
#include <contour/ContourApp.h>
#include <contour/ExitCode.h>
#include <contour/ExternalLauncher.h>
#include <contour/LayoutStore.h>
#include <contour/TerminalSessionManager.h>
#include <contour/helper.h>

#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <QtCore/QPointer>
#include <QtDBus/QDBusVariant>
#include <QtGui/QPalette>
#include <QtGui/QScreen>
#include <QtQml/QQmlApplicationEngine>

#include <filesystem>
#include <memory>
#include <optional>

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

class AttachController;
class RoutingSessionFactory;
class TerminalSession;

/// Extends ContourApp with terminal GUI capability.
class ContourGuiApp: public QObject, public ContourApp
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
    explicit ContourGuiApp(std::unique_ptr<SessionFactory> sessionFactory = nullptr,
                           std::unique_ptr<ExternalLauncher> externalLauncher = nullptr,
                           std::unique_ptr<LayoutStore> layoutStore = nullptr,
                           std::unique_ptr<CommandHistoryStore> commandHistoryStore = nullptr);
    ~ContourGuiApp() override;

    static ContourGuiApp* instance() { return static_cast<ContourGuiApp*>(ContourApp::instance()); }

    int run(int argc, char const* argv[]) override;
    [[nodiscard]] crispy::cli::command parameterDefinition() const override;

    /// Opens a new OS window (loads a fresh main.qml root).
    /// @param targetScreen The screen the new window should open on (the spawning window's screen),
    ///                     or nullptr when unknown (first window); consumed by the window's
    ///                     WindowController::bindWindow() via takePendingSpawnScreen().
    void newWindow(QScreen* targetScreen = nullptr);

    /// Consumes the pending spawn target screen staged by newWindow().
    /// @return The staged screen, or nullptr (already consumed / never staged / screen unplugged).
    [[nodiscard]] QScreen* takePendingSpawnScreen() noexcept;

    /// The app-wide forced-font-DPI provider (single instance; see display/ContentScale.h), created
    /// lazily on first use (requires a constructed QGuiApplication for platform detection).
    /// @return The provider; never nullptr once a Qt application exists.
    [[nodiscard]] display::ForcedFontDpiProvider* forcedFontDpiProvider();

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
    using ExitStatus = SessionExitStatus;

    [[nodiscard]] ExitStatus exitStatus() const noexcept { return _exitStatus; }

    [[nodiscard]] std::optional<std::filesystem::path> dumpStateAtExit() const;

    void onExit(TerminalSession& session);

    config::Config& config() noexcept { return _config; }
    [[nodiscard]] config::Config const& config() const noexcept { return _config; }
    [[nodiscard]] config::TerminalProfile const& profile() const noexcept
    {
        if (auto const* const profile = config().profile(profileName()))
            return *profile;
        displayLog()("Failed to access config profile.");
        Require(false);
    }

    [[nodiscard]] bool liveConfig() const noexcept { return _config.live.value(); }

    TerminalSessionManager& sessionsManager() noexcept { return _sessionManager; }

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
    [[nodiscard]] ExternalLauncher& externalLauncher() noexcept { return *_externalLauncher; }

    [[nodiscard]] static QUrl resolveResource(std::string_view path);

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

    /// The GUI-aware `contour attach` verb: with --gui, boots the QML
    /// machinery attached to a daemon; otherwise falls back to the base's
    /// thin TTY client.
    int attachAction();

    /// The attach engine, declared FIRST so it is destroyed LAST: remote-
    /// backed sessions hold ptys that unregister from it on destruction.
    std::unique_ptr<AttachController> _attachController;

    /// Invoked by terminalGuiAction right after the first window booted —
    /// attach mode adopts the remaining remote sessions as tabs here.
    std::function<void()> _onGuiBooted;

    config::Config _config;
    // Declared before _sessionManager: the manager holds a reference to the factory.
    // Always a RoutingSessionFactory wrapping the injected/default factory,
    // so attach mode can switch the route without touching the manager.
    std::unique_ptr<SessionFactory> _sessionFactory;
    RoutingSessionFactory* _routingFactory = nullptr; ///< The concrete view of _sessionFactory.
    // The external-resource launcher (URL open / process spawn), reached by sessions via _app.
    std::unique_ptr<ExternalLauncher> _externalLauncher;
    // Declared before _sessionManager: the manager holds a reference to the store.
    std::unique_ptr<LayoutStore> _layoutStore;
    // Likewise: the manager holds a reference to the command-history store.
    std::unique_ptr<CommandHistoryStore> _commandHistoryStore;
    TerminalSessionManager _sessionManager;
    std::unique_ptr<display::ForcedFontDpiProvider> _forcedFontDpiProvider;
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

    std::unique_ptr<QQmlApplicationEngine> _qmlEngine;
};

} // namespace contour
