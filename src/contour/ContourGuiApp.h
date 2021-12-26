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

    int run(int argc, char const* argv[]) override;
    crispy::cli::Command parameterDefinition() const override;

    TerminalWindow* newWindow();
    TerminalWindow* newWindow(contour::config::Config const& _config);
    void showNotification(std::string_view _title, std::string_view _content);

    std::string profileName() const;

    std::optional<terminal::Process::ExitStatus> exitStatus() const noexcept { return exitStatus_; }

    std::optional<FileSystem::path> dumpStateAtExit() const;

    void onExit(TerminalSession& _session);

  private:
    int terminalGuiAction();
    std::chrono::seconds earlyExitThreshold() const;
    config::Config config_;

    int argc_ = 0;
    char const** argv_ = nullptr;
    std::optional<terminal::Process::ExitStatus> exitStatus_;

    std::list<std::unique_ptr<TerminalWindow>> terminalWindows_;
};

} // namespace contour
