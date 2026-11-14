// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/state/StateCodec.hpp>

#include <algorithm>
#include <array>

namespace vtbackend
{

namespace
{
    constexpr auto Magic = std::array<uint8_t, 4> { 'C', 'T', 'S', 'T' };
} // namespace

StateWriter::StateWriter(uint32_t formatVersion)
{
    raw(Magic);
    varint(formatVersion);
}

std::expected<StateReader, StateDecodeError> StateReader::open(std::span<uint8_t const> stream)
{
    if (stream.size() < Magic.size())
        return std::unexpected { StateDecodeError::Truncated };
    if (!std::ranges::equal(stream.first(Magic.size()), Magic))
        return std::unexpected { StateDecodeError::MalformedRecord };
    auto reader = StateReader { stream, 0 };
    if (auto const magic = reader.raw(Magic.size()); !magic)
        return std::unexpected { magic.error() };
    auto const version = reader.varint();
    if (!version)
        return std::unexpected { version.error() };
    if (*version < OldestReadableTerminalStateFormat || *version > TerminalStateFormatVersion)
        return std::unexpected { StateDecodeError::UnknownFormatVersion };
    reader._formatVersion = static_cast<uint32_t>(*version);
    return reader;
}

} // namespace vtbackend
