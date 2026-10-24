// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// An in-memory IByteStream pair: what one end writes, the other reads, in both directions.

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>

#include <vthost/ByteStream.hpp>

namespace vthost::testing
{

/// Whether a direction of a memory stream still takes bytes.
enum class MemoryPipeState : std::uint8_t
{
    Open = 0, ///< Writes are accepted.
    Closed,   ///< The writer closed it; the reader sees end of stream once the queue drains.
};

/// One direction of a memory stream: a byte queue guarded for one writer and one reader thread.
class MemoryPipe
{
  public:
    /// Blocks until a byte is queued or the pipe is closed.
    /// @return The count moved into @p buffer; 0 once closed and drained.
    [[nodiscard]] std::size_t read(std::span<std::byte> buffer)
    {
        auto lock = std::unique_lock { _mutex };
        _changed.wait(lock, [this]() { return !_bytes.empty() || _state == MemoryPipeState::Closed; });
        auto const count = std::min(buffer.size(), _bytes.size());
        std::ranges::copy_n(_bytes.begin(), static_cast<std::ptrdiff_t>(count), buffer.begin());
        _bytes.erase(_bytes.begin(), _bytes.begin() + static_cast<std::ptrdiff_t>(count));
        return count;
    }

    /// Queues @p bytes. @return Nothing, or an error once the pipe is closed.
    [[nodiscard]] std::expected<void, std::string> write(std::span<std::byte const> bytes)
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            if (_state == MemoryPipeState::Closed)
                return std::unexpected { std::string { "the stream is closed for writing" } };
            _bytes.insert(_bytes.end(), bytes.begin(), bytes.end());
        }
        _changed.notify_all();
        return {};
    }

    /// Ends the pipe for its reader. Idempotent.
    void close() noexcept
    {
        {
            auto const lock = std::scoped_lock { _mutex };
            _state = MemoryPipeState::Closed;
        }
        _changed.notify_all();
    }

  private:
    std::mutex _mutex;
    std::condition_variable _changed;
    std::deque<std::byte> _bytes;
    MemoryPipeState _state = MemoryPipeState::Open;
};

/// One end of a memory stream pair.
class MemoryByteStream final: public IByteStream
{
  public:
    /// @param incoming What this end reads. @param outgoing What this end writes.
    MemoryByteStream(std::shared_ptr<MemoryPipe> incoming, std::shared_ptr<MemoryPipe> outgoing):
        _incoming { std::move(incoming) }, _outgoing { std::move(outgoing) }
    {
    }

    [[nodiscard]] std::expected<std::size_t, std::string> readSome(std::span<std::byte> buffer) override
    {
        return _incoming->read(buffer);
    }

    [[nodiscard]] std::expected<void, std::string> writeAll(std::span<std::byte const> bytes) override
    {
        return _outgoing->write(bytes);
    }

    void closeWrite() noexcept override { _outgoing->close(); }

  private:
    std::shared_ptr<MemoryPipe> _incoming;
    std::shared_ptr<MemoryPipe> _outgoing;
};

/// Two connected memory streams: what @ref a writes, @ref b reads, and the other way round.
struct MemoryStreamPair
{
    std::shared_ptr<IByteStream> a; ///< One end.
    std::shared_ptr<IByteStream> b; ///< The other end.
};

/// @return A connected pair over two byte queues.
[[nodiscard]] inline MemoryStreamPair makeMemoryStreamPair()
{
    auto aToB = std::make_shared<MemoryPipe>();
    auto bToA = std::make_shared<MemoryPipe>();
    return MemoryStreamPair { .a = std::make_shared<MemoryByteStream>(bToA, aToB),
                              .b = std::make_shared<MemoryByteStream>(aToB, bToA) };
}

} // namespace vthost::testing
