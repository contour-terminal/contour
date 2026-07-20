// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `AsyncBufferedReader` — buffered line reading over an @c ISocket.
///
/// The line-oriented tmux control-mode protocol needs `readLine`, which no
/// transport offers directly. This reader owns the buffering and — unlike the
/// naive accumulate-and-re-search pattern (Endo's HTTP header reader re-scanned
/// its whole buffer after every fill, quadratic in the number of reads) — tracks
/// a scan offset so every buffered byte is examined exactly once, no matter how
/// fragmented the arrivals are.
///
/// Lines are byte strings, deliberately NOT validated as UTF-8: control-mode
/// payloads (capture-pane output) may carry arbitrary bytes.

#include <cstddef>
#include <expected>
#include <string>

#include <coro/Task.hpp>
#include <net/ISocket.h>
#include <net/IoResult.h>

namespace net
{

/// Reads LF-terminated lines from a socket through an internal buffer.
///
/// Not thread-safe and single-consumer: at most one `readLine()` may be awaited
/// at a time (it parks on the socket's read path).
class AsyncBufferedReader
{
  public:
    /// Default cap on a single line's length (guards a peer that never sends LF).
    static constexpr std::size_t DefaultMaxLineLength = 1U << 20U; // 1 MiB

    /// @param socket The transport to read from (not owned; must outlive the reader).
    /// @param maxLineLength Reject any line longer than this many bytes
    ///        (terminator excluded) with @c NetErrorCode::MessageTooLarge.
    explicit AsyncBufferedReader(ISocket* socket, std::size_t maxLineLength = DefaultMaxLineLength) noexcept:
        _socket(socket), _maxLineLength(maxLineLength)
    {
    }

    /// Reads the next LF-terminated line, filling from the socket as needed.
    ///
    /// The trailing LF is stripped, as is one optional CR before it (client line
    /// disciplines inject CRLF; the protocol is LF-terminated).
    /// @return The line's bytes (possibly empty for a bare terminator); or
    ///         @c NetErrorCode::Eof once the peer closed — including mid-line, where
    ///         the unterminated tail is dropped (a connection that died mid-line has
    ///         no valid line to deliver); @c NetErrorCode::MessageTooLarge when the
    ///         line bound is exceeded (the connection is poisoned — close it); or
    ///         the socket's own read error.
    [[nodiscard]] coro::Task<std::expected<std::string, NetError>> readLine();

    /// @return The number of bytes buffered but not yet consumed (tests/diagnostics).
    [[nodiscard]] std::size_t buffered() const noexcept { return _buffer.size(); }

    /// @return The total number of bytes the line scanner has examined so far.
    ///         Tests assert this stays equal to the bytes consumed — i.e. every
    ///         byte is scanned exactly once regardless of read fragmentation.
    [[nodiscard]] std::size_t scannedBytes() const noexcept { return _scannedBytes; }

  private:
    ISocket* _socket;              ///< The transport read from (not owned).
    std::size_t _maxLineLength;    ///< Reject lines longer than this.
    std::string _buffer;           ///< Received-but-unconsumed bytes.
    std::size_t _scanOffset = 0;   ///< First buffer index not yet searched for LF.
    std::size_t _scannedBytes = 0; ///< Lifetime count of bytes examined (see scannedBytes()).
};

} // namespace net
