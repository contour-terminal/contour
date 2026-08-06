// SPDX-License-Identifier: Apache-2.0
#include <net/AsyncBufferedReader.hpp>

#include <array>
#include <cstring>
#include <span>
#include <vector>

#include <net/Sockets.hpp>

namespace net
{

coro::Task<std::expected<std::string, NetError>> AsyncBufferedReader::readLine()
{
    while (true)
    {
        // Search ONLY the bytes that arrived since the last search: everything in
        // [_consumed, _scanOffset) has already been examined and holds no LF.
        auto const unscanned = _buffer.size() - _scanOffset;
        auto const* const found =
            static_cast<char const*>(std::memchr(_buffer.data() + _scanOffset, '\n', unscanned));
        if (found != nullptr)
        {
            auto const newline = static_cast<std::size_t>(found - _buffer.data());
            _scannedBytes += (newline + 1) - _scanOffset;

            auto lineEnd = newline;
            if (lineEnd > _consumed && _buffer[lineEnd - 1] == '\r')
                --lineEnd; // tolerate CRLF from client line disciplines

            auto const lineLength = lineEnd - _consumed;
            if (lineLength > _maxLineLength)
                co_return std::unexpected(
                    makeNetError(NetErrorCode::MessageTooLarge, 0, "line exceeds bound"));

            // Advance a read cursor instead of erasing from the front: a burst of
            // buffered lines is delivered without a memmove per line (the front
            // erase was O(bytes) each; the prefix is reclaimed lazily in compact()).
            auto line = _buffer.substr(_consumed, lineLength);
            _consumed = newline + 1;
            _scanOffset = _consumed;
            co_return line;
        }
        _scannedBytes += unscanned;
        _scanOffset = _buffer.size();

        // No terminator buffered. Refuse to grow the in-progress line past the
        // bound: a peer that never sends LF must not balloon the buffer.
        if (_buffer.size() - _consumed > _maxLineLength)
            co_return std::unexpected(makeNetError(NetErrorCode::MessageTooLarge, 0, "line exceeds bound"));

        // Reclaim the already-delivered prefix before growing the buffer. Compacting
        // here — only when we actually go back to the socket — bounds the buffer to
        // the in-progress line plus one chunk, without a per-line memmove.
        if (_consumed > 0)
        {
            _buffer.erase(0, _consumed);
            _scanOffset -= _consumed;
            _consumed = 0;
        }

        auto chunk = std::array<std::byte, 4096> {};
        auto const got = co_await _socket->read(chunk);
        if (!got.has_value())
            co_return std::unexpected(got.error());
        if (*got == 0)
            // Peer closed. A buffered unterminated tail is dropped deliberately:
            // the connection died mid-line, so there is no valid line to deliver.
            co_return std::unexpected(makeNetError(NetErrorCode::Eof, 0, "peer closed"));

        _buffer.append(reinterpret_cast<char const*>(chunk.data()), *got);
    }
}

coro::Task<IoResult> appendReadChunk(ISocket* socket, std::vector<std::byte>* buffer)
{
    // Straight into the buffer's tail. A local scratch array would be a COROUTINE-FRAME member, so
    // every read on this path — per client per input burst on the daemon, per delta on a client —
    // meant one heap allocation of a >16 KiB frame plus a second copy of the bytes just received.
    // Growing and shrinking the vector costs neither allocation nor copy after the first call: the
    // capacity survives the shrink, so a steady stream reuses the same block.
    constexpr auto ChunkSize = std::size_t { 16384 };
    auto const offset = buffer->size();
    buffer->resize(offset + ChunkSize);
    auto const n = co_await socket->read(std::span { buffer->data() + offset, ChunkSize });
    // The tail is truncated on EVERY exit, error included: it holds nothing the caller may see.
    buffer->resize(offset + (n.has_value() ? *n : std::size_t { 0 }));
    if (!n.has_value())
        co_return std::unexpected(n.error());
    co_return *n;
}

} // namespace net
