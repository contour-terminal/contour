// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/KittyClipboard.h>

#include <crispy/base64.h>

#include <algorithm>
#include <array>

namespace vtbackend::kitty_clipboard
{

namespace
{
    /// Applies one `key=value` pair to @p packet.
    [[nodiscard]] std::expected<void, Error> applyKey(Packet& packet,
                                                      std::string_view key,
                                                      std::string_view value)
    {
        if (key == "type")
        {
            if (value == "read")
                packet.type = PacketType::Read;
            else if (value == "write")
                packet.type = PacketType::Write;
            else if (value == "walias")
                packet.type = PacketType::WriteAlias;
            else if (value == "wdata")
                packet.type = PacketType::WriteData;
            else
                return std::unexpected(Error::UnknownType);
        }
        else if (key == "loc")
            packet.location = value == "primary" ? Location::PrimarySelection : Location::Clipboard;
        else if (key == "mime")
            packet.mimeType = value;
        else if (key == "id")
            packet.id = value;
        // Unknown keys -- `name`, `pw`, and whatever the protocol grows next -- are ignored rather
        // than rejected, so that a packet carrying one still does the part we understand.

        return {};
    }
} // namespace

bool isSupportedMimeType(std::string_view mimeType) noexcept
{
    // An empty type means "unspecified", which for a text clipboard is text.
    if (mimeType.empty())
        return true;

    static constexpr auto Supported = std::array {
        std::string_view { "text/plain" },  std::string_view { "text/plain;charset=utf-8" },
        std::string_view { "UTF8_STRING" }, std::string_view { "STRING" },
        std::string_view { "TEXT" },
    };
    return std::ranges::find(Supported, mimeType) != Supported.end();
}

std::expected<Packet, Error> parsePacket(std::string_view payload)
{
    auto packet = Packet {};

    // Metadata is separated from the payload by the FIRST semicolon; base64 never contains one, but
    // splitting on every semicolon would still be wrong.
    auto const separator = payload.find(';');
    auto const metadata = payload.substr(0, separator);
    if (separator != std::string_view::npos)
        packet.payload = payload.substr(separator + 1);

    // Metadata pairs are colon-separated, as in the text sizing protocol -- not semicolon-separated
    // like OSC 52's parameters.
    for (size_t offset = 0; offset < metadata.size();)
    {
        auto end = metadata.find(':', offset);
        if (end == std::string_view::npos)
            end = metadata.size();

        auto const record = metadata.substr(offset, end - offset);
        offset = end + 1;

        if (record.empty())
            continue;

        auto const equals = record.find('=');
        if (equals == std::string_view::npos)
            return std::unexpected(Error::MalformedMetadata);

        if (auto const result = applyKey(packet, record.substr(0, equals), record.substr(equals + 1));
            !result)
            return std::unexpected(result.error());
    }

    return packet;
}

} // namespace vtbackend::kitty_clipboard
