// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/TerminalStateCodec.hpp>

#include <vtbackend/state/StateFields.hpp>

#include <new>
#include <utility>

namespace vtbackend
{

std::vector<uint8_t> encodeTerminalState(TerminalState const& state)
{
    // One frame after the header: a stream cut short at a field boundary is still refused as truncated,
    // where its fields alone would decode, the missing ones at their defaults.
    auto writer = StateWriter {};
    auto encoder = detail::FieldEncoder { writer };
    encoder.writeValue(state);
    return std::move(writer).take();
}

std::expected<TerminalState, StateDecodeError> decodeTerminalState(std::span<uint8_t const> stream)
{
    auto reader = StateReader::open(stream);
    if (!reader)
        return std::unexpected { reader.error() };
    auto const payload = reader->bytes();
    if (!payload)
        return std::unexpected { payload.error() };
    if (!reader->atEnd())
        return std::unexpected { StateDecodeError::MalformedRecord };
    try
    {
        auto state = TerminalState {};
        if (auto const decoded = detail::decodeFields(*payload, state); !decoded)
            return std::unexpected { decoded.error() };
        return state;
    }
    catch (std::bad_alloc const&)
    {
        // What the decode budget does not bound -- a single string as long as the stream -- may still not
        // fit.
        return std::unexpected { StateDecodeError::TooLarge };
    }
}

} // namespace vtbackend
