// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <utility>

#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/PduTable.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>

namespace vtextension::wasm::remote::transport
{

namespace
{
    /// How many bytes a receive asks the channel for at least: the receive buffer's least size, which holds
    /// many small frames at once.
    constexpr auto ReceiveChunk = std::size_t { 64 } * 1024;

    /// The largest receive buffer kept once it drains. One a larger frame grew is let go, so that one large
    /// module does not pin its size; the frames of a busy instance's memory reads, up to this, reuse it.
    constexpr auto RetainedBufferBytes = (std::size_t { 1024 } * 1024) + ReceiveChunk;

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

    /// @return How many bytes of @p frame, one encodeFrame() made, its body spans: its length less the kind's
    ///         one byte.
    [[nodiscard]] uint64_t bodySizeOf(std::span<uint8_t const> frame) noexcept
    {
        auto header = proto::Reader { frame };
        return header.varint64().value_or(1) - 1;
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
    // A PDU the peer would refuse is not sent: the stream stays usable, and the caller learns why. The
    // encoder checks every list against its cap; the body's bytes are checked here, against the cap the
    // peer's reader applies under the same limits.
    auto const frame = proto::encodeFrame(pdu);
    if (!frame
        || bodySizeOf(*frame) > proto::PduTable[std::to_underlying(proto::kindOf(pdu)) - 1].sizeCap(_limits))
        return std::unexpected(StreamError::OverCap);
    return _channel.send(*frame).transform_error([](ChannelError error) { return streamErrorOf(error); });
}

std::expected<proto::Pdu, StreamError> PduStream::receive()
{
    while (true)
    {
        auto const buffered = std::span<uint8_t const> { _buffer.get(), _buffered };
        auto decoded = proto::decodeFrame(buffered, _arriving, _kind, _limits);
        if (decoded)
        {
            // Moves what follows the frame to the front, and lets a buffer a large frame grew go.
            std::ranges::copy(buffered.subspan(decoded->consumed), _buffer.get());
            _buffered -= decoded->consumed;
            if (_buffered == 0 && _capacity > RetainedBufferBytes)
                reallocate(0);
            return std::move(decoded->pdu);
        }
        if (decoded.error() != proto::DecodeError::NeedMoreData)
            return std::unexpected(streamErrorOf(decoded.error()));

        // Grown once to hold the whole frame as soon as its header tells its size, never past it: a frame's
        // bytes arrive straight where it is decoded from, with no doubling and nothing zero-filled.
        auto const frameBytes =
            proto::frameSizeOf(buffered, _arriving, _kind, _limits).value_or(_buffered + 1);
        if (_capacity < frameBytes || _capacity == _buffered)
            reallocate(std::max({ frameBytes, _buffered + 1, ReceiveChunk }));
        auto const received = _channel.receiveSome(std::span { _buffer.get(), _capacity }.subspan(_buffered));
        if (!received)
            return std::unexpected(streamErrorOf(received.error()));
        _buffered += *received;
    }
}

void PduStream::reallocate(std::size_t capacity)
{
    // Default-initialized: only what is received is written, and the bytes buffered are copied over.
    auto buffer =
        capacity ? std::make_unique_for_overwrite<uint8_t[]>(capacity) : std::unique_ptr<uint8_t[]> {};
    std::ranges::copy(std::span { _buffer.get(), _buffered }, buffer.get());
    _buffer = std::move(buffer);
    _capacity = capacity;
    if (capacity)
    {
        ++_stats.allocations;
        _stats.peakBytes = std::max(_stats.peakBytes, capacity);
    }
}

void PduStream::shutdown() noexcept
{
    _channel.shutdown();
}

} // namespace vtextension::wasm::remote::transport
