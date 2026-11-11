// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/platform/Clock.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <vthost/keeper/Ports.hpp>
#include <vthost/keeper/UniqueFd.hpp>

/// The keeper's ports on POSIX: fork and exec, an AF_UNIX channel, signalfd and pidfds (Linux) or kqueue
/// (macOS), kill(), and the descriptor limit. Only Linux is built and tested on this branch; the macOS paths
/// are kept compiling where that is cheap, and its process-exit backstop comes with the macOS task.
namespace vthost::keeper
{

/// The descriptor a daemon finds its channel on (`--keeper-channel=3`).
inline constexpr int DaemonChannelDescriptor = 3;

/// How the event source learns that a watched process ended.
enum class ProcessWatch : uint8_t
{
    ProcessHandle = 0, ///< A pidfd per process where the kernel has pidfd_open(); SIGCHLD and
                       ///< waitid(P_PID) for a process it has none for.
    SignalOnly,        ///< SIGCHLD and waitid(P_PID) for every process: the fallback, forced (tests).
};

/// What the POSIX ports are configured with; fixed for their lifetime.
struct KeeperPortsConfig
{
    /// The daemon's argv after argv[0], which is its image: frozen, as the keeper's call into the daemon may
    /// never change (`daemon keeper-serve --keeper-channel=3`).
    std::vector<std::string> daemonArguments;

    /// The environment, `NAME=value` each, that the daemon and every spawned child start with.
    std::vector<std::string> environment;

    /// How processes are watched.
    ProcessWatch processWatch = ProcessWatch::ProcessHandle;
};

/// The POSIX ports, owned; ports() lends them to a Keeper. Movable: the ports live on the heap, so the
/// references ports() hands out stay valid across a move.
///
/// Constructing them blocks SIGTERM, SIGINT, SIGHUP and SIGCHLD in this (single-threaded) process, to
/// receive them through the event source, and raises the descriptor limit; destroying them undoes both.
struct PosixKeeperPorts
{
    std::unique_ptr<core::platform::IClock> clock;
    std::unique_ptr<DescriptorBudget> budget; ///< Raises the soft RLIMIT_NOFILE to the hard limit.
    std::unique_ptr<EventSource> events;
    std::unique_ptr<DaemonLauncher> launcher;
    std::unique_ptr<ShellSpawner> spawner;
    std::unique_ptr<Signaller> signaller;

    /// @return The ports, by reference, for a Keeper; valid while this object lives.
    [[nodiscard]] KeeperPorts ports() const;
};

/// Builds the POSIX ports.
/// @param config What they are configured with.
/// @return The ports; or why the signal watch could not be set up.
[[nodiscard]] std::expected<PosixKeeperPorts, std::error_code> makePosixKeeperPorts(KeeperPortsConfig config);

/// Wraps the keeper's end of a channel socket, as the launcher does with the socket pair it creates.
///
/// The socket is made non-blocking. Channel::send() sends at once what the socket takes and queues the rest
/// with duplicates of the frame's descriptors, so they stay the caller's to close as soon as send()
/// returns; a daemon that leaves too much unread is dropped (EventKind::ChannelClosed). A send that finds
/// the peer gone (EPIPE, ECONNRESET) stops only the sending: what the peer wrote before is still read, and
/// ChannelClosed comes at the end of it. Every frame that
/// carries descriptors goes out in a `sendmsg` of its own, its descriptors on its first byte, with
/// MSG_NOSIGNAL (SO_NOSIGPIPE on macOS): a dead peer never raises SIGPIPE. Received descriptors are
/// close-on-exec; a truncated descriptor list (MSG_CTRUNC) ends the link.
/// @param socket A connected AF_UNIX SOCK_STREAM socket.
/// @return The channel; the event source in the same ports watches it.
[[nodiscard]] std::unique_ptr<Channel> makePosixChannel(UniqueFd socket);

/// Why a label could not be locked.
enum class LockError : uint8_t
{
    Unopenable = 0, ///< The lock file could not be opened or created.
    Held,           ///< Another keeper holds the label.
};

/// @param socketPath The label's socket path.
/// @return Its lock file, `<socket path>.keeper.lock`.
[[nodiscard]] std::filesystem::path labelLockPath(std::filesystem::path const& socketPath);

/// Takes the label's exclusive lock (`flock(LOCK_EX | LOCK_NB)` on labelLockPath()), without waiting.
/// @param socketPath The label's socket path.
/// @return The lock, held until it is destroyed; or why it could not be taken.
[[nodiscard]] std::expected<std::unique_ptr<LabelLock>, LockError> lockLabel(
    std::filesystem::path const& socketPath);

} // namespace vthost::keeper
