// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one line `contour daemon --stdio` writes before the native protocol starts, and its scanner.
///
/// Nothing may follow the ready line: the native server reads a ClientHello before it writes anything,
/// and the client has not sent one yet, so the only bytes ever ahead of the protocol are the preamble
/// and whatever the remote login printed before it.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace vthost
{

/// How every ready line begins: words may follow it, separated by spaces, up to the end of the line.
inline constexpr std::string_view StdioPreambleReadyPrefix = "contour-stdio 1 ready";
/// The relay reached the daemon; the native protocol follows.
inline constexpr std::string_view StdioPreambleReady = "contour-stdio 1 ready\n";
/// The word a relay told not to start a daemon (`contour daemon --stdio --no-start`) adds to its ready line:
/// it honoured the flag. A client that asked for it reads a ready line without it as a relay that did not
/// get the flag -- too old for it, or behind a `remote_command:` that does not pass it on.
inline constexpr std::string_view StdioNoStartWord = "no-start";
/// The ready line of a relay told not to start a daemon.
inline constexpr std::string_view StdioPreambleReadyNoStart = "contour-stdio 1 ready no-start\n";

static_assert(StdioPreambleReady.starts_with(StdioPreambleReadyPrefix)
                  && StdioPreambleReadyNoStart.starts_with(StdioPreambleReadyPrefix)
                  && StdioPreambleReadyNoStart.contains(StdioNoStartWord),
              "every ready line begins with the ready prefix");

/// What a relay's ready line states of `--no-start`.
enum class RelayNoStart : uint8_t
{
    Unstated = 0, ///< Nothing: a relay that may have started the daemon.
    Honoured,     ///< The relay was told not to start a daemon, and did not.
};
/// The relay could not reach the daemon; the reason follows, up to the end of the line.
inline constexpr std::string_view StdioPreambleErrorPrefix = "contour-stdio 1 error: ";
/// The reason a relay told not to start a daemon (`contour daemon --stdio --no-start`) gives, as its error
/// preamble's message, when none serves. The client reads a refusal that starts with it as
/// TransportErrorKind::DaemonAbsent rather than as a refusal: the daemon is not running, and can be started.
inline constexpr std::string_view StdioDaemonAbsentMessage = "no daemon is running";
/// How many bytes of junk (a login banner, a motd) may come before the preamble.
inline constexpr std::size_t StdioPreambleScanLimit = std::size_t { 64 } * 1024;

/// No preamble yet; more bytes are needed.
struct PreambleIncomplete
{
};

/// The ready line arrived.
struct PreambleReady
{
    std::size_t skipped = 0;                       ///< Bytes of junk before it.
    RelayNoStart noStart = RelayNoStart::Unstated; ///< What it states of `--no-start`.
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

/// @param noStart What the relay states of `--no-start`.
/// @return The ready line: StdioPreambleReadyNoStart for a relay that honoured it, else StdioPreambleReady.
[[nodiscard]] constexpr std::string_view stdioPreambleReady(RelayNoStart noStart) noexcept
{
    return noStart == RelayNoStart::Honoured ? StdioPreambleReadyNoStart : StdioPreambleReady;
}

/// @return The error preamble carrying @p message, on one line: each '\r' or '\n' in it becomes ' '.
[[nodiscard]] std::string stdioPreambleError(std::string_view message);

} // namespace vthost
