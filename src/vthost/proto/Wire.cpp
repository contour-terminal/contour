// SPDX-License-Identifier: Apache-2.0
#include <vthost/proto/Wire.h>

namespace vthost::proto
{

void writeFrame(Writer& sink, uint64_t serial, uint64_t ident, std::span<std::byte const> body)
{
    // The tagged length covers serial + ident + body and excludes itself; its
    // low bit is the (never set) compression flag.
    auto header = Writer {};
    header.varint(serial);
    header.varint(ident);

    sink.varint((header.size() + body.size()) << 1);
    sink.bytes(header.view());
    sink.bytes(body);
}

std::expected<Frame, DecodeError> readFrame(std::span<std::byte const> data)
{
    auto reader = Reader { data };
    auto const taggedLength = reader.varint();
    if (!taggedLength)
        return std::unexpected(taggedLength.error());
    if ((*taggedLength & 1) != 0)
        return std::unexpected(DecodeError::CompressedFrame);

    auto const payloadLength = *taggedLength >> 1;
    // Reject an over-large declared length BEFORE the NeedMoreData check: a peer
    // that declares a huge payload and then trickles bytes must not make the read
    // loop keep buffering toward it (an unbounded-memory DoS).
    if (payloadLength > MaxFrameSize)
        return std::unexpected(DecodeError::FrameTooLarge);
    if (reader.remaining() < payloadLength)
        return std::unexpected(DecodeError::NeedMoreData);

    auto const beforePayload = reader.consumed();
    auto const serial = reader.varint();
    if (!serial)
        return std::unexpected(serial.error());
    auto const ident = reader.varint();
    if (!ident)
        return std::unexpected(ident.error());

    auto const headerSize = reader.consumed() - beforePayload;
    if (headerSize > payloadLength)
        return std::unexpected(DecodeError::Truncated);

    auto const body = reader.bytes(payloadLength - headerSize);
    if (!body)
        return std::unexpected(body.error());

    return Frame {
        .serial = *serial,
        .ident = *ident,
        .body = *body,
        .consumed = reader.consumed(),
    };
}

} // namespace vthost::proto
