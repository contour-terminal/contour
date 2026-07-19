// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <span>
#include <string_view>

namespace contour::config
{

/// Where the GUI tab strip (tab bar) is placed within the window.
/// @note Exposed to QML as an @c int (0 = Top, 1 = Bottom); keep the enumerator order in sync
///       with the literals in @c main.qml if this is ever extended or reordered.
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

/// One tab bar mode, in every vocabulary the application needs it in.
///
/// The mode used to be spelled out in five places — the YAML reader's if-chain, the std::formatter's
/// switch, the settings page's getter, its setter, and its list of combo box options — and a sixth
/// would be needed by every menu that offers it. Each was an independent chance to disagree, and the
/// settings page did: it showed users the raw configuration tokens because that is what it had.
///
/// So the mode is described once, here, and the code interprets it. Adding a mode is adding a row.
template <typename Mode>
struct TabBarModeInfo
{
    Mode mode;
    std::string_view token; ///< The configuration spelling; also what the settings page stores.
    std::string_view label; ///< What a human reads, in a menu or a settings combo box.
};

namespace detail
{
    // inline, so every translation unit that includes this header shares one table rather than
    // getting a private copy the returned span would then point into.
    inline constexpr auto TabBarPositionTable = std::array {
        TabBarModeInfo<TabBarPosition> { TabBarPosition::Top, "Top", "Top" },
        TabBarModeInfo<TabBarPosition> { TabBarPosition::Bottom, "Bottom", "Bottom" },
    };

    // The tokens are the ones already written into every existing contour.yml, so they are fixed;
    // only the labels are ours to choose, and they say what the mode DOES rather than naming its
    // enumerator ("Multiple" told the user nothing about when the tab bar would appear).
    inline constexpr auto TabBarVisibilityTable = std::array {
        TabBarModeInfo<TabBarVisibility> { TabBarVisibility::Always, "Always", "Always Visible" },
        TabBarModeInfo<TabBarVisibility> { TabBarVisibility::Never, "Never", "Hidden" },
        TabBarModeInfo<TabBarVisibility> { TabBarVisibility::Multiple, "Multiple", "Auto-Hide" },
    };

    /// ASCII-lowercases @p ch.
    ///
    /// Spelled out rather than reached for in crispy, so this header stays free of dependencies: it
    /// is included by Actions.h, which cannot include Config.h (Config.h includes Actions.h).
    [[nodiscard]] constexpr char toLowerAscii(char ch) noexcept
    {
        return ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch - 'A' + 'a') : ch;
    }

    /// Whether @p lhs and @p rhs are equal, ignoring ASCII case.
    [[nodiscard]] constexpr bool equalsIgnoreCase(std::string_view lhs, std::string_view rhs) noexcept
    {
        if (lhs.size() != rhs.size())
            return false;
        for (auto const i: std::views::iota(size_t { 0 }, lhs.size()))
            if (toLowerAscii(lhs[i]) != toLowerAscii(rhs[i]))
                return false;
        return true;
    }
} // namespace detail

/// Every tab bar mode of type @p Mode, in enumerator order.
template <typename Mode>
[[nodiscard]] constexpr std::span<TabBarModeInfo<Mode> const> tabBarModes() noexcept;

template <>
constexpr std::span<TabBarModeInfo<TabBarPosition> const> tabBarModes() noexcept
{
    return detail::TabBarPositionTable;
}

template <>
constexpr std::span<TabBarModeInfo<TabBarVisibility> const> tabBarModes() noexcept
{
    return detail::TabBarVisibilityTable;
}

/// The mode @p token names, ignoring case.
///
/// @param token A configuration spelling, e.g. "multiple".
/// @return The mode, or nullopt when no mode carries that token.
template <typename Mode>
[[nodiscard]] constexpr std::optional<Mode> tabBarModeFromToken(std::string_view token) noexcept
{
    for (auto const& info: tabBarModes<Mode>())
        if (detail::equalsIgnoreCase(info.token, token))
            return info.mode;
    return std::nullopt;
}

/// The configuration spelling of @p mode.
template <typename Mode>
[[nodiscard]] constexpr std::string_view tabBarModeToken(Mode mode) noexcept
{
    for (auto const& info: tabBarModes<Mode>())
        if (info.mode == mode)
            return info.token;
    return {};
}

/// What a human should read for @p mode.
template <typename Mode>
[[nodiscard]] constexpr std::string_view tabBarModeLabel(Mode mode) noexcept
{
    for (auto const& info: tabBarModes<Mode>())
        if (info.mode == mode)
            return info.label;
    return {};
}

} // namespace contour::config
