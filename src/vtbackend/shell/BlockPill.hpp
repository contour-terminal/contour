// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How a native row's block chips are inked, as the canvas's `blocks` board draws them: one row per
/// PillStyle, each ink legible over its pill over the page on every scheme.

#include <vtbackend/core/BlockPill.hpp>
#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>

#include <array>
#include <cstdint>
#include <optional>

namespace vtbackend
{

struct ColorPalette;
struct CommandBlockRecord;

/// Where a pill's ink, or its fill, takes its colour from.
enum class PillInkSource : uint8_t
{
    Label =
        0,  ///< The scheme's foreground: the canvas's `label`, dark on a light scheme, light on a dark one.
    Status, ///< The row's outcome colour: status.danger for a failure, the accent for a running command.
};

/// Which edge of its placed columns a pill keeps to.
enum class PillAnchor : uint8_t
{
    Right = 0, ///< A status chip: right-aligned, as placeBlockChip() right-aligns its columns.
    Left,      ///< The fold chip: right after the command it folds.
};

/// One style's inks, as percentages of their source over the page (the canvas's values).
struct PillStyleRow
{
    PillStyle style = PillStyle::Success;                       ///< The style the row describes.
    CommandBlockOutcome outcome = CommandBlockOutcome::Success; ///< The outcome a Status source reads.
    PillInkSource ink = PillInkSource::Label;                   ///< The label's colour.
    int inkPercent = 100;                                       ///< The label's opacity.
    PillInkSource fill = PillInkSource::Label;                  ///< The fill's colour.
    int fillPercent = 0;                                        ///< The fill's opacity; 0 draws no pill.
    int ringPercent = 0;                   ///< The hairline ring's opacity, of the label; 0 draws none.
    PillAnchor anchor = PillAnchor::Right; ///< Where it sits in its columns.
};

/// The canvas's pills, by PillStyle.
inline constexpr auto PillStyleRows = std::array {
    PillStyleRow { .style = PillStyle::Success,
                   .outcome = CommandBlockOutcome::Success,
                   .ink = PillInkSource::Label,
                   .inkPercent = 62,
                   .fill = PillInkSource::Label,
                   .fillPercent = 0,
                   .ringPercent = 0,
                   .anchor = PillAnchor::Right },
    PillStyleRow { .style = PillStyle::Failed,
                   .outcome = CommandBlockOutcome::Failure,
                   .ink = PillInkSource::Status,
                   .inkPercent = 100,
                   .fill = PillInkSource::Status,
                   .fillPercent = 16,
                   .ringPercent = 0,
                   .anchor = PillAnchor::Right },
    PillStyleRow { .style = PillStyle::Running,
                   .outcome = CommandBlockOutcome::Running,
                   .ink = PillInkSource::Status,
                   .inkPercent = 100,
                   .fill = PillInkSource::Status,
                   .fillPercent = 0,
                   .ringPercent = 0,
                   .anchor = PillAnchor::Right },
    PillStyleRow { .style = PillStyle::Fold,
                   .outcome = CommandBlockOutcome::Success,
                   .ink = PillInkSource::Label,
                   .inkPercent = 70,
                   .fill = PillInkSource::Label,
                   .fillPercent = 8,
                   .ringPercent = 12,
                   .anchor = PillAnchor::Left },
};
static_assert(PillStyleRows.size() == PillStyleCount);

/// The row of @p style.
/// @param style The style.
/// @return Its row in PillStyleRows.
[[nodiscard]] constexpr PillStyleRow const& pillStyleRow(PillStyle style) noexcept
{
    return PillStyleRows[static_cast<std::size_t>(style)];
}

/// The style a status chip of @p outcome is drawn in.
/// @param outcome How the block reads.
/// @return Success, Failed or Running.
[[nodiscard]] constexpr PillStyle pillStyleOf(CommandBlockOutcome outcome) noexcept
{
    switch (outcome)
    {
        case CommandBlockOutcome::Success: return PillStyle::Success;
        case CommandBlockOutcome::Failure: return PillStyle::Failed;
        case CommandBlockOutcome::Running: return PillStyle::Running;
    }
    return PillStyle::Success;
}

/// A pill's inks, resolved against a scheme.
struct PillInks
{
    RGBColor ink {};                  ///< The label, opaque: its opacity already mixed into its ground.
    std::optional<RGBAColor> fill {}; ///< The pill's fill; none for a pill of text alone.
    std::optional<RGBAColor> ring {}; ///< The hairline ring; none for none.
    RGBColor ground {};               ///< What the label stands on: the fill over the page, or the page.
    /// The label before it was lifted to MinimumTextContrast: where the two differ, the canvas's value was
    /// too faint for the scheme.
    RGBColor wanted {};

    bool operator==(PillInks const&) const = default;
};

/// The inks of a pill of @p style on @p palette: the row's colours at its opacities, the label lifted toward
/// the page's legible ink where it would read below MinimumTextContrast over its ground.
/// @param palette The colour scheme.
/// @param style The pill's style.
/// @param underlay What lies between the page and the pill -- a hovered block's tint -- or nullopt.
/// @return The inks.
[[nodiscard]] PillInks pillInks(ColorPalette const& palette,
                                PillStyle style,
                                std::optional<RGBAColor> underlay = std::nullopt) noexcept;

/// The canvas's tint of a hovered block, of the label: at most this, less where it would cost a scheme's text
/// its contrast (blockHoverInks()).
inline constexpr int BlockHoverTintPercent = 5;

/// The canvas's ring around a hovered block, of the label.
inline constexpr int BlockHoverRingPercent = 6;

/// A hovered block's inks.
struct BlockHoverInks
{
    RGBAColor tint {}; ///< Laid over the block's rows, under their text.
    RGBAColor ring {}; ///< The one-pixel ring inside the block's outline.

    bool operator==(BlockHoverInks const&) const = default;
};

/// Whether a tint of the label at @p percent leaves @p palette's text reading: its foreground and its 16 ANSI
/// colours, none that reached MinimumTextContrast against the page falling below it once tinted, and none
/// that reached MinimumMarkContrast (but not the text bar) falling below that.
/// @param palette The colour scheme.
/// @param percent The tint's opacity.
/// @return Whether it does.
[[nodiscard]] bool hoverTintKeepsContrast(ColorPalette const& palette, int percent) noexcept;

/// The inks of a hovered block on @p palette: the label at BlockHoverTintPercent, or as much less as
/// hoverTintKeepsContrast() needs, and the ring at BlockHoverRingPercent.
/// @param palette The colour scheme.
/// @return The inks.
[[nodiscard]] BlockHoverInks blockHoverInks(ColorPalette const& palette) noexcept;

} // namespace vtbackend
