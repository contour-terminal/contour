// SPDX-License-Identifier: Apache-2.0
#include <core/tui/Cell.hpp>

namespace core::tui
{

namespace
{
    // Helper to compare Color variants
    bool colorEquals(Color const& a, Color const& b) noexcept
    {
        if (a.index() != b.index())
            return false;

        if (std::holds_alternative<std::monostate>(a))
            return true;

        if (auto const* idxA = std::get_if<uint8_t>(&a))
            return *idxA == std::get<uint8_t>(b);

        if (auto const* rgbA = std::get_if<RgbColor>(&a))
        {
            auto const& rgbB = std::get<RgbColor>(b);
            return rgbA->r == rgbB.r && rgbA->g == rgbB.g && rgbA->b == rgbB.b;
        }

        return false;
    }

    // Helper to compare Style
    bool styleEquals(Style const& a, Style const& b) noexcept
    {
        return colorEquals(a.fg, b.fg) && colorEquals(a.bg, b.bg) && a.bold == b.bold && a.italic == b.italic
               && a.underline == b.underline && a.strikethrough == b.strikethrough && a.dim == b.dim
               && a.inverse == b.inverse && a.underlineStyle == b.underlineStyle
               && colorEquals(a.underlineColor, b.underlineColor);
    }
} // namespace

bool Cell::operator==(Cell const& other) const noexcept
{
    return grapheme == other.grapheme && width == other.width && styleEquals(style, other.style);
}

} // namespace core::tui
