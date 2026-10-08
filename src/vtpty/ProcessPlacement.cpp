// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#ifdef VTPTY_SYSTEMD
    #include <vtpty/SdBusScopeBus.hpp>
#endif

#include <array>
#include <cerrno>
#include <utility>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

namespace vtpty
{

#ifndef _WIN32
namespace
{
    #ifdef MSG_NOSIGNAL
    constexpr auto NoSigPipeSendFlags = MSG_NOSIGNAL;
    #else
    constexpr auto NoSigPipeSendFlags = 0; // makeGate() set SO_NOSIGPIPE on the socket instead
    #endif

    #ifdef SOCK_CLOEXEC
    constexpr auto CloseOnExecStreamSocket = SOCK_STREAM | SOCK_CLOEXEC;
    #else
    constexpr auto CloseOnExecStreamSocket = SOCK_STREAM; // made close-on-exec by makeGate()
    #endif
} // namespace
#endif

ParkedChild::ParkedChild(int pid, OwnedFd pidfd, OwnedFd gate) noexcept:
    _pid { pid }, _pidfd { std::move(pidfd) }, _gate { std::move(gate) }
{
}

ParkedChild::~ParkedChild()
{
    release();
}

void ParkedChild::release() noexcept
{
    if (!_gate.isOpen())
        return;
#ifndef _WIN32
    // A byte rather than end-of-file: a child forked for another session while this one is parked
    // inherits this end too (close-on-exec acts only at exec), and end-of-file would wait for it.
    char const go = 'g';
    while (::send(_gate.get(), &go, 1, NoSigPipeSendFlags) < 0 && errno == EINTR)
        ;
#endif
    _gate.reset();
    _pidfd.reset();
}

#ifndef _WIN32
std::expected<Gate, std::error_code> makeGate()
{
    auto fds = std::array<int, 2> { -1, -1 };
    if (::socketpair(AF_UNIX, CloseOnExecStreamSocket, 0, fds.data()) != 0)
        return std::unexpected(std::error_code { errno, std::generic_category() });
    auto gate = Gate { .parentEnd = OwnedFd { fds[0] }, .childEnd = OwnedFd { fds[1] } };
    #ifndef SOCK_CLOEXEC
    for (auto const fd: fds)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    #endif
    #ifndef MSG_NOSIGNAL
    // Where send() takes no MSG_NOSIGNAL (macOS), the socket itself is told not to raise SIGPIPE.
    auto const enabled = 1;
    (void) ::setsockopt(gate.parentEnd.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    #endif
    return gate;
}
#endif

std::shared_ptr<ProcessPlacement> makeDefaultProcessPlacement()
{
#ifdef VTPTY_SYSTEMD
    return makeSystemdScopePlacement(SystemdScopeConfig {});
#else
    return std::make_shared<NoPlacement>();
#endif
}

} // namespace vtpty
