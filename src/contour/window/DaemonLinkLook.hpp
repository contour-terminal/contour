// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one vocabulary for where a window's link to a daemon stands, and how each state looks: its dot, its
/// mark, its words. The location chip, the switcher and the sidebar's daemon rows all read this table, and
/// no mark here reuses a tab-status glyph (TabBadge.hpp), so a mark means one thing wherever it is drawn.

#include <contour/config/ChromeTokenGroups.hpp>
#include <contour/config/UiStyle.hpp>
#include <contour/window/TranslateNoop.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <string_view>

namespace contour::window
{

/// Where a window's link to one daemon stands, as the sidebar and the chip report it.
enum class DaemonLinkState : uint8_t
{
    NotConnected = 0, ///< Nothing links to the daemon, and nothing is being tried.
    Connecting,       ///< A first connection is being made.
    Connected,        ///< The daemon is reachable.
    Reconnecting,     ///< A lost connection is being made again.
    NeedsInput,       ///< Connecting waits for the user: a password, a host key, an exposed window.
    NotRunning,       ///< Discovery found no daemon serving: it can be started.
    TooOld,           ///< The daemon speaks a protocol too old for this client.
    Failed,           ///< Connecting failed, and nothing is retried.
};

/// Number of @c DaemonLinkState enumerators, derived from the last one.
inline constexpr size_t DaemonLinkStateCount = static_cast<size_t>(DaemonLinkState::Failed) + 1;

/// How one link state looks and what it says.
struct LinkLook
{
    DaemonLinkState state;            ///< The state this row describes.
    config::StatusColorRole dot;      ///< The dot's status role.
    std::string_view glyph;           ///< The mark drawn where a dot cannot be: one cell on the Terminal row.
    std::string_view words;           ///< What the row says of the state (English source text).
    std::string_view accessibleWords; ///< The state words assistive technology announces.
};

/// The looks, indexed by @c DaemonLinkState. Their glyphs are the Terminal row's; @c DaemonLinkGlyphRows
/// overrides them per row.
inline constexpr std::array<LinkLook, DaemonLinkStateCount> DaemonLinkLooks {
    LinkLook { .state = DaemonLinkState::NotConnected,
               .dot = config::StatusColorRole::Warning,
               .glyph = "○",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Not connected"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "not connected") },
    LinkLook { .state = DaemonLinkState::Connecting,
               .dot = config::StatusColorRole::Running,
               .glyph = "◌",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Connecting…"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "connecting") },
    LinkLook { .state = DaemonLinkState::Connected,
               .dot = config::StatusColorRole::Success,
               .glyph = "●",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Connected"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "connected") },
    LinkLook { .state = DaemonLinkState::Reconnecting,
               .dot = config::StatusColorRole::Warning,
               .glyph = "↻",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Reconnecting…"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "reconnecting") },
    LinkLook { .state = DaemonLinkState::NeedsInput,
               .dot = config::StatusColorRole::Warning,
               .glyph = "?",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Needs you"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "needs you") },
    LinkLook { .state = DaemonLinkState::NotRunning,
               .dot = config::StatusColorRole::Warning,
               .glyph = "◇",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Not running"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "not running") },
    LinkLook { .state = DaemonLinkState::TooOld,
               .dot = config::StatusColorRole::Danger,
               .glyph = "⊘",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Daemon too old"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "daemon too old") },
    LinkLook { .state = DaemonLinkState::Failed,
               .dot = config::StatusColorRole::Danger,
               .glyph = "⨯",
               .words = QT_TRANSLATE_NOOP("DaemonLink", "Failed"),
               .accessibleWords = QT_TRANSLATE_NOOP("DaemonLink", "failed") },
};

/// One mark a link state draws on one chrome row, where it differs from the state's own @c LinkLook::glyph.
struct DaemonLinkGlyphRow
{
    DaemonLinkState state;    ///< The link state.
    config::SidebarLook look; ///< The chrome row (the sidebar look is the row's name).
    std::string_view glyph;   ///< What the row draws for the state.
};

/// The per-row marks. Every row draws the state's own single-cell mark today; a row that wants another adds
/// it here, and the distinctness test then holds it apart from that row's tab-status glyphs.
inline constexpr auto DaemonLinkGlyphRows = std::array<DaemonLinkGlyphRow, 0> {};

namespace detail
{
    [[nodiscard]] constexpr bool daemonLinkLooksInOrder() noexcept
    {
        for (auto const i: std::views::iota(size_t { 0 }, DaemonLinkLooks.size()))
            if (DaemonLinkLooks[i].state != DaemonLinkState(i))
                return false;
        return true;
    }
} // namespace detail

static_assert(detail::daemonLinkLooksInOrder(), "DaemonLinkLooks must list the states in enumerator order");

/// The look of a link state on a chrome row.
/// @param state The link state.
/// @param look  The chrome row, as its sidebar look.
/// @return The state's row in @c DaemonLinkLooks, its glyph overridden from @c DaemonLinkGlyphRows.
[[nodiscard]] constexpr LinkLook linkLookOf(DaemonLinkState state, config::SidebarLook look) noexcept
{
    auto result = DaemonLinkLooks[static_cast<size_t>(state)];
    auto const row = std::ranges::find_if(DaemonLinkGlyphRows, [&](DaemonLinkGlyphRow const& candidate) {
        return candidate.state == state && candidate.look == look;
    });
    if (row != DaemonLinkGlyphRows.end())
        result.glyph = row->glyph;
    return result;
}

/// Merges what a live link reports with what discovery found: a link that is up, or being brought up,
/// knows better than a scan that found no daemon serving (it may be a remote one, or one mid-upgrade).
/// @param live       The window's link state.
/// @param discovered What discovery reports of the same daemon.
/// @return @p discovered when it is NotRunning and the link is neither Connected, Reconnecting nor
///         Connecting; otherwise @p live.
[[nodiscard]] constexpr DaemonLinkState precedenceOf(DaemonLinkState live,
                                                     DaemonLinkState discovered) noexcept
{
    auto const linkIsLive = live == DaemonLinkState::Connected || live == DaemonLinkState::Reconnecting
                            || live == DaemonLinkState::Connecting;
    if (discovered == DaemonLinkState::NotRunning && !linkIsLive)
        return discovered;
    return live;
}

} // namespace contour::window
