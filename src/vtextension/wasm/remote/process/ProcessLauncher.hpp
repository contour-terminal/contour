// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Starting extension host processes, and knowing when they end: the operating system's processes behind an
/// interface, so that the supervisor above runs on a fake in its tests.
///
/// A child starts with exactly the handles it is given (`LaunchSpec::inherited`), the environment it is given
/// and nothing of the parent's, the null device as its stdin and stdout, the parent's stderr, and the root
/// directory as its working directory. On POSIX the i-th inherited handle is descriptor 3 + i in the child;
/// on Windows a child sees inheritable duplicates under values known only once they exist, which is why the
/// command line is a function of them.

#include <core/Clock.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/transport/Channel.hpp>

namespace vtextension::wasm::remote::process
{

/// A process's id, as the system numbers it.
using ProcessId = int64_t;

/// The default cap on a helper's data memory: 1 GiB.
inline constexpr uint64_t DefaultDataBytes = uint64_t { 1 } << 30;

/// The exit code kill() ends a process with on Windows, which has no signals to tell a kill by.
inline constexpr unsigned KilledExitCode = 0xC0DE'0009;

/// Why a process could not be started, or watched.
enum class LaunchError : uint8_t
{
    NotFound,    ///< The executable does not exist, or cannot be executed.
    Resources,   ///< The system is out of memory, processes or handles.
    Unsupported, ///< The system lacks what the launcher needs (Linux: pidfds, kernel 5.3).
    Stopped,     ///< Whoever launches has stopped launching.
    Io,          ///< The system refused for any other reason.
};

/// How a process ended.
enum class ExitKind : uint8_t
{
    Exited,    ///< It exited; the value is its exit code.
    Signalled, ///< A signal ended it; the value is the signal's number (POSIX).
};

/// How a process ended, and with what.
struct ExitStatus
{
    ExitKind kind = ExitKind::Exited;
    int value = 0; ///< The exit code, or the signal's number.

    bool operator==(ExitStatus const&) const = default;
};

/// The caps on a child's resources.
struct ResourceCaps
{
    /// The cap on its data memory, in bytes; 0 for none. Windows: the job's memory limit, applied here.
    /// POSIX: handed to the helper as `--data-bytes`, which applies it itself.
    uint64_t dataBytes = DefaultDataBytes;
};

#ifdef _WIN32
/// What a process's exit is waited on by: the process HANDLE.
using Waitable = void*;
#else
/// What a process's exit is waited on by: a pidfd (Linux), a process descriptor (FreeBSD), the pid (macOS,
/// for EVFILT_PROC).
using Waitable = int;
#endif

/// What to start.
struct LaunchSpec
{
    std::filesystem::path executable;               ///< The program; an absolute path.
    std::vector<transport::NativeHandle> inherited; ///< What the child inherits; it sees them in this order.
    /// The command line after the program's name, given the values the inherited handles have in the child:
    /// on POSIX 3, 4, …; on Windows the duplicates' values.
    std::function<std::vector<std::string>(std::span<transport::NativeHandle const> childHandles)> arguments;
    std::vector<std::pair<std::string, std::string>> environment; ///< The child's whole environment.
    ResourceCaps caps {};                                         ///< Its caps.
};

/// One started process. Non-copyable, non-movable; used by one thread at a time (the supervisor's).
class IProcessHandle
{
  public:
    IProcessHandle() = default;
    IProcessHandle(IProcessHandle const&) = delete;
    IProcessHandle(IProcessHandle&&) = delete;
    IProcessHandle& operator=(IProcessHandle const&) = delete;
    IProcessHandle& operator=(IProcessHandle&&) = delete;

    /// Neither kills nor waits for the process. Where the system binds a process's life to the handle on it
    /// (Windows: the kill-on-close job; FreeBSD: the process descriptor), closing that handle ends it.
    virtual ~IProcessHandle() = default;

    /// @return The process's id.
    [[nodiscard]] virtual ProcessId id() const noexcept = 0;

    /// Ends the process at once (SIGKILL; TerminateProcess with KilledExitCode). Idempotent; does nothing
    /// once tryReap() returned a status, when the id may name another process already.
    virtual void kill() noexcept = 0;

    /// Collects the process's status if it ended. Never blocks.
    /// @return How it ended, the same status on every later call; nothing while it runs.
    [[nodiscard]] virtual std::optional<ExitStatus> tryReap() noexcept = 0;

    /// @return What its exit is waited on by, still owned by the handle.
    [[nodiscard]] virtual Waitable waitable() const noexcept = 0;
};

/// Waits for any of several processes to end. All but wake() are called by one thread (the supervisor's).
class IExitWaitSet
{
  public:
    IExitWaitSet() = default;
    IExitWaitSet(IExitWaitSet const&) = delete;
    IExitWaitSet(IExitWaitSet&&) = delete;
    IExitWaitSet& operator=(IExitWaitSet const&) = delete;
    IExitWaitSet& operator=(IExitWaitSet&&) = delete;
    virtual ~IExitWaitSet() = default;

    /// Watches @p process under @p key until unwatch(key). @p process must outlive the watch.
    /// @return Nothing; or why it cannot be watched.
    [[nodiscard]] virtual std::expected<void, LaunchError> watch(uint64_t key,
                                                                 IProcessHandle const& process) = 0;

    /// Stops watching @p key; a key not watched is ignored.
    virtual void unwatch(uint64_t key) noexcept = 0;

    /// Blocks until a watched process exits, wake() is called, or @p deadline passes (by the clock the set
    /// was made with).
    /// @return The keys whose process may have exited -- each is reported for as long as it is watched and
    ///         has ended; empty on a wake or the deadline. An error (any LaunchError) if the system cannot
    ///         wait at all -- for instance a watched process's handle closed while watched, which breaks
    ///         watch()'s contract: a caller that waits again at once would spin, so it must stop waiting on
    ///         this set.
    [[nodiscard]] virtual std::expected<std::vector<uint64_t>, LaunchError> wait(
        std::optional<core::SteadyTimePoint> deadline) = 0;

    /// Ends a wait() blocked now, or else the next one. Wait-free, any thread.
    virtual void wake() noexcept = 0;
};

/// Starts processes, and makes the sets their exits are waited on with: a handle and the set that waits on it
/// are one platform's.
class IProcessLauncher
{
  public:
    IProcessLauncher() = default;
    IProcessLauncher(IProcessLauncher const&) = delete;
    IProcessLauncher(IProcessLauncher&&) = delete;
    IProcessLauncher& operator=(IProcessLauncher const&) = delete;
    IProcessLauncher& operator=(IProcessLauncher&&) = delete;
    virtual ~IProcessLauncher() = default;

    /// Starts @p spec. The handles in it stay the caller's.
    /// @return The process; or why it did not start, in which case no process of it runs.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IProcessHandle>, LaunchError> launch(
        LaunchSpec const& spec) = 0;

    /// @param clock What the set's deadlines are measured by; must outlive the set.
    /// @return A set to wait for this launcher's processes with.
    [[nodiscard]] virtual std::expected<std::unique_ptr<IExitWaitSet>, LaunchError> makeWaitSet(
        core::IClock& clock) = 0;
};

/// @return This process's id: what a helper is told is its parent (`--parent`).
[[nodiscard]] ProcessId currentProcessId() noexcept;

/// @return The launcher of this operating system's processes.
[[nodiscard]] std::shared_ptr<IProcessLauncher> makeSystemProcessLauncher();

/// @return What a helper's environment holds by default: nothing on POSIX; `SystemRoot` on Windows, without
///         which a process cannot load the system's DLLs.
[[nodiscard]] std::vector<std::pair<std::string, std::string>> defaultHelperEnvironment();

} // namespace vtextension::wasm::remote::process
