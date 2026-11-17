// SPDX-License-Identifier: Apache-2.0
#include <vtpty/SshAgentIo.hpp>

#ifndef _WIN32

    #include <vtpty/SshSocketWait.hpp>

    #include <sys/socket.h>

    #include <cerrno>

    #include <poll.h>

namespace vtpty
{

namespace
{
    /// @return The context the session was created with.
    [[nodiscard]] SshIoContext& ioContextOf(void** abstract) noexcept
    {
        return *static_cast<SshIoContext*>(*abstract);
    }

    /// Waits for an agent socket under the agent's deadline, recording a timeout.
    /// @return 0 once it may be used; -ETIMEDOUT or another negative errno otherwise.
    [[nodiscard]] ssize_t waitForAgent(SshIoContext& io, int socket, short events)
    {
        if (!io.agentDeadline)
            return 0;
        auto const ready = waitForDescriptor(socket, events, *io.agentDeadline);
        if (ready)
            return 0;
        if (ready.error() == ETIMEDOUT)
            io.agentIo = SshAgentIo::TimedOut;
        return -static_cast<ssize_t>(ready.error());
    }
} // namespace

ssize_t sshAgentBoundedRecv(int socket, void* buffer, size_t length, int flags, void** abstract)
{
    auto& io = ioContextOf(abstract);
    if (socket == io.transport)
        return io.recv(socket, buffer, length, flags, abstract);
    if (auto const waited = waitForAgent(io, socket, POLLIN); waited < 0)
        return waited;
    auto const received = io.recv(socket, buffer, length, flags, abstract);
    // An agent gone away reads as the end of the stream, or -- on Linux, when it left our request unread --
    // as a reset.
    if ((received == 0 && length > 0) || received == -ECONNRESET)
    {
        io.agentIo = SshAgentIo::Closed;
        return -ECONNRESET;
    }
    return received;
}

ssize_t sshAgentBoundedSend(int socket, void const* buffer, size_t length, int flags, void** abstract)
{
    auto& io = ioContextOf(abstract);
    if (socket == io.transport)
        return io.send(socket, buffer, length, flags, abstract);
    if (auto const waited = waitForAgent(io, socket, POLLOUT); waited < 0)
        return waited;
    #ifdef MSG_NOSIGNAL
    auto const sent = io.send(socket, buffer, length, flags | MSG_NOSIGNAL, abstract);
    #else
    // No MSG_NOSIGNAL here: the socket's own SO_NOSIGPIPE does the same, set before every send because the
    // socket is libssh2's and may be a new one each time the agent is asked.
    auto const noSigPipe = 1;
    ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
    auto const sent = io.send(socket, buffer, length, flags, abstract);
    #endif
    if (sent == -EPIPE || sent == -ECONNRESET)
        io.agentIo = SshAgentIo::Closed;
    return sent;
}

} // namespace vtpty

#endif
