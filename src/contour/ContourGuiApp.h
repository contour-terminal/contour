// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>
#include <contour/ContourApp.h>
#include <contour/ExitCode.h>
#include <contour/ExternalLauncher.h>
#include <contour/TerminalSessionManager.h>
#include <contour/helper.h>

#include <vtpty/Process.h>
#include <vtpty/SshSession.h>

#include <QtCore/QPointer>
#include <QtDBus/QDBusVariant>
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
    explicit ContourGuiApp(std::unique_ptr<SessionFactory> sessionFactory = nullptr,
                           std::unique_ptr<ExternalLauncher> externalLauncher = nullptr);

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
        if (const auto* const profile = config().profile(profileName()))
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

  private:
    static void ensureTermInfoFile();
    void setupQCoreApplication();
    bool loadConfig(std::string const& target);
    int terminalGuiAction();
    int fontConfigAction();
    int checkConfig();

    config::Config _config;
    // Declared before _sessionManager: the manager holds a reference to the factory.
    std::unique_ptr<SessionFactory> _sessionFactory;
    // The external-resource launcher (URL open / process spawn), reached by sessions via _app.
    std::unique_ptr<ExternalLauncher> _externalLauncher;
    TerminalSessionManager _sessionManager;
    std::unique_ptr<display::ForcedFontDpiProvider> _forcedFontDpiProvider;
    // Spawn context: the screen the next window should open on (QPointer: screens can be unplugged
    // between staging and consumption).
    QPointer<QScreen> _pendingSpawnScreen;

    int _argc = 0;
    char const** _argv = nullptr;
    ExitStatus _exitStatus;

    vtbackend::ColorPreference _colorPreference = vtbackend::ColorPreference::Dark;

    std::unique_ptr<QQmlApplicationEngine> _qmlEngine;
};

} // namespace contour
