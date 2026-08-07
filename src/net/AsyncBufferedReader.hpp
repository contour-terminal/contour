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
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

#include <coro/Task.hpp>
#include <net/ISocket.hpp>
#include <net/IoResult.hpp>

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
    ///        (terminator excluded) with @c NetErrorCode::MessageTooLarge. The bound
    ///        is checked between refills, so the buffer may reach the bound plus one
    ///        read chunk before the refusal fires — it caps memory, and is not an
    ///        exact byte count of what was accepted.
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

    /// Reads until @p delimiter is seen, filling from the socket as needed.
    ///
    /// Like @c readLine, every buffered byte is examined at most once across
    /// refills: the scan resumes at the last position that could still begin a
    /// match, so a delimiter split across two reads is still found without
    /// re-searching the prefix.
    /// @param delimiter The byte sequence to stop after (must not be empty).
    /// @return The bytes preceding @p delimiter, with the delimiter consumed but
    ///         not returned; or @c NetErrorCode::Eof once the peer closed before
    ///         the delimiter arrived (the buffered tail is dropped);
    ///         @c NetErrorCode::MessageTooLarge if the bound is exceeded first; or
    ///         the socket's own read error.
    [[nodiscard]] coro::Task<std::expected<std::string, NetError>> readUntil(std::string_view delimiter);

    /// Reads exactly @p count bytes, filling from the socket as needed.
    /// @param count The number of bytes to deliver (0 returns an empty string).
    /// @return The bytes; or @c NetErrorCode::Eof if the peer closed first (the
    ///         partial tail is dropped — a truncated message is not a message); or
    ///         the socket's own read error. Not subject to the line-length bound:
    ///         the caller has already agreed to @p count (e.g. from Content-Length).
    [[nodiscard]] coro::Task<std::expected<std::string, NetError>> readExactly(std::size_t count);

    /// @return The number of bytes buffered but not yet consumed (tests/diagnostics).
    [[nodiscard]] std::size_t buffered() const noexcept { return _buffer.size() - _consumed; }

    /// @return True if at least one more complete (LF-terminated) line is already
    ///         buffered, so the next @c readLine returns without touching the
    ///         socket. Consumers use this as a burst boundary: once it is false,
    ///         the batch that arrived together has been fully delivered.
    [[nodiscard]] bool hasBufferedLine() const noexcept
    {
        return _buffer.find('\n', _consumed) != std::string::npos;
    }

    /// @return The total number of bytes the line scanner has examined so far.
    ///         Tests assert this stays equal to the bytes consumed — i.e. every
    ///         byte is scanned exactly once regardless of read fragmentation.
    [[nodiscard]] std::size_t scannedBytes() const noexcept { return _scannedBytes; }

  private:
    /// Compacts the delivered prefix and appends one chunk read from the socket.
    /// @return Nothing on success; @c NetErrorCode::Eof once the peer closed, or the
    ///         socket's own error. The two are kept distinct on purpose: reporting a
    ///         connection reset as a clean EOF would tell the caller the message
    ///         simply ended when in fact the transport failed.
    [[nodiscard]] coro::Task<std::expected<void, NetError>> fill();

    /// Which scanner last set @c _scanOffset. The offset is an optimization private
    /// to one scanning strategy — "not yet searched for LF" for @c readLine, "could
    /// still begin a delimiter match" for @c readUntil — so a switch must reset it
    /// rather than inherit a bound that means something else. Without this, a
    /// @c readLine that ends past a buffered delimiter (its MessageTooLarge exit
    /// returns without compacting) makes the next @c readUntil skip data it holds.
    enum class Scanner : std::uint8_t
    {
        None = 0, ///< Nothing scanned yet; the offset carries no meaning.
        Line,     ///< @c readLine set it: everything before it holds no LF.
        Until,    ///< @c readUntil set it: no match can begin before it.
    };

    /// Resets @c _scanOffset when the scanning strategy changes.
    /// @param scanner The scanner about to run.
    void beginScan(Scanner scanner) noexcept
    {
        if (_scanner != scanner)
        {
            _scanOffset = _consumed;
            _scanner = scanner;
        }
    }

    ISocket* _socket;                 ///< The transport read from (not owned).
    std::size_t _maxLineLength;       ///< Reject lines longer than this.
    std::string _buffer;              ///< Received bytes; [0, _consumed) already delivered.
    std::size_t _consumed = 0;        ///< First buffer index not yet delivered (a read cursor).
    std::size_t _scanOffset = 0;      ///< Scan resume point; meaning depends on _scanner.
    Scanner _scanner = Scanner::None; ///< Which scanner _scanOffset belongs to.
    std::size_t _scannedBytes = 0;    ///< Lifetime count of bytes examined (see scannedBytes()).
};

} // namespace net
