// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <sys/types.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <poll.h>

#include <orchestrator/core/Engine.hpp>

namespace orchestrator::proto
{

/// The most bytes kept of one output stream of a command; the rest is read and dropped.
inline constexpr std::size_t MaxStreamBytes = std::size_t { 1024 } * 1024;

/// A program to start: run directly, never through a shell.
struct SpawnRequest
{
    std::vector<std::string> argv;        ///< Its words; the first is looked up on the caller's PATH.
    std::vector<std::string> environment; ///< Its whole environment, as `NAME=VALUE`.
    std::string cwd;                      ///< Its working directory, which must exist.
    std::chrono::milliseconds timeout {}; ///< How long it may run before it and its process group are killed.
};

/// Owns a file descriptor.
class Descriptor
{
  public:
    Descriptor() = default;
    /// @param fd The descriptor to own; -1 for none.
    explicit Descriptor(int fd) noexcept: _fd { fd } {}
    Descriptor(Descriptor const&) = delete;
    Descriptor& operator=(Descriptor const&) = delete;
    Descriptor(Descriptor&& other) noexcept;
    Descriptor& operator=(Descriptor&& other) noexcept;
    ~Descriptor() { reset(); }

    /// @return The descriptor; -1 when none is owned.
    [[nodiscard]] int get() const noexcept { return _fd; }
    /// Closes the descriptor, if one is owned.
    void reset() noexcept;

  private:
    int _fd = -1;
};

/// Whether a child still needs the poll loop.
enum class ChildState : uint8_t
{
    Collecting, ///< Its output is still open, or it has not exited yet.
    Ended,      ///< It exited, or was killed, and is reaped: take its outcome().
};

/// How long a child that exited may keep its output open, through a process it left behind, before the
/// rest of its process group is killed and the child counts as having exited when it did.
inline constexpr auto ExitGraceMs = uint64_t { 1000 };

/// A started process whose two output streams are collected, each capped at MaxStreamBytes, and which is
/// killed with its process group at its deadline. Everything is driven by the caller's poll(2) loop: no
/// thread. Whether it timed out is decided by when it exited, not by when its output was collected.
class Child
{
  public:
    /// Starts a process. posix_spawnp() has no working-directory parameter, so the child changes into it
    /// with posix_spawn_file_actions_addchdir_np() (glibc 2.29, macOS 10.15); it gets its own process group,
    /// /dev/null as standard input, the default signal dispositions, and no descriptor above standard error
    /// (posix_spawn_file_actions_addclosefrom_np(), glibc 2.34; POSIX_SPAWN_CLOEXEC_DEFAULT on macOS).
    /// A program path is taken as given: a relative one containing `/` is resolved after the chdir.
    /// @param request What to start.
    /// @param nowMs Now, in monotonic milliseconds: the deadline is counted from it.
    /// @return The child; StartResult::Refused when it could not be started.
    [[nodiscard]] static std::expected<Child, StartResult> spawn(SpawnRequest const& request, uint64_t nowMs);

    /// Appends the descriptors still open to @p fds, each asking for input.
    void addPollFds(std::vector<pollfd>& fds) const;
    /// Reads what @p fds reported for this child's descriptors, notes when it exited, and ends it once it
    /// exited and both are closed.
    /// @param fds What poll(2) returned.
    /// @param nowMs Now, in monotonic milliseconds.
    void service(std::span<pollfd const> fds, uint64_t nowMs);
    /// Ends a child whose time is up: one that exited but whose output stays open past ExitGraceMs counts
    /// as Exited; one still running at its deadline is killed with its process group and counts as TimedOut.
    /// @param nowMs Now, in monotonic milliseconds.
    void enforceDeadline(uint64_t nowMs);
    /// Kills the child's process group and reaps it, whatever it was doing: the driver is stopping.
    void terminate() noexcept;
    /// @return Whether the child still needs the loop.
    [[nodiscard]] ChildState state() const noexcept { return _state; }
    /// @return When enforceDeadline() next has something to do, in monotonic milliseconds.
    [[nodiscard]] uint64_t nextCheckMs() const noexcept;
    /// @return What became of the child; meaningful once it Ended.
    [[nodiscard]] ProcessOutcome const& outcome() const noexcept { return _outcome; }

  private:
    /// What one read of a stream brought.
    enum class ReadResult : uint8_t
    {
        Nothing, ///< Nothing yet: interrupted, or it would block.
        Data,    ///< Bytes, kept up to MaxStreamBytes.
        Closed,  ///< The end of the stream, or an error: it is closed now.
    };

    Child(pid_t pid, Descriptor out, Descriptor err, uint64_t deadlineMs) noexcept;
    [[nodiscard]] ReadResult readOnce(std::size_t index);
    void drainAvailable();
    void noteExit(uint64_t nowMs);
    void closeStreams() noexcept;

    pid_t _pid;
    std::array<Descriptor, 2> _streams; ///< Standard output, then standard error.
    uint64_t _deadlineMs;
    std::optional<uint64_t> _exitedAtMs; ///< When it was seen to have exited and was reaped.
    ChildState _state = ChildState::Collecting;
    ProcessOutcome _outcome;
};

/// Starts a process and waits for it, with its timeout: for the driver's own short calls.
/// @param request What to start.
/// @param nowMs Now, in monotonic milliseconds.
/// @return What became of it, StartResult::Refused included.
[[nodiscard]] ProcessOutcome runToCompletion(SpawnRequest const& request, uint64_t nowMs);

/// @param start Why a command did not run: StartResult::CwdMissing or StartResult::Refused.
/// @return The outcome of a command that never started.
[[nodiscard]] ProcessOutcome notStartedOutcome(StartResult start);

/// @return Now, in monotonic milliseconds; always greater than 0.
[[nodiscard]] uint64_t monotonicNowMs() noexcept;

} // namespace orchestrator::proto
