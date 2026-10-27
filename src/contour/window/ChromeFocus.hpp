// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <ranges>

namespace contour::window
{

/// An area of a window that keyboard focus can be in, other than a pane's own children.
enum class ChromeArea : uint8_t
{
    Terminal = 0, ///< The terminal: no chrome has focus.
    Tabs,         ///< The tab well (the pills, or the flat strip).
    Toolbar,      ///< The band's toolbar items, in visual order.
};

/// The number of ChromeArea values; derived from the last one.
inline constexpr auto ChromeAreaCount = static_cast<std::size_t>(ChromeArea::Toolbar) + 1;

/// Whether an area is shown right now.
enum class AreaPresence : uint8_t
{
    Absent = 0,
    Present,
};

/// Which chrome areas the window shows. The terminal is always there, so it has no member.
struct ChromeAreaPresence
{
    AreaPresence tabs = AreaPresence::Absent;    ///< Whether the tab well is shown.
    AreaPresence toolbar = AreaPresence::Absent; ///< Whether the band has at least one toolbar item.

    bool operator==(ChromeAreaPresence const&) const = default;
};

/// The order Ctrl+F6 walks the areas in; it wraps around.
inline constexpr std::array ChromeAreaCycle { ChromeArea::Terminal, ChromeArea::Tabs, ChromeArea::Toolbar };

static_assert(ChromeAreaCycle.size() == ChromeAreaCount, "the cycle must name every area exactly once");

/// A request to move focus between areas.
enum class ChromeFocusAction : uint8_t
{
    Next = 0, ///< The next area in ChromeAreaCycle.
    Previous, ///< The previous area in ChromeAreaCycle.
    Toolbar,  ///< The toolbar, wherever focus is now.
};

/// Whether @p area is shown.
/// @param area The area to ask about.
/// @param presence What the window shows.
/// @return True for the terminal, and for a chrome area that is present.
[[nodiscard]] constexpr bool isPresent(ChromeArea area, ChromeAreaPresence presence) noexcept
{
    switch (area)
    {
        case ChromeArea::Terminal: return true;
        case ChromeArea::Tabs: return presence.tabs == AreaPresence::Present;
        case ChromeArea::Toolbar: return presence.toolbar == AreaPresence::Present;
    }
    return false;
}

/// Where focus goes for @p action.
///
/// Next and Previous walk ChromeAreaCycle and skip areas that are absent, so focus is never sent to
/// chrome that is not there; the terminal is always present, so the walk always ends.
///
/// @param action What was asked for.
/// @param current The area focus is in now.
/// @param presence What the window shows.
/// @return The area to focus: always a present one. Toolbar stays at @p current when there is no toolbar.
[[nodiscard]] constexpr ChromeArea chromeFocusTarget(ChromeFocusAction action,
                                                     ChromeArea current,
                                                     ChromeAreaPresence presence) noexcept
{
    if (action == ChromeFocusAction::Toolbar)
        return isPresent(ChromeArea::Toolbar, presence) ? ChromeArea::Toolbar : current;

    auto const position =
        static_cast<std::size_t>(std::ranges::find(ChromeAreaCycle, current) - ChromeAreaCycle.begin());
    for (auto const step: std::views::iota(std::size_t { 1 }, ChromeAreaCount + 1))
    {
        auto const offset = action == ChromeFocusAction::Next ? step : ChromeAreaCount - step;
        auto const candidate = ChromeAreaCycle[(position + offset) % ChromeAreaCount];
        if (isPresent(candidate, presence))
            return candidate;
    }
    return ChromeArea::Terminal;
}

} // namespace contour::window
