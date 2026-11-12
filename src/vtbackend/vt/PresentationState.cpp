// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/vt/PresentationState.hpp>

#include <algorithm>
#include <ranges>

namespace vtbackend
{

namespace
{
    /// The largest value a numeric field may take: far beyond any page, and far below an overflow.
    constexpr auto MaxFieldValue = 65535;

    /// The character sets Contour has a map for, each with whether it is a 96-character set.
    constexpr auto KnownCharsets = std::array {
        std::pair { CharsetId::Special, false },   std::pair { CharsetId::British, false },
        std::pair { CharsetId::Dutch, false },     std::pair { CharsetId::Finnish, false },
        std::pair { CharsetId::French, false },    std::pair { CharsetId::FrenchCanadian, false },
        std::pair { CharsetId::German, false },    std::pair { CharsetId::NorwegianDanish, false },
        std::pair { CharsetId::Spanish, false },   std::pair { CharsetId::Swedish, false },
        std::pair { CharsetId::Swiss, false },     std::pair { CharsetId::USASCII, false },
        std::pair { CharsetId::Technical, false }, std::pair { CharsetId::ISOLatin1Supplemental, true },
    };

    /// Reads a decimal field.
    /// @param text The field, without its separator.
    /// @return Its value, or MalformedNumber when it is empty, holds a non-digit or exceeds MaxFieldValue.
    std::expected<int, PresentationStateRejection> decodeNumber(std::string_view text)
    {
        if (text.empty())
            return std::unexpected { PresentationStateRejection::MalformedNumber };
        auto value = 0;
        for (auto const ch: text)
        {
            if (ch < '0' || ch > '9')
                return std::unexpected { PresentationStateRejection::MalformedNumber };
            value = (value * 10) + (ch - '0');
            if (value > MaxFieldValue)
                return std::unexpected { PresentationStateRejection::MalformedNumber };
        }
        return value;
    }

    /// Reads a one-character bit field: a single character whose bits above @p base are all within @p mask.
    /// @param text The field, without its separator.
    /// @param mask The bits the field may carry above its 0x40 base.
    /// @return The bits, or MalformedFlags.
    std::expected<uint8_t, PresentationStateRejection> decodeFlags(std::string_view text, uint8_t mask)
    {
        constexpr auto Base = uint8_t { 0x40 };
        if (text.size() != 1)
            return std::unexpected { PresentationStateRejection::MalformedFlags };
        auto const ch = static_cast<uint8_t>(text.front());
        if ((ch & ~(Base | mask)) != 0 || (ch & Base) == 0)
            return std::unexpected { PresentationStateRejection::MalformedFlags };
        return static_cast<uint8_t>(ch & mask);
    }

    /// Reads a G-set number, 0 to 3.
    std::expected<CharsetTable, PresentationStateRejection> decodeCharsetTable(std::string_view text)
    {
        return decodeNumber(text).and_then(
            [](int value) -> std::expected<CharsetTable, PresentationStateRejection> {
                if (value > 3)
                    return std::unexpected { PresentationStateRejection::MalformedNumber };
                return static_cast<CharsetTable>(value);
            });
    }

    /// Splits the four SCS designators of Sdesig apart: each is any intermediates (0x20..0x2F) and then
    /// one final byte (0x30..0x7E).
    std::expected<std::array<std::string, 4>, PresentationStateRejection> decodeDesignations(
        std::string_view text)
    {
        auto designations = std::array<std::string, 4> {};
        for (auto& designation: designations)
        {
            auto const final =
                std::ranges::find_if_not(text, [](char ch) { return ch >= 0x20 && ch <= 0x2F; });
            if (final == text.end() || *final < 0x30 || *final > 0x7E)
                return std::unexpected { PresentationStateRejection::MalformedCharset };
            auto const length = static_cast<size_t>(std::ranges::distance(text.begin(), final)) + 1;
            designation = std::string { text.substr(0, length) };
            text.remove_prefix(length);
        }
        if (!text.empty())
            return std::unexpected { PresentationStateRejection::MalformedCharset };
        return designations;
    }
} // namespace

std::string_view toString(PresentationStateRejection rejection) noexcept
{
    switch (rejection)
    {
        case PresentationStateRejection::MissingField: return "missing field";
        case PresentationStateRejection::MalformedNumber: return "malformed number";
        case PresentationStateRejection::MalformedFlags: return "malformed flags";
        case PresentationStateRejection::MalformedCharset: return "malformed character set designator";
    }
    return "unknown";
}

std::expected<CursorInformation, PresentationStateRejection> decodeCursorInformation(std::string_view payload)
{
    // The nine fields before Sdesig end in ';'. Sdesig is the rest, its designators run together.
    constexpr auto SeparatedFieldCount = 9;
    auto fields = std::array<std::string_view, SeparatedFieldCount> {};
    for (auto& field: fields)
    {
        auto const separator = payload.find(';');
        if (separator == std::string_view::npos)
            return std::unexpected { PresentationStateRejection::MissingField };
        field = payload.substr(0, separator);
        payload.remove_prefix(separator + 1);
    }

    auto info = CursorInformation {};
    auto const assign = [](auto& target) {
        return [&target](auto value) {
            target = std::move(value);
        };
    };
    auto const result =
        decodeNumber(fields[0])
            .transform(assign(info.line))
            .and_then([&] { return decodeNumber(fields[1]); })
            .transform(assign(info.column))
            .and_then([&] { return decodeNumber(fields[2]); })
            .transform(assign(info.page))
            // Srend may carry the VT520's invisible bit (0x10), which is accepted and not restored.
            .and_then([&] { return decodeFlags(fields[3], 0x1F); })
            .transform(assign(info.rendition))
            .and_then([&] { return decodeFlags(fields[4], 0x01); })
            .transform([&](uint8_t bits) { info.isProtected = bits != 0; })
            .and_then([&] { return decodeFlags(fields[5], 0x0F); })
            .transform(assign(info.flags))
            .and_then([&] { return decodeCharsetTable(fields[6]); })
            .transform(assign(info.gl))
            .and_then([&] { return decodeCharsetTable(fields[7]); })
            .transform(assign(info.gr))
            .and_then([&] { return decodeFlags(fields[8], 0x0F); })
            .transform(assign(info.charsetSizes))
            .and_then([&] { return decodeDesignations(payload); })
            .transform(assign(info.designations));
    if (!result)
        return std::unexpected { result.error() };
    return info;
}

std::expected<std::vector<ColumnOffset>, PresentationStateRejection> decodeTabStops(std::string_view payload)
{
    auto stops = std::vector<ColumnOffset> {};
    if (payload.empty())
        return stops;
    for (auto const field: payload | std::views::split('/'))
    {
        auto const column = decodeNumber(std::string_view { field.begin(), field.end() });
        if (!column || *column == 0)
            return std::unexpected { PresentationStateRejection::MalformedNumber };
        stops.emplace_back(*column - 1);
    }
    std::ranges::sort(stops);
    auto const duplicates = std::ranges::unique(stops);
    stops.erase(duplicates.begin(), duplicates.end());
    return stops;
}

std::optional<CharsetId> charsetOfDesignator(std::string_view designator, bool is96) noexcept
{
    if (designator.size() != 1)
        return std::nullopt;
    auto const known = std::ranges::find_if(KnownCharsets, [&](auto const& entry) {
        return entry.second == is96 && charsetDesignation(entry.first) == designator.front();
    });
    if (known == KnownCharsets.end())
        return std::nullopt;
    return known->first;
}

} // namespace vtbackend
