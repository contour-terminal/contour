// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/screen/ProgramStatusSet.hpp>
#include <vtbackend/vt/ProgramStatus.hpp>

#include <libunicode/convert.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
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

/// What a tab's program-status badge shows (OSC 7501).
enum class BadgeKind : uint8_t
{
    None = 0, ///< Nothing: no record, or none worth showing.
    Working,  ///< A spinner, with the progress when the program gave one.
    Blocked,  ///< `!`: the program waits for the user.
    Done,     ///< `✓`: finished, with results the user has not seen.
    Error,    ///< `✕`: failed.
};

/// Whether a tab is the one the user is looking at.
enum class TabFocus : uint8_t
{
    Background = 0, ///< Another tab, or another view, is in front.
    Foreground,     ///< This tab is the window's active one.
};

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
    Always = 0,     ///< In every tab: it is still happening, or it waits for the user.
    BackgroundOnly, ///< Only in a tab the user is not looking at: what it reports is already on screen.
};

/// How one program state reads on the tab strip. A state with no row shows nothing (Idle).
struct BadgeRow
{
    vtbackend::ProgramState state; ///< The record's state.
    BadgeKind kind;                ///< What it shows as.
    BadgeVisibility visibility;    ///< When.
};

/// The states that show, most urgent first: the first row a tab's records match wins.
inline constexpr auto BadgeRows = std::array {
    BadgeRow { .state = vtbackend::ProgramState::Blocked,
               .kind = BadgeKind::Blocked,
               .visibility = BadgeVisibility::Always },
    BadgeRow { .state = vtbackend::ProgramState::Error,
               .kind = BadgeKind::Error,
               .visibility = BadgeVisibility::BackgroundOnly },
    BadgeRow { .state = vtbackend::ProgramState::Working,
               .kind = BadgeKind::Working,
               .visibility = BadgeVisibility::Always },
    BadgeRow { .state = vtbackend::ProgramState::Done,
               .kind = BadgeKind::Done,
               .visibility = BadgeVisibility::BackgroundOnly },
};

/// A range of code points that is never shown, inclusive.
struct CodepointRange
{
    char32_t first; ///< The first code point of the range.
    char32_t last;  ///< The last.
};

/// What disarmInvisibleFormatting() removes: zero-width characters, bidi embeddings, overrides and
/// isolates, invisible operators and the byte-order mark, which could reorder or hide what the badge
/// says; and the C0 and C1 controls, which the decoder already refuses but a peer could still send.
inline constexpr auto InvisibleFormatting = std::array {
    CodepointRange { .first = 0x0000, .last = 0x001F }, CodepointRange { .first = 0x007F, .last = 0x009F },
    CodepointRange { .first = 0x200B, .last = 0x200F }, CodepointRange { .first = 0x202A, .last = 0x202E },
    CodepointRange { .first = 0x2060, .last = 0x2064 }, CodepointRange { .first = 0x2066, .last = 0x2069 },
    CodepointRange { .first = 0xFEFF, .last = 0xFEFF },
};

/// Removes every character of @ref InvisibleFormatting from @p text.
/// @param text UTF-8 text a program sent.
/// @return The text without them.
[[nodiscard]] inline std::string disarmInvisibleFormatting(std::string_view text)
{
    auto codepoints = unicode::convert_to<char32_t>(text);
    std::erase_if(codepoints, [](char32_t codepoint) {
        return std::ranges::any_of(InvisibleFormatting, [codepoint](CodepointRange const& range) {
            return codepoint >= range.first && codepoint <= range.last;
        });
    });
    return unicode::convert_to<char>(std::u32string_view { codepoints });
}

/// @p text cut to @p maxLength characters, its last one an ellipsis when it was cut.
[[nodiscard]] inline std::string elided(std::string_view text, std::size_t maxLength)
{
    auto codepoints = unicode::convert_to<char32_t>(text);
    if (codepoints.size() <= maxLength)
        return std::string { text };
    codepoints.resize(maxLength - 1);
    codepoints.push_back(U'…');
    return unicode::convert_to<char>(std::u32string_view { codepoints });
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
/// @param focus Whether the tab is in front.
/// @return The badge; BadgeKind::None when nothing shows.
[[nodiscard]] inline ProgramStatusBadge badgeFor(std::span<vtbackend::ProgramStatusRecord const> records,
                                                 TabFocus focus)
{
    auto const newestFirst = records | std::views::reverse;
    for (auto const& row: BadgeRows)
    {
        if (row.visibility == BadgeVisibility::BackgroundOnly && focus == TabFocus::Foreground)
            continue;
        auto const found = std::ranges::find(newestFirst, row.state, &vtbackend::ProgramStatusRecord::state);
        if (found == newestFirst.end())
            continue;
        auto const& record = *found;
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

/// The badge of @p tab, whose records are those of all its panes' terminals.
/// @param tab The tab.
/// @param focus Whether the tab is in front.
/// @param paneRecords Reads one pane's records.
[[nodiscard]] inline ProgramStatusBadge badgeForTab(vtworkspace::Tab const& tab,
                                                    TabFocus focus,
                                                    PaneProgramStatus const& paneRecords)
{
    auto records = std::vector<vtbackend::ProgramStatusRecord> {};
    if (auto* root = tab.rootPane())
        root->walkTree([&](vtworkspace::Pane& pane) {
            if (!pane.isLeaf())
                return;
            auto paneOwn = paneRecords(pane.session());
            records.insert(records.end(),
                           std::make_move_iterator(paneOwn.begin()),
                           std::make_move_iterator(paneOwn.end()));
        });
    return badgeFor(records, focus);
}

/// The badges of a window's tabs, each computed once until it is dropped. A badge costs one terminal lock
/// per pane, and the strip asks for it once per role (kind, text, progress) of every row it repaints; with
/// this, a repaint of T tabs of P panes takes T*P locks rather than three times that.
///
/// The owner drops a tab's badge whenever its row changes and every badge when which tab is in front
/// changes, as a badge depends on that too. Not thread-safe: the GUI thread's alone.
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
