// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/BlockPill.hpp>

#include <vtbackend/core/ColorPalette.hpp>

#include <cmath>
#include <ranges>

namespace vtbackend
{

namespace
{
    /// @p percent of full opacity, as an alpha byte.
    [[nodiscard]] uint8_t alphaOf(int percent) noexcept
    {
        return static_cast<uint8_t>(std::lround(percent * 255.0 / 100.0));
    }

    /// @p translucent over @p ground, as the compositor draws it.
    [[nodiscard]] RGBColor over(RGBColor ground, RGBAColor translucent) noexcept
    {
        return mixColor(ground, translucent.rgb(), static_cast<float>(translucent.alpha()) / 255.0F);
    }

    /// The colour @p source names on @p palette for @p row.
    [[nodiscard]] RGBColor colorOf(ColorPalette const& palette, PillInkSource source, PillStyleRow const& row)
    {
        switch (source)
        {
            case PillInkSource::Label: return palette.defaultForeground;
            case PillInkSource::Status: return palette.blockStatusColor(row.outcome);
        }
        return palette.defaultForeground;
    }
} // namespace

PillInks pillInks(ColorPalette const& palette, PillStyle style, std::optional<RGBAColor> underlay) noexcept
{
    auto const& row = pillStyleRow(style);
    auto const page = underlay ? over(palette.defaultBackground, *underlay) : palette.defaultBackground;

    auto inks = PillInks {};
    if (row.fillPercent > 0)
        inks.fill = RGBAColor(colorOf(palette, row.fill, row), alphaOf(row.fillPercent));
    if (row.ringPercent > 0)
        inks.ring = RGBAColor(palette.defaultForeground, alphaOf(row.ringPercent));
    inks.ground = inks.fill ? over(page, *inks.fill) : page;
    inks.wanted = over(inks.ground, RGBAColor(colorOf(palette, row.ink, row), alphaOf(row.inkPercent)));
    inks.ink =
        liftToContrast(inks.wanted, legibleInkOn(palette, inks.ground), inks.ground, MinimumTextContrast);
    return inks;
}

bool hoverTintKeepsContrast(ColorPalette const& palette, int percent) noexcept
{
    auto const page = palette.defaultBackground;
    auto const tinted = over(page, RGBAColor(palette.defaultForeground, alphaOf(percent)));
    auto const keeps = [&](RGBColor text) {
        auto const before = contrastRatio(text, page);
        auto const after = contrastRatio(text, tinted);
        if (before >= MinimumTextContrast)
            return after >= MinimumTextContrast;
        return before < MinimumMarkContrast || after >= MinimumMarkContrast;
    };
    return keeps(palette.defaultForeground)
           && std::ranges::all_of(std::views::iota(std::size_t { 0 }, std::size_t { 16 }),
                                  [&](std::size_t index) { return keeps(palette.palette.at(index)); });
}

BlockHoverInks blockHoverInks(ColorPalette const& palette) noexcept
{
    auto percent = BlockHoverTintPercent;
    while (percent > 0 && !hoverTintKeepsContrast(palette, percent))
        --percent;
    return BlockHoverInks { .tint = RGBAColor(palette.defaultForeground, alphaOf(percent)),
                            .ring = RGBAColor(palette.defaultForeground, alphaOf(BlockHoverRingPercent)) };
}

} // namespace vtbackend
