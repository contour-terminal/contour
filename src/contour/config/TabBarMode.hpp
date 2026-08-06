// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnum.hpp>

#include <array>
#include <cstdint>
#include <span>

namespace contour::config
{

/// Where the GUI tab strip (tab bar) is placed within the window.
/// @note Exposed to QML as an @c int (0 = Top, 1 = Bottom); keep the enumerator order in sync
///       with the literals in @c Main.qml if this is ever extended or reordered.
enum class TabBarPosition : uint8_t
{
    Top,    //!< The tab strip sits above the terminal content (default, historical behavior).
    Bottom, //!< The tab strip sits below the terminal content.
};

/// When the GUI tab strip (tab bar) is shown.
enum class TabBarVisibility : uint8_t
{
    Always,   //!< Always show the tab strip (default, historical behavior).
    Never,    //!< Never show the tab strip.
    Multiple, //!< Show the tab strip only when the window has more than one tab.
};

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    inline constexpr auto TabBarPositionTable = std::array {
        ConfigEnumInfo<TabBarPosition> { TabBarPosition::Top, "Top", "Top" },
        ConfigEnumInfo<TabBarPosition> { TabBarPosition::Bottom, "Bottom", "Bottom" },
    };

    // The tokens are the ones already written into every existing contour.yml, so they are fixed;
    // only the labels are ours to choose, and they say what the mode DOES rather than naming its
    // enumerator ("Multiple" told the user nothing about when the tab bar would appear).
    inline constexpr auto TabBarVisibilityTable = std::array {
        ConfigEnumInfo<TabBarVisibility> { TabBarVisibility::Always, "Always", "Always Visible" },
        ConfigEnumInfo<TabBarVisibility> { TabBarVisibility::Never, "Never", "Hidden" },
        ConfigEnumInfo<TabBarVisibility> { TabBarVisibility::Multiple, "Multiple", "Auto-Hide" },
    };
} // namespace detail

template <>
constexpr std::span<ConfigEnumInfo<TabBarPosition> const> configEnumValues() noexcept
{
    return detail::TabBarPositionTable;
}

template <>
constexpr std::span<ConfigEnumInfo<TabBarVisibility> const> configEnumValues() noexcept
{
    return detail::TabBarVisibilityTable;
}

} // namespace contour::config
