/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <contour/Config.h>
#include <contour/ContourApp.h>
#include <contour/TerminalSessionManager.h>

#include <terminal/Process.h>

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
class TerminalWindow;

/// Extends ContourApp with terminal GUI capability.
class ContourGuiApp: public ContourApp
{
  public:
    ContourGuiApp();
    ~ContourGuiApp() override;

    static ContourGuiApp* instance() { return static_cast<ContourGuiApp*>(ContourApp::instance()); }

    int run(int argc, char const* argv[]) override;
    crispy::cli::Command parameterDefinition() const override;

    TerminalWindow* newWindow();
    TerminalWindow* newWindow(contour::config::Config const& _config);
    void showNotification(std::string_view _title, std::string_view _content);

    std::string profileName() const;

    std::optional<terminal::Process::ExitStatus> exitStatus() const noexcept { return _exitStatus; }

    std::optional<FileSystem::path> dumpStateAtExit() const;

    void onExit(TerminalSession& _session);

    config::Config& config() noexcept { return _config; }
    config::Config const& config() const noexcept { return _config; }
    config::TerminalProfile const& profile() const noexcept
    {
        if (auto profile = config().profile(profileName()))
            return *profile;
        fmt::print("Failed to access config profile.\n");
        Require(false);
    }
    bool liveConfig() const noexcept { return parameters().boolean("contour.terminal.live-config"); }

    TerminalSessionManager& sessionsManager() noexcept { return _sessionManager; }

    std::chrono::seconds earlyExitThreshold() const;

    std::string programPath() const { return _argv[0]; }

  private:
    void ensureTermInfoFile();
    bool loadConfig(std::string const& target);
    int terminalGuiAction();
    int fontConfigAction();

    config::Config _config;
    TerminalSessionManager _sessionManager;

    int _argc = 0;
    char const** _argv = nullptr;
    std::optional<terminal::Process::ExitStatus> _exitStatus;

    std::list<TerminalWindow*> _terminalWindows;
};

} // namespace contour
