// SPDX-License-Identifier: Apache-2.0

// winsock2.h MUST precede windows.h / ws2tcpip.h, which project headers pull in.
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
// clang-format on

#include <core/net/detail/BlockingPrimitives.hpp>

#include <core/net/detail/SocketErrors.hpp>

#include <algorithm>
#include <limits>
#include <string>
#include <tuple>
#include <utility>

namespace core::net::detail
{

namespace
{
    [[nodiscard]] SOCKET asSocket(platform::NativeHandle handle) noexcept
    {
        return reinterpret_cast<SOCKET>(handle);
    }

    /// Winsock lengths are `int`; a larger span is sent or received in pieces, which a blocking
    /// caller loops over anyway.
    [[nodiscard]] int clampedLength(std::size_t size) noexcept
    {
        return static_cast<int>(
            std::min<std::size_t>(size, static_cast<std::size_t>(std::numeric_limits<int>::max())));
    }
} // namespace

std::expected<std::size_t, NetError> receiveSome(platform::NativeHandle socket, std::span<std::byte> buffer)
{
    auto const got =
        ::recv(asSocket(socket), reinterpret_cast<char*>(buffer.data()), clampedLength(buffer.size()), 0);
    if (got == SOCKET_ERROR)
        return std::unexpected(socketError(::WSAGetLastError(), "recv"));
    return static_cast<std::size_t>(got);
}

std::expected<std::size_t, NetError> waitReadable(platform::NativeHandle socket)
{
    auto probe = char {};
    // A blocking peek of one byte is the wait, for the reason the POSIX half gives.
    auto const got = ::recv(asSocket(socket), &probe, 1, MSG_PEEK);
    if (got == SOCKET_ERROR)
        return std::unexpected(socketError(::WSAGetLastError(), "recv(MSG_PEEK)"));
    if (got == 0)
        return std::size_t { 0 };
    auto pending = u_long { 0 };
    if (::ioctlsocket(asSocket(socket), static_cast<long>(FIONREAD), &pending) == 0 && pending > 0)
        return static_cast<std::size_t>(pending);
    return std::size_t { 1 };
}

std::expected<std::size_t, NetError> sendSome(platform::NativeHandle socket,
                                              std::span<std::byte const> buffer)
{
    // No SIGPIPE on Windows: a write to a broken connection is reported through WSAGetLastError.
    auto const sent = ::send(
        asSocket(socket), reinterpret_cast<char const*>(buffer.data()), clampedLength(buffer.size()), 0);
    if (sent == SOCKET_ERROR)
        return std::unexpected(socketError(::WSAGetLastError(), "send"));
    return static_cast<std::size_t>(sent);
}

std::expected<void, NetError> shutdownSend(platform::NativeHandle socket)
{
    if (::shutdown(asSocket(socket), SD_SEND) == 0)
        return {};
    auto const err = ::WSAGetLastError();
    if (err == WSAENOTCONN)
        return {};
    return std::unexpected(socketError(err, "shutdown"));
}

void closeSocket(platform::NativeHandle socket) noexcept
{
    if (socket != platform::InvalidHandle)
        std::ignore = ::closesocket(asSocket(socket));
}

void setIoTimeout(platform::NativeHandle socket,
                  IoDirection direction,
                  std::chrono::milliseconds timeout) noexcept
{
    // A DWORD of milliseconds, where zero is "no timeout" -- so a non-positive request is written as
    // zero rather than skipped, for the reason the POSIX half gives.
    auto const millis = static_cast<DWORD>(std::max<std::chrono::milliseconds::rep>(timeout.count(), 0));
    auto const option = direction == IoDirection::Receive ? SO_RCVTIMEO : SO_SNDTIMEO;
    std::ignore = ::setsockopt(
        asSocket(socket), SOL_SOCKET, option, reinterpret_cast<char const*>(&millis), sizeof(millis));
}

void armNoSigPipe(platform::NativeHandle /*socket*/) noexcept
{
    // Windows has no SIGPIPE.
}

std::expected<void, NetError> waitDialled(DialHandles const& handles, std::chrono::milliseconds timeout)
{
    // The dial's WSAEVENT is selected for FD_CONNECT, so it is signalled once the connect resolves,
    // either way. `pendingSocketError` then says which.
    auto event = static_cast<WSAEVENT>(handles.readiness);
    auto const millis = timeout.count() > 0 ? static_cast<DWORD>(timeout.count()) : WSA_INFINITE;
    auto const waited = ::WSAWaitForMultipleEvents(1, &event, FALSE, millis, FALSE);
    if (waited == WSA_WAIT_TIMEOUT)
        return std::unexpected(makeNetError(
            NetErrorCode::Timeout, 0, "connect timed out after " + std::to_string(timeout.count()) + "ms"));
    if (waited == WSA_WAIT_FAILED)
        return std::unexpected(socketError(::WSAGetLastError(), "WSAWaitForMultipleEvents"));
    return {};
}

std::expected<platform::NativeHandle, NetError> releaseBlocking(DialHandles& handles)
{
    auto const socket = asSocket(handles.socket);
    auto const event = static_cast<WSAEVENT>(handles.readiness);
    handles = DialHandles {};

    // FIONBIO and FIONREAD are unsigned constants and `ioctlsocket` takes a signed command, hence
    // the casts. `WSAEventSelect` forced the socket non-blocking, and `ioctlsocket` refuses to clear FIONBIO
    // while an event selection is in force -- so the selection is cancelled first, then the event
    // closed, then the mode restored.
    auto const deselected = ::WSAEventSelect(socket, nullptr, 0) == 0;
    ::WSACloseEvent(event);
    auto mode = u_long { 0 };
    if (!deselected || ::ioctlsocket(socket, static_cast<long>(FIONBIO), &mode) != 0)
    {
        auto const error = socketError(::WSAGetLastError(), "ioctlsocket(FIONBIO=0)");
        std::ignore = ::closesocket(socket);
        return std::unexpected(error);
    }
    return reinterpret_cast<platform::NativeHandle>(socket);
}

} // namespace core::net::detail
