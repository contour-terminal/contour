// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The words a command block's status is told in -- the chip's, the block menu header's, VoiceOver's -- and
/// the chip's inks. One duration format everywhere: formatCommandDuration()'s (ruling F1).

#include <vtbackend/core/Color.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/shell/CommandBlock.hpp>

#include <array>
#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace vtbackend
{

struct ColorPalette;

/// How one ending is worded: a label shown before the detail and the duration, and the label spoken.
///
/// The detail is the one the finish notification's FinishWordingTable names for the ending (asserted beside
/// that table); the words differ in case only because a chip starts a phrase ("Failed") where a title
/// continues one ("make failed").
struct BlockStatusWording
{
    std::optional<CommandEnding> ending {}; ///< The ending; nullopt for the Running row.
    std::string_view label; ///< Shown before the detail; empty where the duration says it all.
    FinishDetail detail = FinishDetail::None; ///< What follows the label: an exit code or a signal.
    std::string_view spokenLabel;             ///< What VoiceOver says first.
};

/// One row per CommandEnding in its order, then Running (R-f).
inline constexpr auto BlockStatusWordingRows = std::array {
    BlockStatusWording { .ending = CommandEnding::Unknown,
                         .label = "",
                         .detail = FinishDetail::None,
                         .spokenLabel = "Finished" },
    BlockStatusWording { .ending = CommandEnding::Succeeded,
                         .label = "",
                         .detail = FinishDetail::None,
                         .spokenLabel = "Succeeded" },
    BlockStatusWording { .ending = CommandEnding::Failed,
                         .label = "Failed",
                         .detail = FinishDetail::ExitCode,
                         .spokenLabel = "Failed" },
    BlockStatusWording { .ending = CommandEnding::Killed,
                         .label = "Killed",
                         .detail = FinishDetail::Signal,
                         .spokenLabel = "Killed" },
    BlockStatusWording { .ending = CommandEnding::Crashed,
                         .label = "Crashed",
                         .detail = FinishDetail::None,
                         .spokenLabel = "Crashed" },
    BlockStatusWording { .ending = CommandEnding::Interrupted,
                         .label = "Interrupted",
                         .detail = FinishDetail::None,
                         .spokenLabel = "Interrupted" },
    BlockStatusWording {
        .ending = std::nullopt, .label = "Running", .detail = FinishDetail::None, .spokenLabel = "Running" },
};
static_assert(BlockStatusWordingRows.size() == CommandEndingCount + 1);

/// What a shown detail is put after, by FinishDetail: `exit 8`, but a bare `SIGKILL`.
inline constexpr auto BlockStatusDetailPrefixes = std::array<std::string_view, 3> { "", "exit ", "" };
static_assert(BlockStatusDetailPrefixes.size() == static_cast<std::size_t>(FinishDetail::Signal) + 1);

/// The chip's text: "Failed · exit 8 · 12s", "Running · 8s", a success's bare "4ms".
/// @param record The block.
/// @param now The wall clock now, what a running command's time counts to.
/// @return The text; empty for a Prompting block, and for a success with no duration.
[[nodiscard]] std::string blockStatusText(CommandBlockRecord const& record,
                                          std::chrono::system_clock::time_point now);

/// The same parts as VoiceOver says them: "Failed, exit 8, 12 seconds", "Succeeded, 4 milliseconds".
/// @param record The block.
/// @param now The wall clock now.
/// @return The words; empty for a Prompting block.
[[nodiscard]] std::string spokenBlockStatus(CommandBlockRecord const& record,
                                            std::chrono::system_clock::time_point now);

/// @p duration spelled out as formatCommandDuration() abbreviates it: "12 seconds", "2 minutes 14 seconds".
/// @param duration How long.
/// @return The words.
[[nodiscard]] std::string spokenCommandDuration(std::chrono::steady_clock::duration duration);

/// The opacity of the chip's band over the page: the outcome's colour at 16 %.
inline constexpr auto BlockChipBandAlpha = 0.16F;

/// The opacity of the fold chip's band over the page: the text colour at 8 %.
inline constexpr auto FoldChipBandAlpha = 0.08F;

/// A chip's inks.
struct BlockChipColors
{
    RGBColor text {};                ///< The text, at least 4.5:1 over the band over the page.
    std::optional<RGBColor> band {}; ///< The band's colour; none for a success's status chip.
    float bandAlpha = 0.0F;          ///< The band's opacity over the page.

    bool operator==(BlockChipColors const&) const = default;
};

/// The chip's inks for @p outcome: its status colour (success, the gutter's text colour), a band of it,
/// lifted toward the page's ink where it would not read at MinimumTextContrast over the band.
/// @param palette The colour scheme.
/// @param outcome How the block reads.
/// @return The inks.
[[nodiscard]] BlockChipColors blockChipColors(ColorPalette const& palette,
                                              CommandBlockOutcome outcome) noexcept;

/// The fold chip's inks: the gutter's text colour on a band of itself, lifted toward the page's ink where it
/// would not read at MinimumTextContrast over the band.
/// @param palette The colour scheme.
/// @return The inks.
[[nodiscard]] BlockChipColors foldChipColors(ColorPalette const& palette) noexcept;

/// What the chip's text stands on: the page, with its band over it.
/// @param palette The colour scheme.
/// @param colors The chip's inks.
/// @return The ground's colour.
[[nodiscard]] RGBColor chipGround(ColorPalette const& palette, BlockChipColors const& colors) noexcept;

} // namespace vtbackend
