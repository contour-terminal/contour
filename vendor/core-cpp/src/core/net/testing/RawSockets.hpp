// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Plain OS sockets for a test that must stand outside core-cpp: a connection accepted with the
/// platform's own calls, a blocking send and receive on its peer end, and the OS handle of a socket
/// or listener core-cpp built, so a case can ask the kernel what it was given.
///
/// One declaration, and an implementation per platform in `testing/posix/RawSockets.cpp` and
/// `testing/windows/RawSockets.cpp` -- the same split as @c makeSocketPair -- so the cases that
/// use it carry no `#ifdef` of their own.

#include <core/net/IListener.hpp>
#include <core/net/ISocket.hpp>
#include <core/platform/Types.hpp>

#include <cstddef>
#include <span>
#include <string_view>

namespace core::net::testing
{

/// Closes @p handle, if it is one; @c platform::InvalidHandle is ignored.
/// @param handle A socket handle this test owns.
void closeRawSocket(platform::NativeHandle handle) noexcept;

/// A socket handle a test owns, closed when this goes unless it was released.
class RawSocket
{
  public:
    /// @param handle The handle to own; may be @c platform::InvalidHandle.
    explicit RawSocket(platform::NativeHandle handle) noexcept: _handle(handle) {}

    RawSocket(RawSocket const&) = delete;
    RawSocket(RawSocket&&) = delete;
    RawSocket& operator=(RawSocket const&) = delete;
    RawSocket& operator=(RawSocket&&) = delete;

    ~RawSocket() { closeRawSocket(_handle); }

    /// @return The handle, still owned here.
    [[nodiscard]] platform::NativeHandle get() const noexcept { return _handle; }

    /// @return The handle, no longer owned here.
    [[nodiscard]] platform::NativeHandle release() noexcept
    {
        auto const handle = _handle;
        _handle = platform::InvalidHandle;
        return handle;
    }

  private:
    platform::NativeHandle _handle;
};

/// @return A fresh IPv4 TCP socket nothing has configured, or @c platform::InvalidHandle.
[[nodiscard]] platform::NativeHandle openRawTcpSocket() noexcept;

/// Both ends of one loopback TCP connection, made with blocking platform calls and no core-cpp.
struct RawConnection
{
    platform::NativeHandle client = platform::InvalidHandle;   ///< The dialling end.
    platform::NativeHandle accepted = platform::InvalidHandle; ///< The accepted end.
};

/// @return A loopback connection the caller owns both ends of, or two invalid handles.
[[nodiscard]] RawConnection rawLoopbackConnection() noexcept;

/// Sends all of @p payload with one blocking call.
/// @param handle A connected socket.
/// @param payload What to send.
/// @return Whether every byte went.
[[nodiscard]] bool rawSendAll(platform::NativeHandle handle, std::string_view payload) noexcept;

/// Receives into @p into with one blocking call.
/// @param handle A connected socket.
/// @param into Where the bytes go.
/// @return How many arrived; zero or less at EOF or on an error.
[[nodiscard]] std::ptrdiff_t rawReceive(platform::NativeHandle handle, std::span<char> into) noexcept;

/// @param socket A socket core-cpp built.
/// @return Its OS handle, or @c platform::InvalidHandle for a transport that has none (an
///         in-memory one).
[[nodiscard]] platform::NativeHandle nativeHandleOf(ISocket const& socket) noexcept;

/// @param listener A TCP listener core-cpp built.
/// @return Its listening OS handle, or @c platform::InvalidHandle for one that has none.
[[nodiscard]] platform::NativeHandle nativeHandleOf(IListener const& listener) noexcept;

} // namespace core::net::testing
