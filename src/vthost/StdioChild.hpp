// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Spawning a program with its standard input and output on a reactor-driven socket: what runs
/// `ssh <target> contour daemon --stdio` for a client and hands it the far end as a transport.

#include <vtpty/ProgramLookup.hpp>

#include <core/net/EventLoop.hpp>
#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>

namespace vthost
{

/// How much of a child's stderr is kept: its last bytes, which is where a failing ssh says why.
inline constexpr std::size_t StderrTailLimit = 4096;

/// What IStdioChild::exitCode() reports for a child that ended while its status was lost: something else
/// (a SIGCHLD handler) reaped it. No exit status and no signal yields it, so nothing that classifies an
/// exit code -- SshTransport's ShellCommandNotFound in particular -- can mistake it for one.
inline constexpr int ExitStatusLost = -1;

/// A running child whose stdin/stdout are the transport handed out beside it.
class IStdioChild
{
  public:
    /// terminate()s the child if it still runs.
    virtual ~IStdioChild() = default;

    /// @return The last StderrTailLimit bytes the child wrote to stderr so far.
    [[nodiscard]] virtual std::string stderrTail() const = 0;

    /// Waits up to @p wait for the child to end. Once it has, waits within the same budget for its
    /// stderr to be drained too, so stderrTail() then holds everything it wrote.
    /// @return Its exit code (128 + the signal for one a signal ended), ExitStatusLost for one another
    ///         waiter reaped, or nullopt while it runs.
    [[nodiscard]] virtual std::optional<int> exitCode(std::chrono::milliseconds wait) = 0;

    /// Ends and reaps the child: SIGTERM, 1 s, SIGKILL on POSIX; TerminateProcess on Windows. Idempotent.
    virtual void terminate() noexcept = 0;
};

/// A spawned child and the socket its stdin/stdout are connected to.
struct SpawnedStdioChild
{
    std::unique_ptr<core::net::ISocket> transport; ///< Writes reach the child's stdin; its stdout is read.
    std::unique_ptr<IStdioChild> process;          ///< The child itself.
};

/// Why a spawn failed.
enum class SpawnFailure : std::uint8_t
{
    ProgramNotFound, ///< argv[0] names no program on the search path.
    SystemError,     ///< The operating system refused a step of the spawn.
};

/// A spawn failure, and its words for the user.
struct StdioSpawnError
{
    SpawnFailure kind;   ///< What went wrong.
    std::string message; ///< Says what, naming the program or the call that failed.
};

/// Starts programs with their stdio on a reactor socket.
class IStdioSpawner
{
  public:
    virtual ~IStdioSpawner() = default;

    /// Starts @p argv. Must run on @p loop's thread (adoptFd/adoptSocket require it).
    /// @param loop Drives the returned transport.
    /// @param argv The program and its arguments; argv[0] is looked up as a program.
    /// @return The transport and the child, or why it could not be started.
    [[nodiscard]] virtual std::expected<SpawnedStdioChild, StdioSpawnError> spawn(
        core::net::EventLoop& loop, std::span<std::string const> argv) = 0;
};

/// The operating system's spawner.
///
/// The child is not placed (vtpty::ProcessPlacement): it lives exactly as long as the client's
/// connection it carries, so the client's own resource domain is its right one -- unlike the daemon,
/// which must outlive any client.
/// @param search Where argv[0] is looked up. Holds views: what it names must outlive the spawner.
/// @return The spawner.
[[nodiscard]] std::unique_ptr<IStdioSpawner> makeSystemStdioSpawner(vtpty::ProgramSearch search);

} // namespace vthost
