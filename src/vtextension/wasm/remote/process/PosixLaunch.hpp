// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the POSIX launchers share: a launch prepared before anything is spawned -- the executable checked,
/// the command line and environment as C strings, the inherited descriptors parked above the numbers the
/// child gives them -- the posix_spawn() of the systems that spawn (Linux, macOS), the reaping by pid of the
/// systems that reap so (macOS, FreeBSD), the mapping of errno and wait statuses, and the poll-based exit
/// wait set of the systems whose processes are descriptors (Linux pidfds, FreeBSD process descriptors).

#include <sys/types.h>

#include <cstddef>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <spawn.h>

#include <vtextension/wasm/remote/process/ProcessLauncher.hpp>

namespace vtextension::wasm::remote::process::posix
{

/// The descriptor the first inherited handle is in the child; the i-th is this plus i.
inline constexpr int FirstChildDescriptor = 3;

/// An open descriptor, closed when destroyed. Move-only.
class UniqueFd
{
  public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int descriptor) noexcept: _descriptor { descriptor } {}
    UniqueFd(UniqueFd&& other) noexcept: _descriptor { std::exchange(other._descriptor, -1) } {}
    UniqueFd& operator=(UniqueFd&& other) noexcept;
    UniqueFd(UniqueFd const&) = delete;
    UniqueFd& operator=(UniqueFd const&) = delete;
    ~UniqueFd();

    /// @return The descriptor; -1 if none.
    [[nodiscard]] int get() const noexcept { return _descriptor; }

  private:
    int _descriptor = -1;
};

/// Owns a posix_spawn() object of type T, initialized by Init and destroyed by Destroy, and the first error
/// of its initialization and of every change made to it since.
template <typename T, int (*Init)(T*), int (*Destroy)(T*)>
class SpawnObject
{
  public:
    SpawnObject() noexcept: _initError { Init(&_object) }, _error { _initError } {}
    SpawnObject(SpawnObject const&) = delete;
    SpawnObject(SpawnObject&&) = delete;
    SpawnObject& operator=(SpawnObject const&) = delete;
    SpawnObject& operator=(SpawnObject&&) = delete;
    ~SpawnObject()
    {
        if (_initError == 0)
            Destroy(&_object);
    }

    /// @return The first error: of the initialization, or of a change since; 0 for none.
    [[nodiscard]] int error() const noexcept { return _error; }

    /// Records @p result, a change's return value, unless an error is recorded already.
    void check(int result) noexcept
    {
        if (_error == 0)
            _error = result;
    }

    /// @return The object, for the calls that change or use it.
    [[nodiscard]] T* get() noexcept { return &_object; }

  private:
    T _object {};
    int _initError;
    int _error;
};

/// Owns initialized file actions.
using FileActions = SpawnObject<posix_spawn_file_actions_t,
                                &::posix_spawn_file_actions_init,
                                &::posix_spawn_file_actions_destroy>;

/// Owns initialized spawn attributes.
using SpawnAttributes = SpawnObject<posix_spawnattr_t, &::posix_spawnattr_init, &::posix_spawnattr_destroy>;

/// A launch prepared for the system call that spawns it.
struct PreparedLaunch
{
    std::string executable;               ///< The program's path.
    std::vector<std::string> arguments;   ///< argv, the program's path first.
    std::vector<std::string> environment; ///< envp, each `NAME=value`.
    std::vector<UniqueFd> parked; ///< The inherited descriptors' close-on-exec copies, all at or above
                                  ///< childEnd(): dup2() onto 3, 4, … cannot overwrite one of them.
    UniqueFd nullDevice;          ///< /dev/null, read-write and close-on-exec: stdin and stdout.
    /// The stderr the spec names, parked as the inherited ones are; -1 for the null device.
    UniqueFd standardError;

    /// @return What becomes the child's stderr: the one the spec names, else the null device.
    [[nodiscard]] int childStandardError() const noexcept
    {
        return standardError.get() != -1 ? standardError.get() : nullDevice.get();
    }

    /// @return The first descriptor beyond those the child inherits: what it closes from.
    [[nodiscard]] int childEnd() const noexcept
    {
        return FirstChildDescriptor + static_cast<int>(parked.size());
    }

    /// @return argv for exec: pointers into arguments, null-terminated. Valid while this is unchanged.
    [[nodiscard]] std::vector<char*> argv();

    /// @return envp for exec: pointers into environment, null-terminated. Valid while this is unchanged.
    [[nodiscard]] std::vector<char*> envp();
};

/// Checks @p spec's executable, parks its inherited descriptors and builds its command line and environment.
/// @return The prepared launch; `NotFound` if the executable cannot be executed, or why a descriptor could
/// not
///         be duplicated.
[[nodiscard]] std::expected<PreparedLaunch, LaunchError> prepareLaunch(LaunchSpec const& spec);

/// @return Why a call that set @p error failed to start or watch a process.
[[nodiscard]] LaunchError launchErrorOf(int error) noexcept;

/// @return How a process ended, from the status waitpid() reported.
[[nodiscard]] ExitStatus exitStatusOfWait(int status) noexcept;

#if defined(__linux__) || defined(__APPLE__)
/// Spawns @p prepared with posix_spawn(): the null device on stdin and stdout, its stderr
/// (childStandardError()), the parked descriptors dup2()ed onto 3, 4, … and no other descriptor (Linux:
/// closed from childEnd() on; macOS: POSIX_SPAWN_CLOEXEC_DEFAULT), "/" as its working directory, default
/// signal dispositions, an empty signal mask, and a session of its own (POSIX_SPAWN_SETSID), so that it has
/// no controlling terminal and is in no process group a terminal signals.
/// @return The child's pid; or why it could not be spawned, in which case no child runs.
[[nodiscard]] std::expected<pid_t, LaunchError> spawn(PreparedLaunch& prepared);
#endif

#if defined(__APPLE__) || defined(__FreeBSD__)
/// Collects the status of child @p pid if it ended, by waitpid(). Never blocks.
/// @return How it ended -- an exit code of -1 if another reaped it (ECHILD), its status lost with it;
///         nothing while it runs; why waitpid() failed otherwise.
[[nodiscard]] std::expected<std::optional<ExitStatus>, LaunchError> reapChild(pid_t pid) noexcept;
#endif

#ifndef __APPLE__
/// A wait set that polls process descriptors, which become readable (Linux) or hang up (FreeBSD) when their
/// process exits, beside a pipe its wake() writes to. Not on macOS, whose processes are no descriptors.
/// @param clock What deadlines are measured by.
/// @return The set; or why its wake descriptor could not be made.
[[nodiscard]] std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makePollExitWaitSet(
    core::IClock& clock);
#endif

} // namespace vtextension::wasm::remote::process::posix
