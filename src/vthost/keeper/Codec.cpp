// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeper/Codec.hpp>

#include <core/codec/Stream.hpp>

#include <array>
#include <limits>
#include <utility>

namespace vthost::keeper
{

namespace
{
    template <typename T>
    [[nodiscard]] std::expected<Message, CodecError> decodeAlternative(std::span<uint8_t const> fields)
    {
        auto value = T {};
        return core::codec::decodeFields(fields, value)
            .transform([&] { return Message { std::move(value) }; })
            .transform_error([](core::codec::DecodeError error) {
                return error == core::codec::DecodeError::Truncated ? CodecError::Truncated
                                                                    : CodecError::Malformed;
            });
    }

    using Decoder = std::expected<Message, CodecError> (*)(std::span<uint8_t const>);

    /// One decoder per alternative of Message, in the order of the wire table.
    template <typename... Alternatives>
    [[nodiscard]] consteval auto decoderTable(std::variant<Alternatives...> const*)
    {
        return std::array<Decoder, sizeof...(Alternatives)> { &decodeAlternative<Alternatives>... };
    }

    constexpr auto Decoders = decoderTable(static_cast<Message const*>(nullptr));
} // namespace

std::vector<uint8_t> encode(Message const& message)
{
    auto writer = core::codec::Writer {};
    writer.varint(message.index() + 1);
    std::visit([&](auto const& alternative) { core::codec::encodeFields(writer, alternative); }, message);
    return std::move(writer).take();
}

std::expected<Message, CodecError> decode(std::span<uint8_t const> body)
{
    auto reader = core::codec::Reader { body };
    auto const tag = reader.varint();
    if (!tag)
        return std::unexpected { tag.error() == core::codec::DecodeError::Truncated ? CodecError::Truncated
                                                                                    : CodecError::Malformed };
    if (*tag == 0 || *tag > Decoders.size())
        return std::unexpected { CodecError::UnknownTag };
    return Decoders[*tag - 1](body.subspan(body.size() - reader.remaining()));
}

std::optional<uint32_t> peekTag(std::span<uint8_t const> body)
{
    auto reader = core::codec::Reader { body };
    auto const tag = reader.varint();
    if (!tag || *tag > std::numeric_limits<uint32_t>::max())
        return std::nullopt;
    return static_cast<uint32_t>(*tag);
}

std::expected<Message, CodecError> decodeFrame(Frame const& frame)
{
    return decode(frame.body).and_then([&](Message message) -> std::expected<Message, CodecError> {
        if (frame.fds.size() > MaxDescriptorsOfMessage[message.index()])
            return std::unexpected { CodecError::TooManyDescriptors };
        return message;
    });
}

} // namespace vthost::keeper
