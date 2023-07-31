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

namespace crispy
{

/// General purpose Application main with CLI parameter handling and stuff.
class app
{
  public:
    app(std::string appName, std::string appTitle, std::string appVersion, std::string appLicense);
    virtual ~app();

    static app* instance() noexcept { return _instance; }

    [[nodiscard]] virtual crispy::cli::command parameterDefinition() const = 0;
    [[nodiscard]] cli::flag_store const& parameters() const noexcept { return _flags.value(); }

    void link(std::string command, std::function<int()> handler);

    virtual int run(int argc, char const* argv[]);

    [[nodiscard]] std::string const& appName() const noexcept { return _appName; }
    [[nodiscard]] std::string const& appVersion() const noexcept { return _appVersion; }
    [[nodiscard]] FileSystem::path const& localStateDir() const noexcept { return _localStateDir; }

    static void customizeLogStoreOutput();

  protected:
    static void listDebugTags();

  private:
    int versionAction();
    int licenseAction();
    int helpAction();

    static app* _instance; // NOLINT(readability-identifier-naming)

    std::string _appName;
    std::string _appTitle;
    std::string _appVersion;
    std::string _appLicense;
    FileSystem::path _localStateDir;
    std::optional<crispy::cli::command> _syntax;
    std::optional<crispy::cli::flag_store> _flags;
    std::map<std::string, std::function<int()>> _handlers;
};

} // namespace crispy
