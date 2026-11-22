// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <core/Environment.hpp>
#include <core/platform/Clock.hpp>
#include <core/testing/ScopedTempDir.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <future>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace vtextension::testing
{

/// A process environment, as name and value pairs.
using GitEnvironment = std::vector<std::pair<std::string, std::string>>;

/// How one git run ended, and what it printed.
struct GitResult
{
    std::optional<int32_t> exitCode; ///< Its exit status; std::nullopt when it did not exit by itself.
    std::string standardOutput;      ///< What it wrote to standard output.
    std::string standardError;       ///< What it wrote to standard error.
};

/// Why a GitSandbox could not be made.
enum class GitSandboxError : uint8_t
{
    GitNotFound,       ///< No executable `git` on PATH.
    Unsupported,       ///< This platform has no directory handles, so nothing can be run in a directory.
    RunnerUnavailable, ///< The process runner could not be made.
    SetupFailed,       ///< A git command that builds the repository failed.
};

/// A git repository with one linked worktree in a temporary directory, and git run there with none of the
/// user's configuration: `HOME` and `XDG_CONFIG_HOME` point into the sandbox, `GIT_CONFIG_GLOBAL` names an
/// empty file there and `GIT_CONFIG_NOSYSTEM` is set; nothing else of this process's environment but
/// `PATH` reaches git.
///
/// Layout, beneath root():
/// - `home/` (`HOME`), `home/.config/` (`XDG_CONFIG_HOME`), `home/gitconfig` (`GIT_CONFIG_GLOBAL`);
/// - `repos/main/` — the repository: branch `main`, one commit of `f` (`a\n`), and the clean filter
///   `sentinel` defined in its config, which creates `sentinel` (beside `home/`) before passing its input
///   through. Nothing selects that filter until a test writes an attribute naming it.
/// - `worktrees/` — the worktree root, holding `wt/`: a linked worktree of `main`, its HEAD detached.
class GitSandbox
{
    /// Lets only create() construct one, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// Makes the sandbox and its repository.
    /// @param environment Where `PATH` is read from: git is looked up there, and runs with it.
    /// @return The sandbox; or why it could not be made — `GitNotFound` is for the caller to skip on.
    [[nodiscard]] static std::expected<std::unique_ptr<GitSandbox>, GitSandboxError> create(
        core::Environment const& environment)
    {
        auto const searchPath = environment.get("PATH").value_or(std::string {});
        if (!onPath("git", searchPath))
            return std::unexpected(GitSandboxError::GitNotFound);
        auto runner = vtpty::ThreadedProcessRunner::create(core::platform::defaultSteadyClock(), {});
        if (!runner)
            return std::unexpected(GitSandboxError::RunnerUnavailable);
        auto sandbox = std::make_unique<GitSandbox>(Key {}, searchPath, std::move(*runner));
        if (auto const opened = vtpty::nativeDirectoryOpener().open(sandbox->root().string()); !opened)
            return std::unexpected(opened.error() == vtpty::DirectoryError::Unsupported
                                       ? GitSandboxError::Unsupported
                                       : GitSandboxError::SetupFailed);
        if (!sandbox->build())
            return std::unexpected(GitSandboxError::SetupFailed);
        return sandbox;
    }

    /// For create() only: lays out the directories, runs no git.
    GitSandbox(Key /*key*/, std::string searchPath, std::unique_ptr<vtpty::ThreadedProcessRunner> runner):
        _directory { "contour-git-sandbox" },
        _root { std::filesystem::canonical(_directory.path()) },
        _runner { std::move(runner) },
        _environment {
            { "PATH", std::move(searchPath) },
            { "HOME", (_root / "home").string() },
            { "XDG_CONFIG_HOME", (_root / "home" / ".config").string() },
            { "GIT_CONFIG_NOSYSTEM", "1" },
            { "GIT_CONFIG_GLOBAL", (_root / "home" / "gitconfig").string() },
            { "GIT_CEILING_DIRECTORIES", _root.string() },
            { "LC_ALL", "C" },
            { "GIT_AUTHOR_NAME", "agent" },
            { "GIT_AUTHOR_EMAIL", "agent@example.invalid" },
            { "GIT_COMMITTER_NAME", "agent" },
            { "GIT_COMMITTER_EMAIL", "agent@example.invalid" },
            { "GIT_EDITOR", "true" },
            { "GIT_MERGE_AUTOEDIT", "no" },
        }
    {
        std::filesystem::create_directories(xdgConfigHome());
        std::filesystem::create_directories(repository());
        std::filesystem::create_directories(worktreeRoot());
        writeFile(_root / "home" / "gitconfig", "");
    }

    /// @return The sandbox's directory, symlinks resolved.
    [[nodiscard]] std::filesystem::path const& root() const noexcept { return _root; }
    /// @return `XDG_CONFIG_HOME` as git sees it.
    [[nodiscard]] std::filesystem::path xdgConfigHome() const { return _root / "home" / ".config"; }
    /// @return The repository's checkout.
    [[nodiscard]] std::filesystem::path repository() const { return _root / "repos" / "main"; }
    /// @return The directory linked worktrees are added to.
    [[nodiscard]] std::filesystem::path worktreeRoot() const { return _root / "worktrees"; }
    /// @return The linked worktree.
    [[nodiscard]] std::filesystem::path worktree() const { return worktreeRoot() / "wt"; }
    /// @return The file the `sentinel` clean filter creates when it runs.
    [[nodiscard]] std::filesystem::path sentinel() const { return _root / "sentinel"; }

    /// @return The environment every git the sandbox runs gets, whole.
    [[nodiscard]] GitEnvironment const& environment() const noexcept { return _environment; }

    /// @return The environment as `NAME=value` entries, as a process's own environment reads.
    [[nodiscard]] std::vector<std::string> environmentEntries() const
    {
        auto entries = std::vector<std::string> {};
        for (auto const& [name, value]: _environment)
            entries.push_back(std::format("{}={}", name, value));
        return entries;
    }

    /// @return The runner the sandbox runs git through; it outlives every process started on it.
    [[nodiscard]] vtpty::IProcessRunner& runner() const noexcept { return *_runner; }

    /// Runs @p argv in @p directory with the sandbox's environment, and waits for it to end.
    /// @param directory Where it runs.
    /// @param argv The program and its arguments.
    /// @param extra Variables added to the environment, replacing one of the same name.
    /// @return How it ended; exitCode std::nullopt and the reason in standardError when it could not start.
    [[nodiscard]] GitResult runGit(std::filesystem::path const& directory,
                                   std::vector<std::string> argv,
                                   GitEnvironment const& extra = {}) const
    {
        auto opened = vtpty::nativeDirectoryOpener().open(directory.string());
        if (!opened)
            return GitResult { .exitCode = std::nullopt,
                               .standardOutput = {},
                               .standardError = std::format("cannot open {}", directory.string()) };
        auto environment = _environment;
        for (auto const& [name, value]: extra)
        {
            auto const same = std::ranges::find(environment, name, &GitEnvironment::value_type::first);
            if (same != environment.end())
                same->second = value;
            else
                environment.emplace_back(name, value);
        }
        auto ended = std::promise<vtpty::ProcessExit> {};
        auto exit = ended.get_future();
        auto const started = _runner->start(
            vtpty::ProcessSpec { .argv = std::move(argv),
                                 .workingDirectory = std::move(*opened),
                                 .environment = std::move(environment),
                                 .timeout = std::chrono::minutes { 2 },
                                 .outputCap = std::size_t { 1 } << 20U,
                                 .standardOutput = vtpty::StreamCapture::Capture,
                                 .standardError = vtpty::StreamCapture::Capture },
            [&ended](vtpty::ProcessToken, vtpty::ProcessExit result) { ended.set_value(std::move(result)); });
        if (!started)
            return GitResult { .exitCode = std::nullopt,
                               .standardOutput = {},
                               .standardError = "not started" };
        auto result = exit.get();
        return GitResult { .exitCode = result.exitCode,
                           .standardOutput = std::move(result.standardOutput.bytes),
                           .standardError = std::move(result.standardError.bytes) };
    }

    /// @return What `git --version` prints, without its line end.
    [[nodiscard]] std::string gitVersion() const
    {
        auto version = runGit(_root, { "git", "--version" }).standardOutput;
        while (version.ends_with('\n'))
            version.pop_back();
        return version;
    }

    /// @return What `git config --system --list` prints (the system configuration git would read without
    ///         `GIT_CONFIG_NOSYSTEM`, as the declared commands do), or why it printed nothing.
    [[nodiscard]] std::string systemConfiguration() const
    {
        auto listed =
            runGit(_root, { "git", "config", "--system", "--list" }, { { "GIT_CONFIG_NOSYSTEM", "0" } });
        auto text = listed.exitCode == 0 ? std::move(listed.standardOutput) : std::move(listed.standardError);
        while (text.ends_with('\n'))
            text.pop_back();
        if (listed.exitCode != 0)
            return std::format("none ({})", text);
        return text.empty() ? std::string("empty") : text;
    }

    /// Writes @p text to @p path, replacing what was there and creating its directory.
    static void writeFile(std::filesystem::path const& path, std::string_view text)
    {
        std::filesystem::create_directories(path.parent_path());
        auto file = std::ofstream(path, std::ios::binary | std::ios::trunc);
        file << text;
    }

    /// Moves @p path's modification time a minute on, leaving its content alone: git, finding the stat
    /// information it recorded no longer matching, reads the file again — through its clean filter.
    static void makeStatDirty(std::filesystem::path const& path)
    {
        std::filesystem::last_write_time(path,
                                         std::filesystem::last_write_time(path) + std::chrono::minutes { 1 });
    }

  private:
    /// @return Whether an executable @p program is in one of @p searchPath's directories.
    [[nodiscard]] static bool onPath(std::string_view program, std::string_view searchPath)
    {
        for (auto const part: std::views::split(searchPath, ':'))
        {
            auto const directory = std::string_view(part.begin(), part.end());
            if (directory.empty())
                continue;
            auto const candidate = std::filesystem::path(directory) / program;
            auto error = std::error_code {};
            auto const status = std::filesystem::status(candidate, error);
            if (!error && std::filesystem::is_regular_file(status)
                && (status.permissions() & std::filesystem::perms::owner_exec)
                       != std::filesystem::perms::none)
                return true;
        }
        return false;
    }

    /// Creates the repository, its first commit, the filter's definition and the linked worktree.
    /// @return Whether every step succeeded.
    [[nodiscard]] bool build() const
    {
        if (runGit(_root, { "git", "init", "-q", "-b", "main", repository().string() }).exitCode != 0)
            return false;
        writeFile(repository() / "f", "a\n");
        auto const filter = std::format("touch '{}' && cat", sentinel().string());
        auto const steps = std::vector<std::vector<std::string>> {
            { "git", "add", "f" },
            { "git", "commit", "-q", "-m", "a" },
            { "git", "config", "filter.sentinel.clean", filter },
            { "git", "worktree", "add", "-q", "--detach", worktree().string(), "main" },
        };
        return std::ranges::all_of(
            steps, [this](auto const& argv) { return runGit(repository(), argv).exitCode == 0; });
    }

    core::testing::ScopedTempDir _directory;
    std::filesystem::path _root;
    std::unique_ptr<vtpty::ThreadedProcessRunner> _runner;
    GitEnvironment _environment;
};

} // namespace vtextension::testing
