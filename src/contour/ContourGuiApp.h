// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/Config.h>
#include <contour/ContourApp.h>
#include <contour/TerminalSessionManager.h>

#include <vtpty/Process.h>

#include <QtQml/QQmlApplicationEngine>

#include <filesystem>
#include <list>
#include <memory>
#include <optional>
#include <string_view>

namespace contour
{

namespace config
{
    struct Config;
}

class TerminalSession;

/// Extends ContourApp with terminal GUI capability.
class ContourGuiApp: public QObject, public ContourApp
{
    Q_OBJECT

  public:
    ContourGuiApp();

    static ContourGuiApp* instance() { return static_cast<ContourGuiApp*>(ContourApp::instance()); }

    int run(int argc, char const* argv[]) override;
    crispy::cli::command parameterDefinition() const override;

    void newWindow();
    static void showNotification(std::string_view title, std::string_view content);

    std::string profileName() const;

    std::optional<vtpty::Process::ExitStatus> exitStatus() const noexcept { return _exitStatus; }

    std::optional<std::filesystem::path> dumpStateAtExit() const;

    void onExit(TerminalSession& session);

    config::Config& config() noexcept { return _config; }
    config::Config const& config() const noexcept { return _config; }
    config::TerminalProfile const& profile() const noexcept
    {
        if (const auto* const profile = config().profile(profileName()))
            return *profile;
        fmt::print("Failed to access config profile.\n");
        Require(false);
    }

    [[nodiscard]] bool liveConfig() const noexcept { return _config.live; }

    TerminalSessionManager& sessionsManager() noexcept { return _sessionManager; }

    std::chrono::seconds earlyExitThreshold() const;

    std::string programPath() const { return _argv[0]; }

    [[nodiscard]] static QUrl resolveResource(std::string_view path);

  private:
    static void ensureTermInfoFile();
    bool loadConfig(std::string const& target);
    int terminalGuiAction();
    int fontConfigAction();

    config::Config _config;
    TerminalSessionManager _sessionManager;

    int _argc = 0;
    char const** _argv = nullptr;
    std::optional<vtpty::Process::ExitStatus> _exitStatus;

    std::unique_ptr<QQmlApplicationEngine> _qmlEngine;
};

} // namespace contour
