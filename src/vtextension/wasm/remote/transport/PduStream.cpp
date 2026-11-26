// SPDX-License-Identifier: Apache-2.0
#include <vtextension/wasm/remote/transport/PduStream.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <vtextension/wasm/remote/proto/Codec.hpp>
#include <vtextension/wasm/remote/proto/PduTable.hpp>
#include <vtextension/wasm/remote/proto/Wire.hpp>
#include <vtextension/wasm/remote/transport/HandleGrant.hpp>

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
            case ChannelError::Refused: return StreamError::Malformed;
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

    /// @return Whether the stream of @p kind that receives PDUs travelling @p arriving takes in what a peer
    ///         attaches: only the helper's control stream does, and none on Windows, where nothing travels
    ///         beside a frame.
    [[nodiscard]] constexpr PduStream::Attachments attachmentsOf(
        [[maybe_unused]] proto::ChannelKind kind, [[maybe_unused]] proto::Direction arriving) noexcept
    {
#ifdef _WIN32
        return PduStream::Attachments::Discarded;
#else
        return kind == proto::ChannelKind::Control && arriving == proto::Direction::ToHelper
                   ? PduStream::Attachments::Taken
                   : PduStream::Attachments::Discarded;
#endif
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
    _channel { std::move(channel) },
    _kind { kind },
    _arriving { arriving },
    _limits { limits },
    _attachments { attachmentsOf(kind, arriving) }
{
}

std::expected<std::vector<uint8_t>, StreamError> PduStream::frameOf(proto::Pdu const& pdu) const
{
    // A PDU the peer would refuse is not sent: the stream stays usable, and the caller learns why. The
    // encoder checks every list against its cap; the body's bytes are checked here, against the cap the
    // peer's reader applies under the same limits.
    auto frame = proto::encodeFrame(pdu);
    if (!frame
        || bodySizeOf(*frame) > proto::PduTable[std::to_underlying(proto::kindOf(pdu)) - 1].sizeCap(_limits))
        return std::unexpected(StreamError::OverCap);
    return *std::move(frame);
}

std::expected<void, StreamError> PduStream::flushUnsentTail()
{
    if (_unsentTail.empty())
        return {};
    if (auto const tail = _channel.send(_unsentTail); !tail)
        return std::unexpected(streamErrorOf(tail.error()));
    _unsentTail.clear();
    return {};
}

std::expected<void, StreamError> PduStream::send(proto::Pdu const& pdu)
{
    return sendAttaching(pdu, std::nullopt);
}

std::expected<void, StreamError> PduStream::send(proto::Pdu const& pdu, HandleGrant const& grant)
{
    return sendAttaching(pdu, grant.attached);
}

std::expected<void, StreamError> PduStream::sendAttaching(
    proto::Pdu const& pdu, [[maybe_unused]] std::optional<NativeHandle> attached)
{
    auto const frame = frameOf(pdu);
    if (!frame)
        return std::unexpected(frame.error());
    return flushUnsentTail().and_then([&] {
#ifndef _WIN32
        if (attached)
            return _channel.sendWithHandle(*frame, *attached).transform_error([](ChannelError error) {
                return streamErrorOf(error);
            });
#endif
        return _channel.send(*frame).transform_error([](ChannelError error) { return streamErrorOf(error); });
    });
}

OwnedHandle PduStream::takeAttachedHandle() noexcept
{
    // One that came with a frame not received yet is that frame's.
    if (_attachedTo != AttachedTo::Received)
        return OwnedHandle {};
    _attachedTo = AttachedTo::Nothing;
    return std::move(_attached);
}

std::expected<SendOutcome, StreamError> PduStream::trySend(proto::Pdu const& pdu)
{
    auto const frame = frameOf(pdu);
    if (!frame)
        return std::unexpected(frame.error());
    if (!_unsentTail.empty())
    {
        auto const sent = _channel.trySend(_unsentTail);
        if (!sent)
            return std::unexpected(streamErrorOf(sent.error()));
        _unsentTail.erase(_unsentTail.begin(), _unsentTail.begin() + static_cast<std::ptrdiff_t>(*sent));
        if (!_unsentTail.empty())
            return SendOutcome::WouldBlock;
    }
    auto const sent = _channel.trySend(*frame);
    if (!sent)
        return std::unexpected(streamErrorOf(sent.error()));
    if (*sent == 0)
        return SendOutcome::WouldBlock;
    // Begun, the frame must be finished, or the peer would read the next one's bytes as its rest.
    _unsentTail.assign(frame->begin() + static_cast<std::ptrdiff_t>(*sent), frame->end());
    return SendOutcome::Sent;
}

std::expected<proto::Pdu, StreamError> PduStream::receive()
{
    while (true)
    {
        auto const buffered = std::span<uint8_t const> { _buffer.get(), _buffered };
        auto decoded = proto::decodeFrame(buffered, _arriving, _kind, _limits);
        if (decoded)
        {
            // A handle held for a frame further on is that much nearer the front; one held for this frame is
            // this PDU's.
            if (_attachedTo == AttachedTo::Buffered)
            {
                if (_attachedAt == 0)
                    _attachedTo = AttachedTo::Received;
                else
                    _attachedAt -= decoded->consumed;
            }
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
        auto const received = receiveMore();
        if (!received)
            return std::unexpected(received.error());
        _buffered += *received;
    }
}

std::expected<std::size_t, StreamError> PduStream::receiveMore()
{
    auto const space = std::span { _buffer.get(), _capacity }.subspan(_buffered);
    if (_attachments == Attachments::Discarded)
        return _channel.receiveSome(space).transform_error(
            [](ChannelError error) { return streamErrorOf(error); });
#ifdef _WIN32
    // No stream takes handles in on Windows (attachmentsOf()).
    std::unreachable();
#else
    auto arrived = std::vector<OwnedHandle> {};
    auto const received = _channel.receiveSomeWithHandles(space, arrived);
    if (!received)
        return std::unexpected(streamErrorOf(received.error()));
    // At most one waits to be claimed: a second is a peer that broke the protocol, and is closed with it.
    if (arrived.size() > 1 || (!arrived.empty() && _attached))
        return std::unexpected(StreamError::Malformed);
    if (!arrived.empty())
    {
        _attached = std::move(arrived.front());
        _attachedTo = AttachedTo::Buffered;
        _attachedAt = attachedFrameStart(_buffered, _buffered + *received);
    }
    return *received;
#endif
}

std::size_t PduStream::attachedFrameStart([[maybe_unused]] std::size_t chunkStart,
                                          std::size_t chunkEnd) const noexcept
{
    // The frames the buffer holds, walked from its front: a frame's header tells where the next one starts.
    // Linux ends a receive with the message that carried descriptors, but may begin it with bytes sent before
    // that message: the handle came with the last frame that starts in the buffer. The BSDs end a receive
    // before a message that carries descriptors once bytes were read: there it came with the first frame that
    // starts in what arrived.
    auto const bytes = std::span<uint8_t const> { _buffer.get(), chunkEnd };
    auto start = std::size_t { 0 };
    while (true)
    {
#ifndef __linux__
        if (start >= chunkStart)
            return start;
#endif
        auto const size = proto::frameSizeOf(bytes.subspan(start), _arriving, _kind, _limits);
        if (!size || start + *size >= chunkEnd)
            return start;
        start += *size;
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
