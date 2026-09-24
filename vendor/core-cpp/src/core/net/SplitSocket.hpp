// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SplitSocket` — one duplex ISocket made of two simplex halves.
///
/// The tmux imsg handshake passes the client's STDIN and STDOUT as two
/// separate descriptors; the control-mode engine consumes exactly one
/// duplex transport. This adapter reads from one half and writes to the
/// other, closing both together.

#include <core/net/ISocket.hpp>

#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace core::net
{

/// Reads from @p readHalf, writes to @p writeHalf; owns both.
class SplitSocket final: public ISocket
{
  public:
    /// @param readHalf The transport reads come from (owned).
    /// @param writeHalf The transport writes go to (owned).
    SplitSocket(std::unique_ptr<ISocket> readHalf, std::unique_ptr<ISocket> writeHalf):
        _readHalf(std::move(readHalf)), _writeHalf(std::move(writeHalf))
    {
    }

    /// **Every verb forwards the inner operation out by value, adding nothing.** A
    /// @c ResultAwaitable is neither copyable nor movable, so this is not a choice of style: the
    /// returned prvalue is constructed straight into the caller's frame, and the half's own arm and
    /// retire hooks point at the half. Which is exactly right — the slot being claimed is the
    /// half's, so its one-read-operation rule stays the half's to enforce.
    [[nodiscard]] IoAwaitable read(std::span<std::byte> buffer) override { return _readHalf->read(buffer); }

    /// Forwards to the read half so an fd passed over an fd-capable read half
    /// (SCM_RIGHTS) is not silently dropped by the base default.
    [[nodiscard]] ResultAwaitable<ReadWithFd> readWithFd(std::span<std::byte> buffer) override
    {
        return _readHalf->readWithFd(buffer);
    }

    [[nodiscard]] IoAwaitable waitReadable() override { return _readHalf->waitReadable(); }

    [[nodiscard]] IoAwaitable write(std::span<std::byte const> buffer) override
    {
        return _writeHalf->write(buffer);
    }

    [[nodiscard]] IoAwaitable writeVectored(std::span<std::span<std::byte const> const> segments,
                                            std::shared_ptr<void const> keepAlive = {}) override
    {
        return _writeHalf->writeVectored(segments, std::move(keepAlive));
    }

    /// Retires the READ half's parked operation. The write half has no read slot to retire, and
    /// forwarding there would be a call with nothing to do rather than a second retirement.
    void cancelRead() noexcept override { _readHalf->cancelRead(); }

    /// Half-closes the WRITE half, which is the only half that has a write side to close.
    [[nodiscard]] ResultAwaitable<void> shutdownWrite() override { return _writeHalf->shutdownWrite(); }

    /// Bounds a read, so it goes to the half reads come from.
    /// @param deadline How long a read may wait.
    void setReceiveDeadline(std::chrono::milliseconds deadline) noexcept override
    {
        _readHalf->setReceiveDeadline(deadline);
    }

    /// Closes both halves, and touches nothing of this object once a retirement has run.
    ///
    /// **Two retirements, and either may destroy this object.** Closing a half completes the
    /// operation parked on it, which resumes a coroutine that may own this socket and drop it
    /// before the half's `close()` returns -- taking both halves with it. Ordering the two calls
    /// cannot help, because whichever runs first can do that. So the second call is made only if
    /// this object is still alive afterwards, which @c _alive answers without touching a member.
    /// Where it is not, the second half was destroyed with it, and a destroyed socket ABANDONS
    /// whatever was parked on it (@c ResultAwaitable::abandon), so nothing is left unretired.
    void close() noexcept override
    {
        auto const alive = std::weak_ptr<void const> { _alive };
        _readHalf->close();
        if (alive.expired())
            return;
        _writeHalf->close();
    }

    /// Closed once EITHER half is: reading a dead read half or writing a dead write
    /// half both make the duplex socket unusable, so a one-sided closure counts.
    [[nodiscard]] bool isClosed() const noexcept override
    {
        return _readHalf->isClosed() || _writeHalf->isClosed();
    }

    /// Forwards to the read half so tmux sessions attached via combineHalves()
    /// report the real peer address instead of the ISocket default (empty).
    [[nodiscard]] std::string peerAddress() const override { return _readHalf->peerAddress(); }

  private:
    std::unique_ptr<ISocket> _readHalf;
    std::unique_ptr<ISocket> _writeHalf;

    /// Expires with this object, so @c close can tell whether a retirement destroyed it.
    std::shared_ptr<void const> _alive = std::make_shared<char>();
};

/// Combines two simplex transports into one duplex socket.
/// @param readHalf The transport reads come from (owned).
/// @param writeHalf The transport writes go to (owned).
/// @return The combined socket.
[[nodiscard]] inline std::unique_ptr<ISocket> combineHalves(std::unique_ptr<ISocket> readHalf,
                                                            std::unique_ptr<ISocket> writeHalf)
{
    return std::make_unique<SplitSocket>(std::move(readHalf), std::move(writeHalf));
}

} // namespace core::net
