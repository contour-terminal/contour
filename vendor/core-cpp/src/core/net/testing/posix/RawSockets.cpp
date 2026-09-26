// SPDX-License-Identifier: Apache-2.0
#include <core/net/testing/RawSockets.hpp>

#include <core/net/posix/PosixListener.hpp>
#include <core/net/posix/PosixSocket.hpp>

#include <sys/socket.h>

#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>

namespace core::net::testing
{

void closeRawSocket(platform::NativeHandle handle) noexcept
{
    if (handle != platform::InvalidHandle)
        ::close(handle);
}

platform::NativeHandle openRawTcpSocket() noexcept
{
    return ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
}

RawConnection rawLoopbackConnection() noexcept
{
    auto const listener = RawSocket { openRawTcpSocket() };
    if (listener.get() == platform::InvalidHandle)
        return {};
    auto address = sockaddr_in {};
    address.sin_family = AF_INET;
    address.sin_port = 0;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    auto* const generic = reinterpret_cast<sockaddr*>(&address);
    auto length = static_cast<socklen_t>(sizeof(address));
    if (::bind(listener.get(), generic, length) != 0 || ::listen(listener.get(), 1) != 0
        || ::getsockname(listener.get(), generic, &length) != 0)
        return {};

    auto client = RawSocket { openRawTcpSocket() };
    if (client.get() == platform::InvalidHandle
        || ::connect(client.get(), generic, static_cast<socklen_t>(sizeof(address))) != 0)
        return {};
    auto accepted = RawSocket { ::accept(listener.get(), nullptr, nullptr) };
    if (accepted.get() == platform::InvalidHandle)
        return {};
    return RawConnection { .client = client.release(), .accepted = accepted.release() };
}

bool rawSendAll(platform::NativeHandle handle, std::string_view payload) noexcept
{
    auto const sent = ::send(handle, payload.data(), payload.size(), 0);
    return sent >= 0 && static_cast<std::size_t>(sent) == payload.size();
}

std::ptrdiff_t rawReceive(platform::NativeHandle handle, std::span<char> into) noexcept
{
    return ::recv(handle, into.data(), into.size(), 0);
}

platform::NativeHandle nativeHandleOf(ISocket const& socket) noexcept
{
    if (auto const* posix = dynamic_cast<PosixSocket const*>(&socket))
        return posix->native();
    return platform::InvalidHandle;
}

platform::NativeHandle nativeHandleOf(IListener const& listener) noexcept
{
    if (auto const* posix = dynamic_cast<PosixListener const*>(&listener))
        return posix->native();
    return platform::InvalidHandle;
}

} // namespace core::net::testing
