// SPDX-License-Identifier: Apache-2.0
#include <contour/config/GpuSelector.hpp>

#include <core/Utils.hpp>

#include <algorithm>
#include <array>
#include <charconv>

namespace contour::config
{

namespace
{
    struct Keyword
    {
        std::string_view text;
        GpuPreference preference;
    };

    // One row per keyword; the first row of a preference is its canonical spelling.
    constexpr auto Keywords = std::array {
        Keyword { .text = "auto", .preference = GpuPreference::Auto },
        Keyword { .text = "integrated", .preference = GpuPreference::Integrated },
        Keyword { .text = "discrete", .preference = GpuPreference::Discrete },
    };

    [[nodiscard]] constexpr char toLower(char c) noexcept
    {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }

    [[nodiscard]] bool equalsIgnoringCase(std::string_view a, std::string_view b) noexcept
    {
        return std::ranges::equal(a, b, {}, toLower, toLower);
    }

    [[nodiscard]] std::optional<std::uint16_t> parseHex16(std::string_view text) noexcept
    {
        if (text.empty() || text.size() > 4)
            return std::nullopt;
        auto value = std::uint16_t {};
        auto const* const end = text.data() + text.size();
        auto const [last, error] = std::from_chars(text.data(), end, value, 16);
        if (error != std::errc {} || last != end)
            return std::nullopt;
        return value;
    }
} // namespace

std::optional<PciId> parsePciId(std::string_view text) noexcept
{
    auto const colon = text.find(':');
    if (colon == std::string_view::npos)
        return std::nullopt;
    auto const vendor = parseHex16(text.substr(0, colon));
    auto const device = parseHex16(text.substr(colon + 1));
    if (!vendor || !device)
        return std::nullopt;
    return PciId { .vendor = *vendor, .device = *device };
}

std::expected<GpuSelector, GpuSelectorError> parseGpuSelector(std::string_view text)
{
    auto const value = core::trim(text);
    if (value.empty())
        return GpuSelector {};
    for (auto const& keyword: Keywords)
        if (equalsIgnoringCase(value, keyword.text))
            return GpuSelector { .preference = keyword.preference, .id = std::nullopt };
    if (auto const id = parsePciId(value))
        return GpuSelector { .preference = GpuPreference::Specific, .id = id };
    return std::unexpected(GpuSelectorError::Malformed);
}

std::string_view keywordOf(GpuPreference preference) noexcept
{
    auto const row = std::ranges::find(Keywords, preference, &Keyword::preference);
    return row != Keywords.end() ? row->text : std::string_view {};
}

} // namespace contour::config
