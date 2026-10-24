// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one line `contour daemon --stdio` writes before the native protocol starts, and its scanner.
///
/// Nothing may follow the ready line: the native server reads a ClientHello before it writes anything,
/// and the client has not sent one yet, so the only bytes ever ahead of the protocol are the preamble
/// and whatever the remote login printed before it.

#include <cstddef>
#include <string>
#include <string_view>
#include <variant>

namespace vthost
{

/// The relay reached the daemon; the native protocol follows.
inline constexpr std::string_view StdioPreambleReady = "contour-stdio 1 ready\n";
/// The relay could not reach the daemon; the reason follows, up to the end of the line.
inline constexpr std::string_view StdioPreambleErrorPrefix = "contour-stdio 1 error: ";
/// How many bytes of junk (a login banner, a motd) may come before the preamble.
inline constexpr std::size_t StdioPreambleScanLimit = std::size_t { 64 } * 1024;

/// No preamble yet; more bytes are needed.
struct PreambleIncomplete
{
};

/// The ready line arrived.
struct PreambleReady
{
    std::size_t skipped = 0; ///< Bytes of junk before it.
};

/// The error line arrived.
struct PreambleRefused
{
    std::string message;     ///< Why the relay could not reach the daemon.
    std::size_t skipped = 0; ///< Bytes of junk before it.
};

/// No preamble within @ref StdioPreambleScanLimit bytes of junk.
struct PreambleOverflow
{
};

/// The ready line arrived, but bytes followed it, which the protocol forbids.
struct PreambleTrailing
{
    std::size_t extra = 0; ///< Bytes after the ready line.
};

/// What @ref scanPreamble found.
using PreambleScan =
    std::variant<PreambleIncomplete, PreambleReady, PreambleRefused, PreambleOverflow, PreambleTrailing>;

/// Looks for the preamble in everything received so far.
/// @param received Every byte read from the relay so far.
/// @return What was found; @ref PreambleIncomplete asks for more bytes.
[[nodiscard]] PreambleScan scanPreamble(std::string_view received);

/// @return The error preamble carrying @p message, on one line: each '\r' or '\n' in it becomes ' '.
[[nodiscard]] std::string stdioPreambleError(std::string_view message);

} // namespace vthost
