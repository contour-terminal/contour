// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one decode-or-read-more loop every native-protocol endpoint runs.
/// (Lives beside the transports, not in proto/ — the codec stays std-only.)

#include <cstddef>
#include <functional>
#include <vector>

#include <coro/Task.hpp>
#include <muxserver/proto/Pdu.h>
#include <net/ISocket.h>
#include <net/Sockets.h>

namespace muxserver
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
    while (true)
    {
        auto const decoded = proto::decodePdu(buffer);
        if (!decoded)
        {
            if (decoded.error() != proto::DecodeError::NeedMoreData
                || !co_await net::appendReadChunk(socket, &buffer))
                co_return;
            continue;
        }
        buffer.erase(buffer.begin(), buffer.begin() + static_cast<long>(decoded->consumed));
        if (!handler(*decoded))
            co_return;
    }
}

} // namespace muxserver
