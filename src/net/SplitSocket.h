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

    [[nodiscard]] coro::Task<IoResult> write(std::span<std::byte const> buffer) override
    {
        return _writeHalf->write(buffer);
    }

    void close() noexcept override
    {
        _readHalf->close();
        _writeHalf->close();
    }

    [[nodiscard]] bool isClosed() const noexcept override { return _readHalf->isClosed(); }

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
