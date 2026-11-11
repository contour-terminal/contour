// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

/// Bytes with descriptors over an AF_UNIX stream socket (`SCM_RIGHTS`): the one place both ends of the
/// keeper's channel -- the keeper's PosixChannel and the daemon's KeeperChannel -- send and receive. POSIX
/// only.
namespace vthost::keeper
{

/// Why a send failed.
enum class SendError : uint8_t
{
    PeerGone = 0, ///< EPIPE, ECONNRESET: the peer closed; what it wrote before is still to be read.
    Broken,       ///< Anything else, a descriptor list over MaxFdsPerFrame among it.
};

/// Sends what of @p runs the socket takes now, gathered, with @p fds on the first byte. Never blocks, and
/// never raises SIGPIPE (MSG_NOSIGNAL; SO_NOSIGPIPE, set on the socket, where there is none).
/// @param socket A non-blocking AF_UNIX stream socket.
/// @param runs The bytes, in order; at most the first 64 runs are sent.
/// @param fds Descriptors to send with them, at most MaxFdsPerFrame; they stay the caller's.
/// @return How many bytes went: 0 when the socket takes nothing now; or why the send failed.
[[nodiscard]] std::expected<std::size_t, SendError> sendWithDescriptors(
    int socket, std::span<std::span<uint8_t const> const> runs, std::span<int const> fds);

/// Why a receive brought nothing.
enum class ReceiveError : uint8_t
{
    WouldBlock = 0, ///< Nothing is there now.
    PeerGone,       ///< ECONNRESET.
    Broken,         ///< Anything else.
    Truncated,      ///< The descriptor list was cut short (MSG_CTRUNC), or is longer than MaxFdsPerFrame; the
                    ///< ones that came are closed.
};

/// What one receive brought.
struct Received
{
    std::size_t bytes = 0; ///< How many bytes; 0 at the end of the stream.
    std::vector<int> fds;  ///< The descriptors that came with them, close-on-exec; now the caller's.
};

/// Reads once, without blocking, collecting the descriptors that came with the bytes.
/// @param socket An AF_UNIX stream socket.
/// @param buffer Where the bytes go.
/// @return What came; or why nothing did.
[[nodiscard]] std::expected<Received, ReceiveError> receiveWithDescriptors(int socket,
                                                                           std::span<uint8_t> buffer);

} // namespace vthost::keeper
