// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/config/ConfigEnumInfo.hpp>

#include <array>
#include <cstddef>
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

/// Number of @c TabBarPosition enumerators, derived from the last one.
inline constexpr size_t TabBarPositionCount = static_cast<size_t>(TabBarPosition::Bottom) + 1;

/// Where a window's tabs live: in the title band, or as the workspace sidebar's rows.
enum class TabPlacement : uint8_t
{
    Top = 0, //!< The tab strip (see @c TabBarPosition) holds them: the historical layout.
    Sidebar, //!< The workspace sidebar lists them under their workspace.
};

/// Number of @c TabPlacement enumerators, derived from the last one.
inline constexpr size_t TabPlacementCount = static_cast<size_t>(TabPlacement::Sidebar) + 1;

/// Whether the workspace sidebar is on screen.
enum class SidebarVisibility : uint8_t
{
    Hidden = 0, //!< Not shown: it takes no width.
    Shown,      //!< Shown at the window's left edge.
};

/// Number of @c SidebarVisibility enumerators, derived from the last one.
inline constexpr size_t SidebarVisibilityCount = static_cast<size_t>(SidebarVisibility::Shown) + 1;

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

    inline constexpr auto TabPlacementTable = std::array {
        ConfigEnumInfo<TabPlacement> { TabPlacement::Top, "Top", "Tab Strip" },
        ConfigEnumInfo<TabPlacement> { TabPlacement::Sidebar, "Sidebar", "Workspace Sidebar" },
    };

    // The tokens the GUI state file spells the sidebar's last-used visibility by.
    inline constexpr auto SidebarVisibilityTable = std::array {
        ConfigEnumInfo<SidebarVisibility> { SidebarVisibility::Hidden, "hidden", "Hidden" },
        ConfigEnumInfo<SidebarVisibility> { SidebarVisibility::Shown, "shown", "Shown" },
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

/// Whether the tab strip is shown under @p visibility.
/// @param visibility The configured mode.
/// @param tabCount How many tabs the window holds.
/// @return True when the strip is shown.
[[nodiscard]] constexpr bool isTabBarShown(TabBarVisibility visibility, int tabCount) noexcept
{
    switch (visibility)
    {
        case TabBarVisibility::Always: return true;
        case TabBarVisibility::Never: return false;
        case TabBarVisibility::Multiple: return tabCount > 1;
    }
    return true;
}

template <>
constexpr std::span<ConfigEnumInfo<TabBarPosition> const> configEnumValues() noexcept
{
    return detail::TabBarPositionTable;
}

template <>
constexpr std::span<ConfigEnumInfo<TabPlacement> const> configEnumValues() noexcept
{
    return detail::TabPlacementTable;
}

template <>
constexpr std::span<ConfigEnumInfo<SidebarVisibility> const> configEnumValues() noexcept
{
    return detail::SidebarVisibilityTable;
}

template <>
constexpr std::span<ConfigEnumInfo<TabBarVisibility> const> configEnumValues() noexcept
{
    return detail::TabBarVisibilityTable;
}

} // namespace contour::config
