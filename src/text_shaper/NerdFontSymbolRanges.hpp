// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <string_view>

namespace text
{

/// An inclusive range of code points.
struct CodepointRange
{
    char32_t first; ///< The first code point of the range.
    char32_t last;  ///< The last code point of the range, itself included.
};

/// The file name of the Nerd Fonts symbols font, as it ships in the bundled fonts directory.
inline constexpr std::string_view NerdFontSymbolsFileName = "SymbolsNerdFontMono-Regular.ttf";

/// The code points of the bundled Nerd Fonts symbols font that are Nerd Fonts' own: the glyphs it adds in
/// the private use areas, and the few ordinary code points it gives a symbol (power symbols, angle brackets).
/// Ascending and without overlap.
///
/// This is data, not logic: bundling a newer release is a matter of pasting what
/// `scripts/generate-nerd-font-ranges.py SymbolsNerdFontMono-Regular.ttf` prints over the rows below.
/// A test checks every row against the font actually bundled, so a table that claims glyphs the font
/// lacks cannot slip through an upgrade.
inline constexpr auto NerdFontSymbolRanges = std::to_array<CodepointRange>({
    CodepointRange { 0x23FB, 0x23FE },   CodepointRange { 0x2665, 0x2665 }, CodepointRange { 0x26A1, 0x26A1 },
    CodepointRange { 0x276C, 0x2771 },   CodepointRange { 0x2B58, 0x2B58 }, CodepointRange { 0xE000, 0xE00A },
    CodepointRange { 0xE0A0, 0xE0A3 },   CodepointRange { 0xE0B0, 0xE0C8 }, CodepointRange { 0xE0CA, 0xE0CA },
    CodepointRange { 0xE0CC, 0xE0D2 },   CodepointRange { 0xE0D4, 0xE0D4 }, CodepointRange { 0xE0D6, 0xE0D7 },
    CodepointRange { 0xE200, 0xE2A9 },   CodepointRange { 0xE300, 0xE3E3 }, CodepointRange { 0xE5FA, 0xE6BB },
    CodepointRange { 0xE700, 0xE958 },   CodepointRange { 0xEA60, 0xEA88 }, CodepointRange { 0xEA8A, 0xEA8C },
    CodepointRange { 0xEA8F, 0xEAC7 },   CodepointRange { 0xEAC9, 0xEAC9 }, CodepointRange { 0xEACC, 0xEB09 },
    CodepointRange { 0xEB0B, 0xEB4E },   CodepointRange { 0xEB50, 0xEC5E }, CodepointRange { 0xEC60, 0xEC84 },
    CodepointRange { 0xED00, 0xEFCF },   CodepointRange { 0xF000, 0xF385 }, CodepointRange { 0xF400, 0xF533 },
    CodepointRange { 0xF0001, 0xF1AF0 },
});

/// @param codepoint The code point to look up.
/// @return Whether the bundled Nerd Fonts symbols font is meant to draw @p codepoint.
[[nodiscard]] constexpr bool isNerdFontSymbol(char32_t codepoint) noexcept
{
    return std::ranges::any_of(NerdFontSymbolRanges, [codepoint](CodepointRange const& range) {
        return range.first <= codepoint && codepoint <= range.last;
    });
}

} // namespace text
