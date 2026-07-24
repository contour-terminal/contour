// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <crispy/CLI.h>

#include <filesystem>
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

    /// Parses @p argv into the parameter store WITHOUT dispatching to any command handler, populating
    /// parameters() with the syntax's defaults plus whatever @p argv overrides — the same parse step
    /// run() performs internally. An action that re-enters another verb's code path (e.g. the GUI
    /// boot under `attach --gui`) re-parses so that verb's parameter surface resolves.
    /// @param argc Argument count.
    /// @param argv Argument vector (argv[0] is the program name).
    /// @return true if parsing succeeded (parameters() is now populated), false otherwise.
    [[nodiscard]] bool reparseParameters(int argc, char const* argv[]);

    /// The test-facing alias of reparseParameters(): constructs an app instance with a fully-formed
    /// parameters() (e.g. so a default profile resolves) without launching the GUI event loop.
    [[nodiscard]] bool parseParametersForTesting(int argc, char const* argv[]);

    [[nodiscard]] std::string const& appName() const noexcept { return _appName; }
    [[nodiscard]] std::string const& appVersion() const noexcept { return _appVersion; }
    [[nodiscard]] std::filesystem::path const& localStateDir() const noexcept { return _localStateDir; }

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
    std::filesystem::path _localStateDir;
    std::optional<crispy::cli::command> _syntax;
    std::optional<crispy::cli::flag_store> _flags;
    std::map<std::string, std::function<int()>> _handlers;
};

} // namespace crispy
