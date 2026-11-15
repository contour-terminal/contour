// SPDX-License-Identifier: Apache-2.0
#include <orchestrator/proto/Signals.hpp>

#include <array>
#include <cerrno>
#include <csignal>
#include <expected>
#include <span>

#include <fcntl.h>
#include <unistd.h>

namespace orchestrator::proto
{

namespace
{
    /// The byte a stop signal writes; SIGCHLD writes another.
    constexpr auto StopByte = 's';
    constexpr auto ChildByte = 'c';

    /// Where the handlers write: set once by install(), before any handler is.
    int signalWriteFd = -1;

    extern "C" void onSignal(int signal)
    {
        auto const savedErrno = errno;
        auto const byte = signal == SIGCHLD ? ChildByte : StopByte;
        std::ignore = ::write(signalWriteFd, &byte, 1);
        errno = savedErrno;
    }
} // namespace

std::expected<SignalPipe, int> SignalPipe::install()
{
    auto fds = std::array<int, 2> {};
    if (::pipe2(fds.data(), O_CLOEXEC | O_NONBLOCK) != 0)
        return std::unexpected { errno };
    auto pipe = SignalPipe { Descriptor { fds[0] }, Descriptor { fds[1] } };
    signalWriteFd = pipe._write.get();
    struct sigaction action = {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_NOCLDSTOP;
    for (auto const signal: { SIGINT, SIGTERM, SIGCHLD })
        if (::sigaction(signal, &action, nullptr) != 0)
            return std::unexpected { errno };
    return pipe;
}

StopRequest SignalPipe::drain() const noexcept
{
    auto request = StopRequest::None;
    auto buffer = std::array<char, 64> {};
    while (true)
    {
        auto const got = ::read(_read.get(), buffer.data(), buffer.size());
        if (got <= 0)
            return request;
        for (auto const byte: std::span(buffer).first(static_cast<std::size_t>(got)))
            if (byte == StopByte)
                request = StopRequest::Received;
    }
}

} // namespace orchestrator::proto
