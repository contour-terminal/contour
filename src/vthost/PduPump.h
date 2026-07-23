// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one decode-or-read-more loop every native-protocol endpoint runs.
/// (Lives beside the transports, not in proto/ — the codec stays std-only.)

#include <cstddef>
#include <functional>
#include <vector>

#include <coro/Task.hpp>
#include <vthost/proto/Pdu.h>
#include <net/ISocket.h>
#include <net/Sockets.h>

namespace vthost
{

/// Decodes PDUs off @p socket and hands each to @p handler until the handler
/// returns false, the stream ends, or a protocol error occurs.
/// @param socket The transport to read from (not owned; a pointer, since
///        coroutine reference parameters can dangle).
/// @param handler Consumes one decoded frame; false stops the pump.
inline coro::Task<void> pumpPdus(net::ISocket* socket,
                                 std::function<bool(proto::DecodedFrame const&)> handler)
{
    auto buffer = std::vector<std::byte> {};
    auto consumed = std::size_t { 0 };
    // Compact only when the consumed prefix grows past a threshold, so the
    // common case — many small PDUs — never shifts the buffer. The threshold
    // is low enough that the accumulated "waste" on a connection that keeps
    // streaming but never hits it stays well under the worst-case frame size.
    static constexpr auto CompactThreshold = std::size_t { 65536 };
    while (true)
    {
        auto const decoded = proto::decodePdu(std::span { buffer }.subspan(consumed));
        if (!decoded)
        {
            if (decoded.error() != proto::DecodeError::NeedMoreData
                || !co_await net::appendReadChunk(socket, &buffer))
                co_return;
            continue;
        }
        consumed += decoded->consumed;
        if (consumed >= CompactThreshold)
        {
            buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(consumed));
            consumed = 0;
        }
        if (!handler(*decoded))
            co_return;
    }
}

} // namespace vthost
