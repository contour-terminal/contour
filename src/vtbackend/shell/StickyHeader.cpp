// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/shell/StickyHeader.hpp>

#include <vtbackend/core/TerminalContext.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <string>
#include <string_view>

namespace vtbackend
{

namespace
{
    /// Whether @p mode shows a header, for a block whose head is above the viewport, while the viewport is
    /// @p viewport. The command header and the evicted placeholder below the very top both pass this gate.
    ///
    /// A table rather than a condition: one row per mode, one column per viewport position, so a fourth
    /// mode is a row here and nothing else.
    [[nodiscard]] constexpr bool modeShowsHeader(StickyHeaderMode mode, ViewportScroll viewport) noexcept
    {
        //                                              Live   Scrolled
        constexpr auto Table = std::array { std::array { false, false }, // Never
                                            std::array { false, true },  // Scrolled
                                            std::array { true, true } }; // Always
        return Table[static_cast<size_t>(mode)][static_cast<size_t>(viewport)];
    }

    /// How the chip words one way of ending: its mark, and the detail after it.
    struct ChipWording
    {
        CommandEnding ending;  ///< The ending this row words.
        std::string_view mark; ///< `✓` or `✗`: what isFailure() says, as the chip's colour does.
        FinishDetail detail;   ///< What follows the mark.
    };

    /// One row per CommandEnding. Unlike the finish notification, the chip has no verb to say `crashed` or
    /// `interrupted` with, so for those it gives the code (`✗ 139`) where the notification gives none.
    constexpr auto ChipWordingTable = std::array {
        ChipWording { .ending = CommandEnding::Unknown, .mark = "✓", .detail = FinishDetail::None },
        ChipWording { .ending = CommandEnding::Succeeded, .mark = "✓", .detail = FinishDetail::None },
        ChipWording { .ending = CommandEnding::Failed, .mark = "✗", .detail = FinishDetail::ExitCode },
        ChipWording { .ending = CommandEnding::Killed, .mark = "✗", .detail = FinishDetail::Signal },
        ChipWording { .ending = CommandEnding::Crashed, .mark = "✗", .detail = FinishDetail::ExitCode },
        ChipWording { .ending = CommandEnding::Interrupted, .mark = "✗", .detail = FinishDetail::ExitCode },
    };

    /// How a finished block's chip names its ending: `✓`, `✗ SIGKILL`, `✗ 2` or a bare `✗`.
    [[nodiscard]] std::string finishedMarkOf(CommandBlockRecord const& record)
    {
        auto const ending = commandEndingOf(record.exitCode, record.outcome);
        auto const row = std::ranges::find(ChipWordingTable, ending, &ChipWording::ending);
        auto const& wording = row != ChipWordingTable.end() ? *row : ChipWordingTable.front();
        auto const detail = finishDetailText(wording.detail, record.exitCode, record.outcome);
        return detail.empty() ? std::string { wording.mark } : std::format("{} {}", wording.mark, detail);
    }

    /// @return @p text, UTF-8, as the code points the render buffer lays out.
    [[nodiscard]] std::u32string toUtf32(std::string_view text)
    {
        return unicode::convert_to<char32_t>(text);
    }
} // namespace

std::optional<StickyHeaderDecision> stickyHeaderFor(StickyHeaderInput const& input) noexcept
{
    // The alternate screen belongs to a full-screen application: it carries no blocks, and a header
    // there would sit over vim's first line.
    if (input.screen != ScreenType::Primary || !input.block)
        return std::nullopt;

    auto const& block = *input.block;

    // A block still at its prompt has printed nothing to scroll past.
    if (block.state == CommandBlockState::Prompting)
        return std::nullopt;

    // The header steps aside for a cursor on the row it would cover, which would hide it: the vi-mode cursor
    // `gg` put there, or the terminal's own cursor on a row the application is writing.
    auto const mayCoverTheRow =
        input.cursor == TopRowCursor::Elsewhere && modeShowsHeader(input.settings.mode, input.viewport);

    switch (block.head)
    {
        case StickyHeadPosition::Visible: return std::nullopt;
        case StickyHeadPosition::Evicted:
            // The placeholder stands in for the input row the grid no longer holds, so the header does not
            // vanish just because the prompt was evicted. At the very top of the scrollback it shows always
            // -- whatever the mode, and over a cursor on the row too, being the one sign that the rows above
            // were dropped; anywhere else wherever the mode shows a header. The top row's part is not
            // consulted -- with the head gone there is no input row to tell apart.
            if (input.settings.evictedPlaceholder == EvictedPlaceholder::Shown
                && (input.topRowAge == TopRowAge::Oldest || mayCoverTheRow))
                return StickyHeaderDecision { .kind = StickyHeaderKind::Evicted, .block = block.id };
            return std::nullopt;
        case StickyHeadPosition::Above:
            // Over the command line itself the header would only repeat what is on screen.
            if (block.topRow == TopRowPart::Output && mayCoverTheRow)
                return StickyHeaderDecision { .kind = StickyHeaderKind::Command, .block = block.id };
            return std::nullopt;
    }
    return std::nullopt;
}

bool stickyHeaderPossible(StickyHeaderSettings const& settings,
                          ScreenType screen,
                          ViewportScroll viewport,
                          TopRowAge topRowAge) noexcept
{
    // The command header passes the mode's gate; the placeholder passes it too, or stands at the very top.
    return screen == ScreenType::Primary
           && (modeShowsHeader(settings.mode, viewport)
               || (settings.evictedPlaceholder == EvictedPlaceholder::Shown
                   && topRowAge == TopRowAge::Oldest));
}

std::u32string stickyHeaderChip(CommandBlockRecord const& record, std::chrono::system_clock::time_point now)
{
    switch (record.state)
    {
        case CommandBlockState::Prompting: return {};
        case CommandBlockState::Running:
            if (!record.commandStartedAt)
                return U"running";
            return toUtf32(
                "running "
                + formatCommandDuration(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    now - *record.commandStartedAt)));
        case CommandBlockState::Finished:
            if (!record.duration)
                return toUtf32(finishedMarkOf(record));
            return toUtf32(
                std::format("{} {}", finishedMarkOf(record), formatCommandDuration(*record.duration)));
    }
    return {};
}

std::string stickyHeaderCommandLine(CommandLineText const& commandLine)
{
    auto const cut = CommandLineText { truncateUtf8(commandLine.raw(), MaxStickyHeaderCommandLineBytes) };
    return truncateUtf8(cut.display(), MaxStickyHeaderCommandLineBytes);
}

std::u32string evictedPlaceholderText(CommandLineText const& commandLine, ColumnCount budget)
{
    // Four bytes a column: no codepoint that takes one is longer. Cut while still raw, so the sanitiser never
    // reads past what can show; it alone makes the cut presentable.
    auto constexpr BytesPerColumn = std::size_t { 4 };
    auto const maxBytes = BytesPerColumn * unbox<std::size_t>(std::max(budget, ColumnCount(0)));
    auto const command = CommandLineText { truncateUtf8(commandLine.raw(), maxBytes) }.display();
    if (command.empty())
        return U"⋯ earlier output evicted";
    return toUtf32(std::format("⋯ {} — earlier output evicted", command));
}

} // namespace vtbackend
