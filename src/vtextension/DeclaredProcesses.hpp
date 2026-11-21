// SPDX-License-Identifier: Apache-2.0
//
// The policy between a daemon-world extension and the processes it may run: only a command its manifest
// declares, granted by the user, with arguments of their declared kinds, in a directory of the command's
// root, with an environment built from the daemon's own less a deny list plus the declared values.
#pragma once

#include <vtpty/DirectoryHandle.hpp>
#include <vtpty/ProcessRunner.hpp>

#include <chrono>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <vtextension/CommandPolicy.hpp>
#include <vtextension/DaemonWorld.hpp>
#include <vtextension/Manifest.hpp>
#include <vtextension/ProcessService.hpp>
#include <vtextension/RootResolution.hpp>

namespace vtextension
{

/// How long a declared command may run, by its timeout class.
struct ProcessTimeouts
{
    std::chrono::milliseconds shortClass { 30'000 }; ///< A command declared `short`.
    std::chrono::milliseconds longClass { 600'000 }; ///< A command declared `long`.
};

/// How much an extension's processes may hold.
struct ProcessLimits
{
    std::size_t maxRunning = 8;       ///< Processes running at once.
    std::size_t maxUntaken = 32;      ///< Finished results kept and not released.
    std::size_t streamCap = 1 << 20;  ///< Bytes kept of one stream; the rest is read and dropped.
    std::size_t processCap = 2 << 20; ///< Bytes kept of one process's two streams together.
    std::size_t totalCap = 16 << 20;  ///< Bytes kept of every finished result; the oldest go first past it.
    std::size_t pageBytes = 64 << 10; ///< Bytes one take() returns at most.
};

/// What a DeclaredProcesses is: one extension's declarations and the daemon's facts about it.
struct DeclaredProcessesConfig
{
    Manifest manifest;                             ///< The extension's manifest: its commands and arguments.
    std::vector<ResolvedRoot> roots;               ///< Its roots, resolved; distinct (checkRootsDistinct()).
    std::vector<std::string> inheritedEnvironment; ///< The daemon's environment, as `NAME=value`.
    std::string hostId;                            ///< What the grant store keys the installation on.
    std::string grantDigest;                       ///< The digest the grant must be recorded for.
    ProcessTimeouts timeouts {};                   ///< How long each timeout class may run.
    ProcessLimits limits {};                       ///< How much the processes may hold.
    std::vector<SelfProgram>
        selfPrograms = {}; ///< The programs a command runs as the daemon's own executable.
};

/// What a DeclaredProcesses reaches the world through; each must outlive it.
struct DeclaredProcessesServices
{
    vtpty::IProcessRunner& runner;        ///< Runs the processes.
    vtpty::IDirectoryOpener& directories; ///< Opens working directories and resolves path arguments.
    IGrantStore const& grants;            ///< Holds the user's answer.
    /// Called once per process that finishes, on the runner's thread: posts the `procs` hint. It must not
    /// destroy the service, whose destructor waits for it to return.
    std::function<void()> onFinished;
};

/// One extension's declared processes. Thread-safe: completions arrive on the runner's thread.
///
/// start() checks, in this order: the command is declared; the user granted it; every placeholder has a value
/// and no other name is passed; each value is of its declared kind (a `path:<root>` value also resolves
/// inside its root, and the resolved path is what the command is given); the limits; a missing root declared
/// `create` is created; the working directory is opened and belongs to the command's root. A substituted
/// value stays one argv word, and no shell is involved. A command whose program is one of the configuration's
/// self programs runs the daemon's own executable (resolveSelfProgram()), not what its PATH finds.
class DeclaredProcesses final: public IProcessService
{
    /// Lets only create() construct one, through std::make_unique.
    struct Key
    {
        explicit Key() = default;
    };

  public:
    /// Makes the service, once its roots are fit for it: no two declared directories equal, and every root a
    /// command or a `path:` argument names resolved.
    /// @param config The extension's declarations.
    /// @param services What it is reached through.
    /// @return The service; `EqualRoots` or `Unresolved` otherwise.
    [[nodiscard]] static std::expected<std::unique_ptr<DeclaredProcesses>, RootError> create(
        DeclaredProcessesConfig config, DeclaredProcessesServices services);

    /// For create() only.
    DeclaredProcesses(Key key, DeclaredProcessesConfig config, DeclaredProcessesServices services);

    /// Cancels every process still running, drops every completion still to come, and waits until no
    /// onFinished call is running.
    ~DeclaredProcesses() override;

    DeclaredProcesses(DeclaredProcesses const&) = delete;
    DeclaredProcesses(DeclaredProcesses&&) = delete;
    DeclaredProcesses& operator=(DeclaredProcesses const&) = delete;
    DeclaredProcesses& operator=(DeclaredProcesses&&) = delete;

    [[nodiscard]] std::expected<ProcessHandle, ProcessError> start(std::string_view command,
                                                                   ConfigMap const& arguments,
                                                                   std::string_view cwd,
                                                                   std::string_view tag) override;
    [[nodiscard]] std::vector<ProcessInfo> list() const override;
    [[nodiscard]] std::expected<OutputPage, ProcessError> take(ProcessHandle handle,
                                                               ProcStream stream,
                                                               std::size_t offset) const override;
    [[nodiscard]] std::expected<void, ProcessError> release(ProcessHandle handle) override;

  private:
    struct State;

    /// Creates the root @p root if the manifest declares it `create` and it is missing; otherwise does
    /// nothing.
    [[nodiscard]] std::expected<void, RootError> ensureDeclaredRoot(std::string_view root) const;

    /// Creates the directories @p command's `path:` arguments need above their last component (and their
    /// root, if declared `create`), and puts the path each resolves to now into @p argv.
    [[nodiscard]] std::expected<void, RootError> makePathParents(DeclaredCommand const& command,
                                                                 std::vector<std::string>& argv) const;

    DeclaredProcessesConfig _config;
    vtpty::IProcessRunner& _runner;
    vtpty::IDirectoryOpener& _directories;
    IGrantStore const& _grants;
    std::shared_ptr<State> _state; ///< Shared with the completion handlers, which may outlive this.
};

} // namespace vtextension
