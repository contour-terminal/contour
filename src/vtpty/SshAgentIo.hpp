// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The I/O functions an SSH session hands libssh2 in place of its own, so that the agent's socket -- which
/// libssh2 reads blocking through those very functions (agent.c, agent_transact_unix) -- is bounded by the
/// agent's allowance and cannot spin on a closed connection. POSIX only: on Windows libssh2 reaches the agent
/// through a named pipe, outside these functions. Free of libssh2's headers so it can be tested on its own.

#ifndef _WIN32

    #include <vtpty/SshSessionRules.hpp>

    #include <sys/types.h>

    #include <chrono>
    #include <cstddef>
    #include <optional>

namespace vtpty
{

/// libssh2's receive function, LIBSSH2_RECV_FUNC, on POSIX (where libssh2_socket_t is an int).
using SshRecvFunction = ssize_t (*)(int socket, void* buffer, size_t length, int flags, void** abstract);

/// libssh2's send function, LIBSSH2_SEND_FUNC, on POSIX.
using SshSendFunction =
    ssize_t (*)(int socket, void const* buffer, size_t length, int flags, void** abstract);

/// What sshAgentBoundedRecv() and sshAgentBoundedSend() need, reached through the session's abstract pointer.
struct SshIoContext
{
    SshRecvFunction recv = nullptr; ///< libssh2's own receive function, which every call ends in.
    SshSendFunction send = nullptr; ///< libssh2's own send function, which every call ends in.
    int transport = -1;             ///< The SSH connection's socket; its I/O is passed straight through.
    /// Until when the agent may take; nullopt while no agent is asked, or no limit applies.
    std::optional<std::chrono::steady_clock::time_point> agentDeadline;
    /// What the agent's socket last did. Reset when the agent step starts.
    SshAgentIo agentIo = SshAgentIo::Fine;
};

/// Receives as libssh2's own function does. On any socket but the SSH connection's, it first waits for it
/// under the agent's deadline, failing with -ETIMEDOUT once that has passed, and reports a connection the
/// agent closed (0 bytes for a non-empty buffer) as -ECONNRESET: libssh2 only stops on a negative result
/// (agent.c, RECV_SEND_ALL), so the 0 would have it read again for ever.
/// @param socket   The socket.
/// @param buffer   Where to receive into.
/// @param length   How much to receive at most.
/// @param flags    recv() flags.
/// @param abstract The session's abstract pointer, which points at an SshIoContext.
/// @return What was received, or a negative errno.
[[nodiscard]] ssize_t sshAgentBoundedRecv(
    int socket, void* buffer, size_t length, int flags, void** abstract);

/// Sends as libssh2's own function does. On any socket but the SSH connection's, it first waits for it under
/// the agent's deadline, failing with -ETIMEDOUT once that has passed, and never raises SIGPIPE on an agent
/// that went away.
/// @param socket   The socket.
/// @param buffer   What to send.
/// @param length   How much to send.
/// @param flags    send() flags.
/// @param abstract The session's abstract pointer, which points at an SshIoContext.
/// @return What was sent, or a negative errno.
[[nodiscard]] ssize_t sshAgentBoundedSend(
    int socket, void const* buffer, size_t length, int flags, void** abstract);

} // namespace vtpty

#endif
