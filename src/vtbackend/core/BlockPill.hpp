// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>

namespace vtbackend
{

/// What a block chip on a native row says, which decides its weight, its inks and its pill.
///
/// In core/ rather than shell/ because the render buffer that carries a chip, the terminal that measures
/// it and the renderer that draws it all name it, and render/ may include nothing from shell/.
enum class PillStyle : uint8_t
{
    Success = 0, ///< A success's quiet duration, "4ms": text only.
    Failed,      ///< "Failed · exit 8 · 12s", on a band of the danger colour.
    Running,     ///< "Running · 8s", in the accent colour.
    Fold,        ///< A collapsed fold's "80 lines hidden ⌄", on a band with a hairline ring.
};

/// How many pill styles there are: the size of a table indexed by PillStyle.
inline constexpr std::size_t PillStyleCount = static_cast<std::size_t>(PillStyle::Fold) + 1;

/// The first and last codepoints of printable ASCII, which a pill's label is measured by.
inline constexpr char32_t FirstMeasuredAscii = 0x20;
inline constexpr char32_t LastMeasuredAscii = 0x7E;

/// The codepoints a label is measured by besides printable ASCII: what blockStatusText() puts between its
/// parts, and the chevron collapsedFoldLabel() ends with.
inline constexpr auto PillMeasuredExtras = std::array { U'·', U'⌄' };

/// How many codepoints a pill's face is measured at.
inline constexpr std::size_t PillMeasuredCount =
    static_cast<std::size_t>(LastMeasuredAscii - FirstMeasuredAscii) + 1 + PillMeasuredExtras.size();

/// The codepoint at @p index of the measured set: printable ASCII in order, then PillMeasuredExtras.
/// @param index Below PillMeasuredCount.
/// @return The codepoint.
[[nodiscard]] constexpr char32_t pillMeasuredCodepoint(std::size_t index) noexcept
{
    auto constexpr AsciiCount = static_cast<std::size_t>(LastMeasuredAscii - FirstMeasuredAscii) + 1;
    return index < AsciiCount ? FirstMeasuredAscii + static_cast<char32_t>(index)
                              : PillMeasuredExtras[index - AsciiCount];
}

/// Where @p codepoint is in the measured set.
/// @param codepoint Any codepoint.
/// @return Its index; nullopt for one the set does not hold.
[[nodiscard]] constexpr std::optional<std::size_t> pillMeasuredIndex(char32_t codepoint) noexcept
{
    auto constexpr AsciiCount = static_cast<std::size_t>(LastMeasuredAscii - FirstMeasuredAscii) + 1;
    if (codepoint >= FirstMeasuredAscii && codepoint <= LastMeasuredAscii)
        return static_cast<std::size_t>(codepoint - FirstMeasuredAscii);
    for (auto const offset: std::views::iota(std::size_t { 0 }, PillMeasuredExtras.size()))
        if (PillMeasuredExtras[offset] == codepoint)
            return AsciiCount + offset;
    return std::nullopt;
}

/// How wide a pill's label is in the UI font, per style, in device pixels: the advances the renderer reads
/// from the face once per font change, so the terminal can place a pill over whole columns without shaping
/// it under its lock.
///
/// A sum of advances rather than the shaped run: the renderer shapes the label as it draws it, and kerning
/// only ever narrows a UI font's run, so the pill drawn is never wider than the columns measured for it --
/// and where it would be, the renderer clips it to them.
struct PillLabelMetrics
{
    /// One style's face.
    struct Face
    {
        std::array<int, PillMeasuredCount> advances {}; ///< Indexed by pillMeasuredIndex().
        int widest = 0;  ///< The widest advance: what a codepoint the set does not hold is counted as.
        int padding = 0; ///< The pill's padding, both sides together.

        bool operator==(Face const&) const = default;
    };

    int cellWidth = 0;                          ///< A grid cell's width; 0 until the renderer measured.
    std::array<Face, PillStyleCount> styles {}; ///< Indexed by PillStyle.

    /// Whether the renderer has measured the faces.
    [[nodiscard]] constexpr bool measured() const noexcept { return cellWidth > 0; }

    /// The whole columns a pill of @p style saying @p text takes: its label and padding, rounded up.
    /// @param style The pill's style.
    /// @param text Its label.
    /// @return The columns, at least one; nullopt until measured().
    [[nodiscard]] constexpr std::optional<ColumnCount> columnsOf(PillStyle style,
                                                                 std::u32string_view text) const noexcept
    {
        if (!measured())
            return std::nullopt;
        auto const& face = styles[static_cast<std::size_t>(style)];
        auto width = face.padding;
        for (auto const codepoint: text)
        {
            auto const index = pillMeasuredIndex(codepoint);
            width += index ? face.advances[*index] : face.widest;
        }
        return ColumnCount(std::max(1, (width + cellWidth - 1) / cellWidth));
    }

    bool operator==(PillLabelMetrics const&) const = default;
};

} // namespace vtbackend
