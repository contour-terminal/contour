// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A reliable, ordered byte channel between Contour and its extension host process: one end of a connected
/// pair. A Unix-domain stream socket on POSIX, an overlapped named pipe on Windows.
///
/// Its I/O blocks. shutdown() is what makes a blocked receive return, from any thread, on both ends.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>

namespace vtextension::wasm::remote::transport
{

/// Why a channel operation failed.
enum class ChannelError : uint8_t
{
    Closed, ///< The peer closed its end, or this end was shut down.
    Io,     ///< The operating system reported any other failure.
};

#ifdef _WIN32
/// The operating system's name for one end: a pipe HANDLE.
using NativeHandle = void*;
#else
/// The operating system's name for one end: a socket descriptor.
using NativeHandle = int;
#endif

/// One end of a connected byte channel. Move-only; the end is closed when its owner is destroyed. A
/// moved-from channel may only be shut down, assigned to or destroyed.
class Channel
{
  public:
    /// The platform's state of one end; defined by Channel_posix.cpp or Channel_win32.cpp.
    struct Native;

    /// Takes ownership of an open end.
    /// @param native The end. Never null.
    explicit Channel(std::unique_ptr<Native> native) noexcept;

    /// Takes ownership of an end this process inherited from its parent: a stream socket on POSIX, a pipe
    /// opened for overlapped I/O on Windows. The end is made close-on-exec (not inheritable) again, and on
    /// macOS SIGPIPE-free, as makeChannelPair()'s ends are.
    /// @param handle The end.
    /// @return The channel; `Io` if @p handle names no open socket (pipe), in which case it stays the
    ///         caller's.
    [[nodiscard]] static std::expected<Channel, ChannelError> adopt(NativeHandle handle);

    ~Channel();
    Channel(Channel&&) noexcept;
    Channel& operator=(Channel&&) noexcept;
    Channel(Channel const&) = delete;
    Channel& operator=(Channel const&) = delete;

    /// Sends all of @p bytes, blocking until the channel took them.
    /// @param bytes The bytes to send.
    /// @return Nothing once every byte is sent; `Closed` if the peer is gone or this end was shut down.
    [[nodiscard]] std::expected<void, ChannelError> send(std::span<uint8_t const> bytes);

    /// Receives at least one byte, blocking until one arrives.
    /// @param buffer Where the bytes go. Not empty.
    /// @return How many bytes arrived, at least one; `Closed` at the peer's end of stream, or once this end
    ///         was shut down.
    [[nodiscard]] std::expected<std::size_t, ChannelError> receiveSome(std::span<uint8_t> buffer);

    /// Ends the stream, both ways and for both ends: a receive or send blocked on this end now, from any
    /// thread, returns `Closed`, as does every later one; and the peer's stream ends as if this end were
    /// closed, so that its receives return `Closed` once they drained what was sent before, and its sends
    /// return `Closed`. Idempotent and thread-safe.
    ///
    /// On POSIX the descriptor stays open until the channel is destroyed. On Windows, where nothing short of
    /// closing a pipe's handle ends the peer's stream, the handle is closed as soon as no transfer on this
    /// end is in flight, and nativeHandle() no longer names it.
    void shutdown() noexcept;

    /// @return The end's descriptor or handle, still owned by the channel: for handing it to a child
    ///         process, and for inspecting it. On Windows, INVALID_HANDLE_VALUE once shutdown() closed it.
    ///         Not to be called, nor its value used, while another thread may call shutdown(): on Windows the
    ///         handle may be closed, and its value reused, at any moment after shutdown() begins.
    [[nodiscard]] NativeHandle nativeHandle() const noexcept;

  private:
    std::unique_ptr<Native> _native;
};

/// Two connected ends: what one sends, the other receives.
struct ChannelPair
{
    Channel parent; ///< Contour's end.
    Channel helper; ///< The extension host process's end.
};

/// Creates a connected pair. Neither end is inherited by a child process unless it is handed over on
/// purpose.
/// @return The pair; `Io` if the operating system refused.
[[nodiscard]] std::expected<ChannelPair, ChannelError> makeChannelPair();

} // namespace vtextension::wasm::remote::transport
