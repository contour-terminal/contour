// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/proto/Codec.hpp>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// How many bytes a receive asks the channel for at least. A frame larger than this grows the buffer to
    /// hold it; the buffer drops back to this once it drains, so that one large module does not pin its size.
    constexpr auto ReceiveChunk = std::size_t { 64 } * 1024;

    /// @return The stream's reason for a channel's failure.
    [[nodiscard]] StreamError streamErrorOf(ChannelError error) noexcept
    {
        switch (error)
        {
            case ChannelError::Closed: return StreamError::Closed;
            case ChannelError::Io: return StreamError::Io;
        }
        return StreamError::Io;
    }

    /// @return The stream's reason for a frame the codec refused; never called with NeedMoreData.
    [[nodiscard]] StreamError streamErrorOf(proto::DecodeError error) noexcept
    {
        return error == proto::DecodeError::OverCap ? StreamError::OverCap : StreamError::Malformed;
    }
} // namespace

PduStream::PduStream(Channel channel,
                     proto::ChannelKind kind,
                     proto::Direction arriving,
                     proto::PduLimits limits):
    _channel { std::move(channel) }, _kind { kind }, _arriving { arriving }, _limits { limits }
{
}

std::expected<void, StreamError> PduStream::send(proto::Pdu const& pdu)
{
    return _channel.send(proto::encodeFrame(pdu)).transform_error([](ChannelError error) {
        return streamErrorOf(error);
    });
}

std::expected<proto::Pdu, StreamError> PduStream::receive()
{
    while (true)
    {
        auto decoded = proto::decodeFrame(std::span { _buffer }.first(_buffered), _arriving, _kind, _limits);
        if (decoded)
        {
            // Moves what follows the frame to the front, and lets a buffer a large frame grew go.
            std::ranges::copy(std::span { _buffer }.subspan(decoded->consumed, _buffered - decoded->consumed),
                              _buffer.begin());
            _buffered -= decoded->consumed;
            if (_buffered == 0 && _buffer.size() > ReceiveChunk)
                _buffer = std::vector<uint8_t> {};
            return std::move(decoded->pdu);
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            return std::unexpected(streamErrorOf(decoded.error()));

        if (_buffer.size() - _buffered < ReceiveChunk)
            _buffer.resize(std::max(_buffered + ReceiveChunk, _buffer.size() * 2));
        auto const received = _channel.receiveSome(std::span { _buffer }.subspan(_buffered));
        if (!received)
            return std::unexpected(streamErrorOf(received.error()));
        _buffered += *received;
    }
}

void PduStream::shutdown() noexcept
{
    _channel.shutdown();
}

} // namespace vtextension::wasm::remote::transport
