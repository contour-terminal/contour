// SPDX-License-Identifier: Apache-2.0
#include <vtpty/ProcessPlacement.hpp>

#ifdef VTPTY_SYSTEMD
    #include <vtpty/SdBusScopeBus.hpp>
#endif

#include <algorithm>
#include <array>
#include <cerrno>
#include <fstream>
#include <utility>

#ifndef _WIN32
    #include <sys/socket.h>

    #include <fcntl.h>
    #include <unistd.h>
#endif

#ifdef __linux__
    #include <sys/syscall.h>
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

bool ParkedChild::hasExited() const noexcept
{
#if defined(__linux__) && defined(SYS_pidfd_send_signal)
    // Signal 0 checks without sending; a pidfd answers ESRCH once its process has exited, reaped or
    // not, and never for a process that merely reuses the pid.
    return _pidfd.isOpen() && ::syscall(SYS_pidfd_send_signal, _pidfd.get(), 0, nullptr, 0) < 0
           && errno == ESRCH;
#else
    return false;
#endif
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

namespace
{
    /// How much likelier than Contour a session's process is to be the kernel's out-of-memory
    /// victim, and the ceiling the kernel accepts.
    constexpr auto SessionOomScoreAdjustIncrement = 100;
    constexpr auto MaxOomScoreAdjust = 1000;

    /// @return The oom_score_adj for session processes: this process's own plus the increment, or
    ///         nothing where the kernel has no such knob. Read as a stream: /proc reports its files
    ///         as empty, so a read sized by the file's size gets nothing.
    [[nodiscard]] std::optional<int> sessionOomScoreAdjust()
    {
#ifdef __linux__
        auto own = 0;
        if (auto in = std::ifstream { "/proc/self/oom_score_adj" }; in >> own)
            return std::min(own + SessionOomScoreAdjustIncrement, MaxOomScoreAdjust);
#endif
        return std::nullopt;
    }
} // namespace

std::shared_ptr<ProcessPlacement> makeDefaultProcessPlacement()
{
    // The composition root's one look at the process: everything below it is handed the value.
    auto const childOomScoreAdjust = sessionOomScoreAdjust();
#ifdef VTPTY_SYSTEMD
    auto config = SystemdScopeConfig {};
    config.childOomScoreAdjust = childOomScoreAdjust;
    return makeSystemdScopePlacement(std::move(config));
#else
    return std::make_shared<NoPlacement>(childOomScoreAdjust);
#endif
}

} // namespace vtpty
