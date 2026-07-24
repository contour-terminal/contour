// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `SplitSocket` — one duplex ISocket made of two simplex halves.
///
/// The tmux imsg handshake passes the client's STDIN and STDOUT as two
/// separate descriptors; the control-mode engine consumes exactly one
/// duplex transport. This adapter reads from one half and writes to the
/// other, closing both together.

#include <memory>

#include <net/ISocket.h>

namespace net
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

    [[nodiscard]] coro::Task<IoResult> read(std::span<std::byte> buffer) override
    {
        return _readHalf->read(buffer);
    }

    /// Forwards to the read half so an fd passed over an fd-capable read half
    /// (SCM_RIGHTS) is not silently dropped by the base default (which reports -1).
    [[nodiscard]] coro::Task<std::expected<ReadWithFd, NetError>> readWithFd(
        std::span<std::byte> buffer) override
    {
        return _readHalf->readWithFd(buffer);
    }

    [[nodiscard]] coro::Task<IoResult> write(std::span<std::byte const> buffer) override
    {
        return _writeHalf->write(buffer);
    }

    void close() noexcept override
    {
        _readHalf->close();
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

} // namespace net
