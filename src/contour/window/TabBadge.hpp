// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which badge a tab shows and how it looks: the decision is one priority table, the look another, and
/// both are pure so the strip, the accessibility tree and the tests read the same rows.

#include <contour/config/UiStyle.hpp>
#include <contour/window/ProgramStatusBadge.hpp>

#include <vtbackend/shell/CommandBlock.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace contour::window
{

/// Whether a background tab has output its user has not seen.
enum class TabActivity : uint8_t
{
    None = 0,     ///< Nothing new.
    UnseenOutput, ///< Output arrived while the tab was out of sight.
};

/// What a finished command in a tab says, once the tab is out of sight.
/// @note @c strongerFinishedBlock relies on the enumerator order None < Succeeded < Failed.
enum class FinishedBlockBadge : uint8_t
{
    None = 0, ///< No verdict worth showing.
    Succeeded,
    Failed,
};

/// The badge kinds a tab can show.
enum class TabBadgeKind : uint8_t
{
    None = 0, ///< No badge.
    Output,   ///< Unseen output.
    Done,     ///< Finished well.
    Working,  ///< Running, with progress when known.
    Error,    ///< Failed.
    Blocked,  ///< Waits for the user.
};

/// Number of @c TabBadgeKind enumerators, derived from the last one.
inline constexpr size_t TabBadgeKindCount = static_cast<size_t>(TabBadgeKind::Blocked) + 1;

/// Everything that competes for a tab's badge slot.
struct TabBadgeInputs
{
    BadgeKind programStatus = BadgeKind::None; ///< The OSC 7501 program status.
    std::optional<int> progressPercent {};     ///< The program's progress, when it gave one.
    FinishedBlockBadge finishedBlock = FinishedBlockBadge::None; ///< The strongest finished command.
    TabActivity activity = TabActivity::None;                    ///< Unseen output.
};

/// The badge a tab shows.
struct TabBadge
{
    TabBadgeKind kind = TabBadgeKind::None; ///< What to draw.
    std::optional<int> progressPercent {};  ///< Percent done; set for @c Working only.

    bool operator==(TabBadge const&) const = default;
};

/// One row of the priority table: @c kind wins when any non-None source field equals the input's.
struct TabBadgeRule
{
    TabBadgeKind kind;                ///< The badge this row yields.
    BadgeKind programStatus;          ///< The program status that selects it, or None.
    FinishedBlockBadge finishedBlock; ///< The finished-command verdict that selects it, or None.
    TabActivity activity;             ///< The activity that selects it, or None.
};

/// The badge rules, most urgent first.
inline constexpr auto TabBadgeRules = std::array {
    TabBadgeRule { TabBadgeKind::Blocked, BadgeKind::Blocked, FinishedBlockBadge::None, TabActivity::None },
    TabBadgeRule { TabBadgeKind::Error, BadgeKind::Error, FinishedBlockBadge::Failed, TabActivity::None },
    TabBadgeRule { TabBadgeKind::Working, BadgeKind::Working, FinishedBlockBadge::None, TabActivity::None },
    TabBadgeRule { TabBadgeKind::Done, BadgeKind::Done, FinishedBlockBadge::Succeeded, TabActivity::None },
    TabBadgeRule {
        TabBadgeKind::Output, BadgeKind::None, FinishedBlockBadge::None, TabActivity::UnseenOutput },
};

/// Whether @p rule selects @p inputs: any of its non-None fields equals the input's.
/// @param rule   The row.
/// @param inputs The competing sources.
/// @return True when the row matches.
[[nodiscard]] constexpr bool tabBadgeRuleMatches(TabBadgeRule const& rule,
                                                 TabBadgeInputs const& inputs) noexcept
{
    return (rule.programStatus != BadgeKind::None && rule.programStatus == inputs.programStatus)
           || (rule.finishedBlock != FinishedBlockBadge::None && rule.finishedBlock == inputs.finishedBlock)
           || (rule.activity != TabActivity::None && rule.activity == inputs.activity);
}

/// Picks the badge a tab shows.
/// @param inputs The competing sources.
/// @return The first matching rule's kind; the progress is passed through for Working only.
[[nodiscard]] constexpr TabBadge resolveTabBadge(TabBadgeInputs const& inputs) noexcept
{
    auto const rule = std::ranges::find_if(
        TabBadgeRules, [&](TabBadgeRule const& row) { return tabBadgeRuleMatches(row, inputs); });
    if (rule == TabBadgeRules.end())
        return {};
    if (rule->kind == TabBadgeKind::Working)
        return { rule->kind, inputs.progressPercent };
    return { rule->kind, std::nullopt };
}

/// Whether the program status is what put a badge of @p kind on the tab, so that its text is the tooltip.
/// @param kind          The badge the tab shows (resolveTabBadge()).
/// @param programStatus The tab's OSC 7501 badge kind.
/// @return True when @p kind's rule is selected by @p programStatus.
[[nodiscard]] constexpr bool isProgramStatusBadge(TabBadgeKind kind, BadgeKind programStatus) noexcept
{
    auto const rule = std::ranges::find(TabBadgeRules, kind, &TabBadgeRule::kind);
    return rule != TabBadgeRules.end() && programStatus != BadgeKind::None
           && rule->programStatus == programStatus;
}

/// What one pane's header shows of its own program status.
struct PaneStatusBadge
{
    TabBadgeKind kind = TabBadgeKind::None; ///< The badge the pane's records earn.
    std::string text;                       ///< What it says; empty with no badge.

    bool operator==(PaneStatusBadge const&) const = default;
};

/// The badge one pane's own records earn, decided as a tab's is (badgeFor(), then resolveTabBadge()) but
/// in front: a visible header is on screen, so what shows only out of sight (Done, Error) stays the tab's.
/// @param records The pane's resolved records (resolvedRecords()), least recently updated first.
/// @return The pane's badge and its text.
[[nodiscard]] inline PaneStatusBadge paneStatusBadgeOf(
    std::span<vtbackend::ProgramStatusRecord const> records)
{
    auto program = badgeFor(records, TabFocus::Foreground);
    auto const kind = resolveTabBadge({ .programStatus = program.kind }).kind;
    return { .kind = kind, .text = kind == TabBadgeKind::None ? std::string {} : std::move(program.text) };
}

/// How the Pill shape draws a badge kind. The Flat shape always draws the glyph.
enum class BadgeMark : uint8_t
{
    None = 0, ///< Nothing.
    Dot,      ///< A small filled dot.
    Ring,     ///< A ring, with progress when known.
    Disc,     ///< A filled disc carrying the glyph.
    Glyph,    ///< The glyph alone.
};

/// How one badge kind looks.
struct TabBadgeLook
{
    TabBadgeKind kind;               ///< The kind this row describes.
    std::string_view glyph;          ///< The glyph the Flat shape draws.
    BadgeMark mark;                  ///< What the Pill shape draws.
    config::StatusColorRole ink;     ///< The status role colouring it.
    std::string_view accessibleName; ///< The name assistive technology announces (English source text).
};

/// The looks, indexed by @c TabBadgeKind.
inline constexpr std::array<TabBadgeLook, TabBadgeKindCount> TabBadgeLooks {
    TabBadgeLook { TabBadgeKind::None, "", BadgeMark::None, config::StatusColorRole::Running, "" },
    TabBadgeLook {
        TabBadgeKind::Output, "•", BadgeMark::Dot, config::StatusColorRole::Running, "New output" },
    TabBadgeLook { TabBadgeKind::Done, "✓", BadgeMark::Glyph, config::StatusColorRole::Success, "Done" },
    TabBadgeLook { TabBadgeKind::Working, "◑", BadgeMark::Ring, config::StatusColorRole::Running, "Working" },
    TabBadgeLook { TabBadgeKind::Error, "✕", BadgeMark::Glyph, config::StatusColorRole::Danger, "Failed" },
    TabBadgeLook {
        TabBadgeKind::Blocked, "!", BadgeMark::Disc, config::StatusColorRole::Warning, "Waiting for you" },
};

namespace detail
{
    [[nodiscard]] constexpr bool tabBadgeLooksInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, TabBadgeLooks.size()))
            if (TabBadgeLooks[i].kind != TabBadgeKind(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::tabBadgeLooksInOrder(), "TabBadgeLooks must list the kinds in enumerator order");

/// The look of a badge kind.
/// @param kind The kind.
/// @return Its row in @c TabBadgeLooks.
[[nodiscard]] constexpr TabBadgeLook const& tabBadgeLook(TabBadgeKind kind) noexcept
{
    return TabBadgeLooks[static_cast<size_t>(kind)];
}

/// The name QML reads a mark by.
/// @param mark The mark.
/// @return "none", "dot", "ring", "disc" or "glyph".
[[nodiscard]] constexpr std::string_view badgeMarkName(BadgeMark mark) noexcept
{
    switch (mark)
    {
        case BadgeMark::None: return "none";
        case BadgeMark::Dot: return "dot";
        case BadgeMark::Ring: return "ring";
        case BadgeMark::Disc: return "disc";
        case BadgeMark::Glyph: return "glyph";
    }
    return "none";
}

/// One row of the finished-command table.
struct FinishedBlockRow
{
    vtbackend::CommandEnding ending; ///< How the command ended.
    FinishedBlockBadge badge;        ///< What the tab says about it.
};

/// What each way of ending says about a tab. A failure here is exactly @c vtbackend::isFailure, so a
/// tab never disagrees with the gutter and the finish notification; a ^C is a failure there too.
inline constexpr std::array<FinishedBlockRow, vtbackend::CommandEndingCount> FinishedBlockRows {
    FinishedBlockRow { vtbackend::CommandEnding::Unknown, FinishedBlockBadge::None },
    FinishedBlockRow { vtbackend::CommandEnding::Succeeded, FinishedBlockBadge::Succeeded },
    FinishedBlockRow { vtbackend::CommandEnding::Failed, FinishedBlockBadge::Failed },
    FinishedBlockRow { vtbackend::CommandEnding::Killed, FinishedBlockBadge::Failed },
    FinishedBlockRow { vtbackend::CommandEnding::Crashed, FinishedBlockBadge::Failed },
    FinishedBlockRow { vtbackend::CommandEnding::Interrupted, FinishedBlockBadge::Failed },
};

namespace detail
{
    [[nodiscard]] constexpr bool finishedBlockRowsConsistent() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, FinishedBlockRows.size()))
        {
            auto const& row = FinishedBlockRows[i];
            if (row.ending != vtbackend::CommandEnding(i)
                || (row.badge == FinishedBlockBadge::Failed) != vtbackend::isFailure(row.ending))
                return false;
        }
        return true;
    }
} // namespace detail

static_assert(detail::finishedBlockRowsConsistent(),
              "FinishedBlockRows must list the endings in order and agree with isFailure");

/// The badge verdict of a finished command.
/// @param ending How the command ended.
/// @return Its row in @c FinishedBlockRows.
[[nodiscard]] constexpr FinishedBlockBadge finishedBlockBadgeOf(vtbackend::CommandEnding ending) noexcept
{
    return FinishedBlockRows[static_cast<size_t>(ending)].badge;
}

/// The stronger of two verdicts: Failed over Succeeded over None.
/// @param a One verdict.
/// @param b The other.
/// @return The stronger.
[[nodiscard]] constexpr FinishedBlockBadge strongerFinishedBlock(FinishedBlockBadge a,
                                                                 FinishedBlockBadge b) noexcept
{
    return std::to_underlying(a) >= std::to_underlying(b) ? a : b;
}

} // namespace contour::window
