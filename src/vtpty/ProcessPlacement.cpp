// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

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
    #ifdef MSG_NOSIGNAL
    while (::send(_gate.get(), &go, 1, MSG_NOSIGNAL) < 0 && errno == EINTR)
        ;
    #else
    while (::write(_gate.get(), &go, 1) < 0 && errno == EINTR) // makeGate() set SO_NOSIGPIPE
        ;
    #endif
#endif
    _gate.reset();
    _pidfd.reset();
}

#ifndef _WIN32
std::expected<Gate, std::error_code> makeGate()
{
    auto fds = std::array<int, 2> { -1, -1 };
    #ifdef SOCK_CLOEXEC
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds.data()) != 0)
        return std::unexpected(std::error_code { errno, std::generic_category() });
    #else
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds.data()) != 0)
        return std::unexpected(std::error_code { errno, std::generic_category() });
    for (auto const fd: fds)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    #endif
    auto gate = Gate { .parentEnd = OwnedFd { fds[0] }, .childEnd = OwnedFd { fds[1] } };
    #if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
    auto const enabled = 1;
    (void) ::setsockopt(gate.parentEnd.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
    #endif
    return gate;
}
#endif

std::shared_ptr<ProcessPlacement> makeDefaultProcessPlacement()
{
    return std::make_shared<NoPlacement>();
}

} // namespace vtpty
