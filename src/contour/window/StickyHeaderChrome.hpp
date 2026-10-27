// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/UiStyle.hpp>

#include <vtbackend/core/CommandBlockId.hpp>
#include <vtbackend/core/CommandBlockOutcome.hpp>
#include <vtbackend/core/StickyHeaderKind.hpp>
#include <vtbackend/render/RenderBuffer.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <span>
#include <string>
#include <string_view>

namespace contour::window
{

/// How the chrome marks a block of one outcome: the glyph before its command, the status role that inks
/// it, and the mark the terminal's chip opens with (stickyHeaderChip), which the chrome's detail drops
/// because the glyph already says it.
struct StickyHeaderGlyphRow
{
    vtbackend::CommandBlockOutcome outcome = vtbackend::CommandBlockOutcome::Success; ///< The outcome.
    std::string_view glyph;                                                           ///< ✓, ✗ or ▸.
    config::StatusColorRole role = config::StatusColorRole::Success; ///< What inks the glyph.
    std::string_view chipMark;                                       ///< The chip's own mark.
};

/// One row per vtbackend::CommandBlockOutcome, in its order. Running is the accent, never yellow.
inline constexpr auto StickyHeaderGlyphRows = std::array {
    StickyHeaderGlyphRow { .outcome = vtbackend::CommandBlockOutcome::Success,
                           .glyph = "✓",
                           .role = config::StatusColorRole::Success,
                           .chipMark = "✓" },
    StickyHeaderGlyphRow { .outcome = vtbackend::CommandBlockOutcome::Failure,
                           .glyph = "✗",
                           .role = config::StatusColorRole::Danger,
                           .chipMark = "✗" },
    StickyHeaderGlyphRow { .outcome = vtbackend::CommandBlockOutcome::Running,
                           .glyph = "▸",
                           .role = config::StatusColorRole::Running,
                           .chipMark = {} },
};
static_assert(StickyHeaderGlyphRows.size() == vtbackend::CommandBlockOutcomeCount);

/// The glyph an evicted block's header shows in place of its outcome's: its command row is gone.
inline constexpr auto StickyHeaderEvictedGlyph = std::string_view { "⋯" };

/// The sticky header as the pane header or the title band shows it: `✓ cmake --build … · 2m 14s`.
struct StickyHeaderChrome
{
    vtbackend::StickyHeaderKind kind = vtbackend::StickyHeaderKind::Command; ///< Command or placeholder.
    vtbackend::CommandBlockId block {};                                      ///< What a jump goes to.
    vtbackend::CommandBlockOutcome outcome = vtbackend::CommandBlockOutcome::Success; ///< How it reads.
    config::StatusColorRole statusRole = config::StatusColorRole::Success; ///< What inks the glyph.
    std::string glyph {};   ///< ✓, ✗, ▸, or ⋯ for an evicted block.
    std::string command {}; ///< The command line, or what stands for it.
    std::string detail {};  ///< The chip's text without its mark: `2m 14s`, `2 1m 04s`, `running 12s`.

    bool operator==(StickyHeaderChrome const&) const = default;
};

namespace detail
{
    /// What @p cells spell, left to right, in UTF-8.
    [[nodiscard]] inline std::string stickyHeaderCellText(std::span<vtbackend::RenderCell const> cells)
    {
        auto text = std::u32string {};
        for (auto const& cell: cells)
            text += cell.codepoints;
        return unicode::convert_to<char>(std::u32string_view { text });
    }

    /// @p text without blanks at either end.
    [[nodiscard]] inline std::string trimmed(std::string_view text)
    {
        auto const first = text.find_first_not_of(' ');
        if (first == std::string_view::npos)
            return {};
        return std::string { text.substr(first, text.find_last_not_of(' ') - first + 1) };
    }
} // namespace detail

/// Binds the terminal's sticky header to what the chrome shows of it.
///
/// The glyph and its role come from StickyHeaderGlyphRows by outcome; the command is the record's command
/// line, else the copied row's text trimmed (a prompt row the shell reported no command line for); the
/// detail is the chip's text with the outcome's mark dropped. An evicted block shows `⋯` and the
/// placeholder's text after its own `⋯`.
/// @param header The header the terminal laid out (Terminal::stickyHeaderSnapshot).
/// @return What the chrome shows.
[[nodiscard]] inline StickyHeaderChrome bindStickyHeader(vtbackend::RenderStickyHeader const& header)
{
    auto const& row = StickyHeaderGlyphRows.at(static_cast<std::size_t>(header.outcome));
    auto const evicted = header.kind == vtbackend::StickyHeaderKind::Evicted;

    auto chip = detail::trimmed(detail::stickyHeaderCellText(header.chip));
    if (!row.chipMark.empty() && std::string_view { chip }.starts_with(row.chipMark))
        chip = detail::trimmed(std::string_view { chip }.substr(row.chipMark.size()));

    auto command = !evicted && !header.commandLine.empty()
                       ? header.commandLine
                       : detail::trimmed(detail::stickyHeaderCellText(header.cells));
    // The placeholder opens with the glyph the chrome already shows.
    if (evicted && std::string_view { command }.starts_with(StickyHeaderEvictedGlyph))
        command = detail::trimmed(std::string_view { command }.substr(StickyHeaderEvictedGlyph.size()));

    return StickyHeaderChrome {
        .kind = header.kind,
        .block = header.block,
        .outcome = header.outcome,
        .statusRole = row.role,
        .glyph = std::string { evicted ? StickyHeaderEvictedGlyph : row.glyph },
        .command = std::move(command),
        .detail = std::move(chip),
    };
}

} // namespace contour::window
