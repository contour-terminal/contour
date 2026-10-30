// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/ColorPalette.hpp>

#include <algorithm>
#include <array>
#include <ranges>

namespace contour::config
{

/// The desktop's selected-row colours: the fill and the text read on it.
struct HighlightPair
{
    vtbackend::RGBColor fill {}; ///< The row's ground.
    vtbackend::RGBColor text {}; ///< The ink on it.

    [[nodiscard]] bool operator==(HighlightPair const&) const = default;
};

/// The inks a selected row falls back to, either of which is legible on some side of every fill.
inline constexpr std::array HighlightFallbackInks {
    vtbackend::RGBColor { 0xFF, 0xFF, 0xFF },
    vtbackend::RGBColor { 0x00, 0x00, 0x00 },
};

/// The OS highlight pair as the chrome paints it.
///
/// A pair whose text reads on its fill at 4.5:1 is the OS's own. Otherwise the OS hue stays and the text
/// becomes white or black, whichever reads better on it. Moving the fill instead (OKLCH lightness, hue
/// and chroma kept) is never the smaller change: white reads at 4.5:1 on any fill up to a relative
/// luminance of 0.183 and black from 0.175 up, so one of the two always reads on the fill as it is, and
/// the move is zero. KDE Breeze's #3DAEE9 under white reads about 2.5:1; under black, 8.6:1.
/// @param os The desktop's Highlight and HighlightedText.
/// @return The pair to paint a selected row with.
[[nodiscard]] inline HighlightPair legibleHighlight(HighlightPair const& os)
{
    if (vtbackend::contrastRatio(os.text, os.fill) >= vtbackend::MinimumTextContrast)
        return os;
    auto const ratioOf = [&](vtbackend::RGBColor ink) {
        return vtbackend::contrastRatio(ink, os.fill);
    };
    return { .fill = os.fill, .text = *std::ranges::max_element(HighlightFallbackInks, {}, ratioOf) };
}

} // namespace contour::config
