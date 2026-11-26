// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// A reliable, ordered byte channel between Contour and its extension host process: one end of a connected
/// pair. A Unix-domain stream socket on POSIX, an overlapped named pipe on Windows.
///
/// Its I/O blocks, but for trySend(). shutdown() is what makes a blocked receive return, from any thread, on
/// both ends.
///
/// One thread at a time sends on an end -- send(), trySend() or sendWithHandle() -- and one at a time
/// receives on it, while a receive may run beside a send: on Windows each direction of an end waits on one
/// event of its own. trySend() relies on that where the system ignores MSG_DONTWAIT (macOS): the room it
/// found free is still free when it sends.

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <vector>

namespace vtextension::wasm::remote::transport
{

/// Why a channel operation failed.
enum class ChannelError : uint8_t
{
    Closed, ///< The peer closed its end, or this end was shut down.
    Io,     ///< The operating system reported any other failure.
    /// The peer attached what no message carries: more descriptors than a receive takes in, or ancillary
    /// data other than descriptors (receiveSomeWithHandles()); or, sending, the peer's end refuses
    /// descriptors (sendWithHandle()).
    Refused,
};

#ifdef _WIN32
/// The operating system's name for one end: a pipe HANDLE.
using NativeHandle = void*;
#else
/// The operating system's name for one end: a socket descriptor.
using NativeHandle = int;
#endif

class OwnedHandle;

/// Closes a handle this process owns: close() on POSIX, CloseHandle() on Windows.
/// @param handle The handle; never used again.
void closeNativeHandle(NativeHandle handle) noexcept;

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

    /// Sends as much of @p bytes as the channel takes now, without blocking. No other send may run on this
    /// end meanwhile (see the class).
    /// @param bytes The bytes to send.
    /// @return How many of them, from the front, were sent: 0 if none fit; `Closed` if the peer is gone or
    ///         this end was shut down.
    [[nodiscard]] std::expected<std::size_t, ChannelError> trySend(std::span<uint8_t const> bytes);

    /// Receives at least one byte, blocking until one arrives. A descriptor the peer attached to them is
    /// never kept: the system discards it, or, where it installs it all the same (macOS), it is closed at
    /// once.
    /// @param buffer Where the bytes go. Not empty.
    /// @return How many bytes arrived, at least one; `Closed` at the peer's end of stream, or once this end
    ///         was shut down.
    [[nodiscard]] std::expected<std::size_t, ChannelError> receiveSome(std::span<uint8_t> buffer);

#ifndef _WIN32
    /// Sends all of @p bytes, as send() does, with a duplicate of @p handle attached to their first byte
    /// (SCM_RIGHTS): the peer receives it with that byte, as receiveSomeWithHandles() does.
    /// @param bytes The bytes to send; not empty.
    /// @param handle The descriptor; still the caller's.
    /// @return Nothing once every byte is sent; `Closed` if the peer is gone or this end was shut down;
    ///         `Refused`, with nothing sent, if the peer's end refuses descriptors
    ///         (UpwardDescriptors::Refused).
    [[nodiscard]] std::expected<void, ChannelError> sendWithHandle(std::span<uint8_t const> bytes,
                                                                   NativeHandle handle);

    /// Receives at least one byte, as receiveSome() does, and installs the descriptors attached to them,
    /// close-on-exec. At least two are taken in at once, so that a reader tells a peer that attached one
    /// more than a message may carry from one that did not. Past that, Linux and FreeBSD truncate them, and
    /// the receive fails; macOS, which installs every one before it truncates, has every one a message can
    /// carry taken in, so that none stays installed unseen.
    /// @param buffer Where the bytes go. Not empty.
    /// @param handles Where the descriptors that arrived are appended, now the caller's.
    /// @return How many bytes arrived, at least one; `Closed` as receiveSome(); `Refused` if the system
    ///         truncated what was attached, or something other than descriptors was, every descriptor
    ///         received then closed; `Io` if a received descriptor could not be made close-on-exec.
    [[nodiscard]] std::expected<std::size_t, ChannelError> receiveSomeWithHandles(
        std::span<uint8_t> buffer, std::vector<OwnedHandle>& handles);
#endif

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

/// What becomes of a descriptor the extension host process attaches to what it sends Contour.
///
/// Contour reads its ends with plain receives, so nothing a helper attaches is ever kept. But where the
/// system merely discards such a descriptor, or installs it all the same for the receive to close (macOS),
/// it is closed in the receiving thread, and a last close can block without bound (a socket lingering on a
/// full send buffer): a compromised helper could hang a thread of Contour's. Where the system lets an end
/// refuse descriptors outright (Linux 6.16 and later, `SO_PASSRIGHTS`), the helper's send fails instead, and
/// no descriptor ever reaches Contour's process.
enum class UpwardDescriptors : uint8_t
{
    Discarded, ///< They are taken in and closed, in the receiving thread (older kernels, BSDs, macOS).
    Refused,   ///< The system refuses to send them: nothing reaches Contour (Linux 6.16+, and Windows, where
               ///< no handle travels beside the bytes).
};

/// Two connected ends: what one sends, the other receives.
struct ChannelPair
{
    Channel parent; ///< Contour's end.
    Channel helper; ///< The extension host process's end; it takes descriptors (Contour's grants).
    UpwardDescriptors upward {}; ///< What becomes of a descriptor sent from the helper's end to Contour's.
};

/// What becomes of descriptors sent to Contour's end when making it refuse them failed with @p error: the
/// system cannot refuse them there (an older kernel, or an outer sandbox's filter or security module that
/// refuses the option), and they are discarded instead; any other error is the system failing.
/// @param error The errno setting the option failed with.
/// @return `Discarded`; `Io` for an error that says nothing about the option.
#ifndef _WIN32
[[nodiscard]] std::expected<UpwardDescriptors, ChannelError> upwardDescriptorsWhenUnrefused(
    int error) noexcept;
#endif

/// Creates a connected pair. Neither end is inherited by a child process unless it is handed over on
/// purpose. Contour's end refuses descriptors where the system can (UpwardDescriptors); the helper's end
/// never does, so that it still receives the grants Contour sends.
/// @return The pair; `Io` if the operating system refused.
[[nodiscard]] std::expected<ChannelPair, ChannelError> makeChannelPair();

} // namespace vtextension::wasm::remote::transport
