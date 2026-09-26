// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `BlockingSocket` — an @c ISocket whose every operation blocks the calling thread and answers
/// before its awaitable is even returned.
///
/// **For threads that may block, and for nothing else.** A one-shot CLI, a health probe, a
/// heartbeat thread of its own: those callers drive it with `core::async::syncRun`, which is sound
/// precisely because nothing here ever suspends. A caller on a loop thread takes the loop's socket
/// (`connect`, `makeConnector`) instead, because a blocking call there stalls every other
/// connection the loop serves (`.agent/rules/async-and-net.md`, *a synchronous dial spends a thread
/// the caller does not own*).
///
/// Origin: fastcached `src/FastCache/Net/BlockingSocket.{hpp,cpp}`
/// (`0708dd54dc7ee72622c8c0783c2bd4a06f0e9b21`). Its `BlockingListener` and the `Detail` helper
/// layer are not imported: core-cpp's listeners own their bind sequence, and the helpers are the
/// dial primitives and the one socket-error table here.

#include <core/net/ISocket.hpp>
#include <core/net/IoAwaitable.hpp>
#include <core/platform/Types.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

namespace core::net
{

/// A connected stream socket whose reads and writes are plain blocking system calls.
///
/// Every awaitable it returns is already settled, so awaiting one never suspends. How long a call
/// may block is the socket's own `SO_RCVTIMEO` / `SO_SNDTIMEO`: @c setReceiveDeadline and
/// @c setSendDeadline set them, and a dial arms both before handing the socket over
/// (@c BlockingConnectorOptions::ioTimeout). A deadline that expires is an error
/// @c isDeadlineExpiry recognises.
class BlockingSocket final: public ISocket
{
  public:
    /// Takes ownership of a connected, BLOCKING stream socket and arms its SIGPIPE suppression.
    /// @param socket The native socket; closed by @c close or the destructor.
    /// @param peerAddress What @c peerAddress reports, or "" when unknown.
    explicit BlockingSocket(platform::NativeHandle socket, std::string peerAddress = {}) noexcept;
    ~BlockingSocket() override;

    BlockingSocket(BlockingSocket const&) = delete;
    BlockingSocket& operator=(BlockingSocket const&) = delete;
    BlockingSocket(BlockingSocket&&) = delete;
    BlockingSocket& operator=(BlockingSocket&&) = delete;

    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override;
    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override;
    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override;

    /// @copydoc ISocket::waitReadable
    ///
    /// **It BLOCKS until the socket is readable**, bounded by the receive deadline, rather than
    /// inheriting the interface's answer-`1`-at-once default: a transport whose reads block owes a
    /// wait that blocks, or a watch over it is a spin.
    [[nodiscard]] IoAwaitable waitReadable() override;

    /// @copydoc ISocket::cancelRead
    ///
    /// **A written no-op with a reason, rather than an inherited one.** Nothing here parks: every
    /// read has returned before its awaitable exists, so there is no frame to free and no slot to
    /// hand back. Stated because a default that is right for the transport you are thinking about
    /// is the dangerous kind, and two transports upstream inherited it wrongly.
    void cancelRead() noexcept override {}

    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override;

    /// @copydoc ISocket::setReceiveDeadline
    ///
    /// Sets `SO_RCVTIMEO`. A non-positive duration REMOVES the bound, as the interface says and as
    /// the option itself reads zero.
    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override;

    /// Bounds how long one write may block (`SO_SNDTIMEO`); non-positive removes the bound.
    /// @param deadline How long a single send may block.
    void setSendDeadline(std::chrono::milliseconds deadline) const noexcept;

    [[nodiscard]] std::string peerAddress() const override { return _peerAddress; }

    void close() noexcept override;

    /// @return True once @c close was called or a read observed the peer's EOF.
    [[nodiscard]] bool isClosed() const noexcept override { return _closed || _peerClosed; }

  private:
    /// Sends all of @p buffer, looping over partial sends.
    /// @return The bytes sent, which is all of them, or the error that stopped it.
    [[nodiscard]] IoResult sendAll(std::span<std::byte const> buffer) const;

    platform::NativeHandle _socket;
    std::string _peerAddress;
    bool _closed { false };
    bool _peerClosed { false };
};

} // namespace core::net
