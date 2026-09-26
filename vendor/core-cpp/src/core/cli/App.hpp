// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Environment.hpp>
#include <core/cli/CLI.hpp>
#include <core/log/LogSink.hpp>

#include <expected>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace core::cli
{

/// General purpose Application main with CLI parameter handling and stuff.
class App
{
  public:
    /// @param env    The process environment to read from. It must outlive this object, and is the
    ///               one every part of the application derives its own from -- an app is the
    ///               process's composition root, so this is where the choice is made once.
    /// @param appName    Program name, as the CLI spells it.
    /// @param appTitle   Human-readable title.
    /// @param appVersion Version string.
    /// @param appLicense SPDX license identifier.
    App(Environment const& env,
        std::string appName,
        std::string appTitle,
        std::string appVersion,
        std::string appLicense);
    virtual ~App();

    /// @return The application constructed last and not yet destroyed, or nullptr.
    [[nodiscard]] static App* instance() noexcept;

    /// @return The command-line syntax of this application, which run() parses argv against.
    [[nodiscard]] virtual Command parameterDefinition() const = 0;
    /// @return The parsed command line; valid once run() or reparseParameters() has parsed it.
    [[nodiscard]] FlagStore const& parameters() const noexcept { return _flags.value(); }

    void link(std::string command, std::function<int()> handler);

    virtual int run(int argc, char const* argv[]);

    /// Parses @p argv into the parameter store WITHOUT dispatching to any command handler, populating
    /// parameters() with the syntax's defaults plus whatever @p argv overrides — the same parse step
    /// run() performs internally. An action that re-enters another verb's code path re-parses so
    /// that verb's parameter surface resolves.
    /// @param argc Argument count.
    /// @param argv Argument vector (argv[0] is the program name).
    /// @return true if parsing succeeded (parameters() is now populated), false otherwise.
    [[nodiscard]] bool reparseParameters(int argc, char const* argv[]);

    /// The test-facing alias of reparseParameters(): gives an app instance a fully-formed
    /// parameters() without dispatching to any command handler.
    [[nodiscard]] bool parseParametersForTesting(int argc, char const* argv[]);

    /// The environment this application was constructed with, for the collaborators it builds.
    /// @return A reference to it; it outlives this object.
    [[nodiscard]] Environment const& processEnvironment() const noexcept { return _environment; }

    [[nodiscard]] std::string const& appName() const noexcept { return _appName; }
    [[nodiscard]] std::string const& appVersion() const noexcept { return _appVersion; }
    [[nodiscard]] std::filesystem::path const& localStateDir() const noexcept { return _localStateDir; }

    /// This process's own argv, as run() received it (argv[0] first).
    ///
    /// For a verb that must re-launch THIS binary with THIS configuration, a daemon respawning
    /// itself detached for instance. Replaying the original tokens is exact by construction: rebuilding them
    /// from the parsed flags means every option added later has to be remembered in a second place, and the
    /// ones that were forgotten are invisible until someone uses them together.
    /// @return The argument vector; empty before run() has been entered.
    [[nodiscard]] std::vector<std::string> const& commandLine() const noexcept { return _commandLine; }

    static void customizeLogStoreOutput();

    /// Installs this process's log destination from a verb's `--log` / `--log-file` options,
    /// keeping it alive for the rest of the app's lifetime, and reports any filter pattern that
    /// names no known category.
    ///
    /// One implementation so every verb behaves identically: when this was inlined at each
    /// verb the two copies had already drifted — only one of them reported a mistyped tag.
    /// @param optionPrefix The verb's dotted flag prefix, e.g. "tool.serve".
    /// @param showProcessId Whether to prefix each line with the emitting process id.
    /// @return Nothing, or why the log file could not be opened.
    [[nodiscard]] std::expected<void, std::string> installLogging(std::string const& optionPrefix,
                                                                  bool showProcessId = false);

  protected:
    static void listDebugTags();

  private:
    int versionAction();
    int licenseAction();
    int helpAction();

    Environment const& _environment;
    std::string _appName;
    std::string _appTitle;
    std::string _appVersion;
    std::string _appLicense;
    std::vector<std::string> _commandLine; ///< argv as run() received it; see commandLine().
    std::filesystem::path _localStateDir;
    /// The installed log destination; categories hold a reference into it, so it must outlive
    /// every one of them — hence a member of the app rather than a local of a verb handler.
    std::unique_ptr<core::log::ScopedOutput> _logOutput;
    std::optional<Command> _syntax;
    std::optional<FlagStore> _flags;
    std::map<std::string, std::function<int()>> _handlers;
};

} // namespace core::cli
