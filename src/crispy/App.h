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

#include <crispy/CLI.h>
#include <crispy/stdfs.h>

#include <functional>
#include <map>
#include <optional>
#include <string>

namespace crispy {

/// General purpose Application main with CLI parameter handling and stuff.
class App
{
  public:
    App(std::string _appName, std::string _appTitle, std::string _appVersion);
    virtual ~App();

    static App* instance() noexcept { return instance_; }

    virtual crispy::cli::Command parameterDefinition() const = 0;
    crispy::cli::FlagStore const& parameters() const noexcept { return flags_.value(); }

    void link(std::string _command, std::function<int()> _handler);

    virtual int run(int argc, char const* argv[]);

    std::string const& appName() const noexcept { return appName_; }
    std::string const& appVersion() const noexcept { return appVersion_; }
    FileSystem::path const& localStateDir() const noexcept { return localStateDir_; }

  protected:
    void listDebugTags();

  private:
    int versionAction();
    int helpAction();

    static App* instance_;

    std::string appName_;
    std::string appTitle_;
    std::string appVersion_;
    FileSystem::path localStateDir_;
    std::optional<crispy::cli::Command> syntax_;
    std::optional<crispy::cli::FlagStore> flags_;
    std::map<std::string, std::function<int()>> handlers_;
};

}
