// SPDX-License-Identifier: Apache-2.0
#include <net/AsyncBufferedReader.h>

#include <array>
#include <cstring>
#include <span>

namespace net
{

coro::Task<std::expected<std::string, NetError>> AsyncBufferedReader::readLine()
{
    while (true)
    {
        // Search ONLY the bytes that arrived since the last search: everything
        // before _scanOffset has already been examined and holds no LF.
        auto const unscanned = _buffer.size() - _scanOffset;
        auto const* const found =
            static_cast<char const*>(std::memchr(_buffer.data() + _scanOffset, '\n', unscanned));
        if (found != nullptr)
        {
            auto const newline = static_cast<std::size_t>(found - _buffer.data());
            _scannedBytes += (newline + 1) - _scanOffset;

            auto lineEnd = newline;
            if (lineEnd > 0 && _buffer[lineEnd - 1] == '\r')
                --lineEnd; // tolerate CRLF from client line disciplines

            if (lineEnd > _maxLineLength)
                co_return std::unexpected(
                    makeNetError(NetErrorCode::MessageTooLarge, 0, "line exceeds bound"));

            auto line = _buffer.substr(0, lineEnd);
            _buffer.erase(0, newline + 1);
            _scanOffset = 0;
            co_return line;
        }
        _scannedBytes += unscanned;
        _scanOffset = _buffer.size();

        // No terminator buffered. Refuse to grow past the bound: a peer that
        // never sends LF must not balloon the buffer.
        if (_buffer.size() > _maxLineLength)
            co_return std::unexpected(makeNetError(NetErrorCode::MessageTooLarge, 0, "line exceeds bound"));

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

} // namespace net
