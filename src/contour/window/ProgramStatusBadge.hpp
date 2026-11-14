// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/GraphemeElide.hpp>
#include <contour/window/StatusUrgency.hpp>

#include <vtbackend/core/UntrustedText.hpp>
#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <vtworkspace/Pane.hpp>
#include <vtworkspace/Tab.hpp>

namespace contour::window
{

/// The badge a tab shows: its kind, the progress of a Working one, and the tooltip text.
struct ProgramStatusBadge
{
    BadgeKind kind = BadgeKind::None; ///< What to draw.
    std::optional<uint8_t> progress;  ///< Percent done, when the record that won gave one.
    std::string text;                 ///< The tooltip: disarmed, at most @ref MaxBadgeTextLength characters.

    bool operator==(ProgramStatusBadge const&) const = default;
};

/// The longest badge text, in characters, the ellipsis included.
inline constexpr std::size_t MaxBadgeTextLength = 120;

/// When a badge kind shows.
enum class BadgeVisibility : uint8_t
{
    Always = 0,  ///< In every tab: it is still happening, or it waits for the user.
    WhileUnseen, ///< Only until the user has seen the record that reports it (its generation).
};

/// How one program state reads on the tab strip. A state with no row shows nothing (Idle).
struct BadgeRow
{
    vtbackend::ProgramState state; ///< The record's state.
    BadgeKind kind;                ///< What it shows as.
    BadgeVisibility visibility;    ///< When.
};

/// The states that show, most urgent first (see StatusUrgency.hpp, which checks the order): the first row
/// a tab's records match wins. An unseen Done outranks Working; a seen one is skipped.
inline constexpr auto BadgeRows = std::array {
    BadgeRow { .state = vtbackend::ProgramState::Blocked,
               .kind = BadgeKind::Blocked,
               .visibility = BadgeVisibility::Always },
    BadgeRow { .state = vtbackend::ProgramState::Error,
               .kind = BadgeKind::Error,
               .visibility = BadgeVisibility::WhileUnseen },
    BadgeRow { .state = vtbackend::ProgramState::Done,
               .kind = BadgeKind::Done,
               .visibility = BadgeVisibility::WhileUnseen },
    BadgeRow { .state = vtbackend::ProgramState::Working,
               .kind = BadgeKind::Working,
               .visibility = BadgeVisibility::Always },
};

namespace detail
{
    [[nodiscard]] constexpr bool badgeRowsByUrgency() noexcept
    {
        return std::ranges::is_sorted(BadgeRows, std::ranges::greater {}, [](BadgeRow const& row) {
            return urgencyOf(row.kind, StatusSeen::Unseen);
        });
    }
} // namespace detail

static_assert(detail::badgeRowsByUrgency(), "BadgeRows must list the rows most urgent first");

/// Whether the user has seen @p record.
/// @param record The record.
/// @param seenGeneration The newest generation the user has seen (TabAttention); 0 when none.
/// @return Seen when the record's generation is not newer than @p seenGeneration.
[[nodiscard]] constexpr StatusSeen statusSeenBy(vtbackend::ProgramStatusRecord const& record,
                                                uint64_t seenGeneration) noexcept
{
    return record.generation > seenGeneration ? StatusSeen::Unseen : StatusSeen::Seen;
}

/// A seen-generation that has seen every record: what a view in front of the user passes.
inline constexpr uint64_t AllRecordsSeen = std::numeric_limits<uint64_t>::max();

/// Makes a record's text fit for a badge, as the daemon's sanitizer does from the same table
/// (vtbackend::UntrustedTextTreatments): drops the controls and the invisible and bidi formatting characters,
/// which could reorder or hide what the badge says, and folds line breaks by the same
/// vtbackend::UntrustedTextFilter: one space between two shown characters, none at either end. A peer could
/// still send what the daemon would have dropped, which is why the GUI filters again.
/// @param text UTF-8 text a program sent.
/// @return The text, filtered.
[[nodiscard]] inline std::string disarmInvisibleFormatting(std::string_view text)
{
    auto const codepoints = unicode::convert_to<char32_t>(text);
    auto kept = std::u32string {};
    kept.reserve(codepoints.size());
    auto filter = vtbackend::UntrustedTextFilter {};
    for (auto const codepoint: codepoints)
    {
        auto const shown = filter.next(vtbackend::treatmentOf(codepoint));
        if (shown == vtbackend::ShownAs::AfterSpace)
            kept.push_back(U' ');
        if (shown != vtbackend::ShownAs::Nothing)
            kept.push_back(codepoint);
    }
    return unicode::convert_to<char>(std::u32string_view { kept });
}

/// @p text cut to @p maxLength characters (grapheme clusters), its last one an ellipsis when it was cut.
[[nodiscard]] inline std::string elided(std::string_view text, std::size_t maxLength)
{
    return elideEnd(text, maxLength, ElideUnit::Clusters);
}

/// What a badge's tooltip says, in order of preference: the first of them a record does not leave empty.
inline constexpr auto BadgeTextSources = std::array {
    &vtbackend::ProgramStatusRecord::title,
    &vtbackend::ProgramStatusRecord::message,
    &vtbackend::ProgramStatusRecord::app,
};

/// @return @p record's title, else its message, else its app; empty when it has none.
[[nodiscard]] inline std::string_view badgeTextOf(vtbackend::ProgramStatusRecord const& record) noexcept
{
    for (auto const source: BadgeTextSources)
        if (!(record.*source).empty())
            return record.*source;
    return {};
}

/// The badge the most urgent of @p records earns (BadgeRows); among records of the same urgency the most
/// recently updated wins.
/// @param records A tab's records, least recently updated first, each with its app resolved as
///        vtbackend::ProgramStatusSet::appOf() does (@see resolvedRecords).
/// @param seenGeneration The newest generation the user has seen: a BadgeVisibility::WhileUnseen row (Done,
///        Error) shows only when the record that wins it is newer. Blocked and Working ignore it.
/// @return The badge; BadgeKind::None when nothing shows.
[[nodiscard]] inline ProgramStatusBadge badgeFor(std::span<vtbackend::ProgramStatusRecord const> records,
                                                 uint64_t seenGeneration)
{
    auto const newestFirst = records | std::views::reverse;
    for (auto const& row: BadgeRows)
    {
        auto const found = std::ranges::find(newestFirst, row.state, &vtbackend::ProgramStatusRecord::state);
        if (found == newestFirst.end())
            continue;
        auto const& record = *found;
        if (row.visibility == BadgeVisibility::WhileUnseen
            && statusSeenBy(record, seenGeneration) == StatusSeen::Seen)
            continue;
        return ProgramStatusBadge {
            .kind = row.kind,
            .progress = row.kind == BadgeKind::Working ? record.progress : std::nullopt,
            .text = elided(disarmInvisibleFormatting(badgeTextOf(record)), MaxBadgeTextLength),
        };
    }
    return {};
}

/// The records of @p set, each carrying the app it speaks for, its own or its nearest ancestor's: what
/// badgeFor() reads, so that records of several panes can be pooled without one inheriting another's app.
/// @param set A terminal's records; the caller holds that terminal's lock.
[[nodiscard]] inline std::vector<vtbackend::ProgramStatusRecord> resolvedRecords(
    vtbackend::ProgramStatusSet const& set)
{
    auto records = std::vector<vtbackend::ProgramStatusRecord> {};
    records.reserve(set.records().size());
    for (auto const& record: set.records())
    {
        auto resolved = record;
        resolved.app = std::string { set.appOf(record) };
        records.push_back(std::move(resolved));
    }
    return records;
}

/// What a tab's pane holds: the resolved records (resolvedRecords()) of the terminal behind @p session,
/// read under its lock; empty when the session has none or is gone.
using PaneProgramStatus = std::function<std::vector<vtbackend::ProgramStatusRecord>(vtworkspace::SessionId)>;

/// What one pane's session has been seen up to: the newest record generation its user has seen.
using PaneSeenGeneration = std::function<uint64_t(vtworkspace::SessionId)>;

/// The badge of @p tab, whose records are those of all its panes' terminals. Generations count per terminal,
/// so each pane's seen records are dropped by its own seen generation before the panes' records are pooled.
/// @param tab The tab.
/// @param paneRecords Reads one pane's records.
/// @param paneSeen Reads the generation one pane's session has seen up to.
[[nodiscard]] inline ProgramStatusBadge badgeForTab(vtworkspace::Tab const& tab,
                                                    PaneProgramStatus const& paneRecords,
                                                    PaneSeenGeneration const& paneSeen)
{
    auto records = std::vector<vtbackend::ProgramStatusRecord> {};
    if (auto* root = tab.rootPane())
        root->walkTree([&](vtworkspace::Pane& pane) {
            if (!pane.isLeaf())
                return;
            auto paneOwn = paneRecords(pane.session());
            auto const seen = paneSeen(pane.session());
            std::erase_if(paneOwn, [&](vtbackend::ProgramStatusRecord const& record) {
                auto const row = std::ranges::find(BadgeRows, record.state, &BadgeRow::state);
                return row != BadgeRows.end() && row->visibility == BadgeVisibility::WhileUnseen
                       && statusSeenBy(record, seen) == StatusSeen::Seen;
            });
            records.insert(records.end(),
                           std::make_move_iterator(paneOwn.begin()),
                           std::make_move_iterator(paneOwn.end()));
        });
    return badgeFor(records, 0);
}

/// The badges of a window's tabs, each computed once until it is dropped. A badge costs one terminal lock
/// per pane, and the strip asks for it once per role (kind, text, progress) of every row it repaints; with
/// this, a repaint of T tabs of P panes takes T*P locks rather than three times that.
///
/// The owner drops a tab's badge whenever its row changes, and every badge when which tab is in front
/// changes: that is when a pane's records become seen, and a seen Done or Error no longer shows. Not
/// thread-safe: the GUI thread's alone.
class TabBadgeCache
{
  public:
    /// @param tab The tab.
    /// @param compute Computes @p tab's badge; called only when none is held.
    /// @return The badge held for @p tab, computed first if none is.
    template <typename Compute>
    [[nodiscard]] ProgramStatusBadge get(vtworkspace::TabId tab, Compute const& compute)
    {
        auto const [entry, inserted] = _badges.try_emplace(tab.value);
        if (inserted)
            entry->second = compute();
        return entry->second;
    }

    /// Drops @p tab's badge: its records, or its panes, may have changed.
    void drop(vtworkspace::TabId tab) { _badges.erase(tab.value); }

    /// Drops every badge.
    void clear() noexcept { _badges.clear(); }

  private:
    std::unordered_map<uint64_t, ProgramStatusBadge> _badges;
};

} // namespace contour::window
