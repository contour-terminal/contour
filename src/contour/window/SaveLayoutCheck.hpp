// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>

namespace contour::window
{

/// What a typed layout name is, against the layouts already saved.
enum class LayoutNameState : uint8_t
{
    Empty = 0, ///< Nothing typed, or only white space.
    Available, ///< No layout has it.
    Taken,     ///< A layout has it already.
};

/// Whether saving under a taken name may replace the layout that has it.
enum class LayoutReplace : uint8_t
{
    Keep = 0, ///< It may not: a taken name is refused.
    Replace,  ///< It may: the user confirmed, or a configured SaveLayout names it on purpose.
};

/// What Save Layout's accept button offers.
enum class SaveLayoutButton : uint8_t
{
    Disabled = 0, ///< Nothing: no name, or a taken one not yet confirmed.
    Save,         ///< Save under a new name.
    Replace,      ///< Replace the layout that has the name.
};

/// What Save Layout makes of a typed name.
struct SaveLayoutCheck
{
    LayoutNameState name = LayoutNameState::Empty;
    SaveLayoutButton button = SaveLayoutButton::Disabled;

    constexpr bool operator==(SaveLayoutCheck const&) const noexcept = default;
};

/// What Save Layout makes of @p trimmed: empty disables Save; a name no layout has is saved; a taken one is
/// offered as a replacement only once the user confirmed it. Names compare exactly, case included, as the
/// layouts' keys do.
/// @pre @p trimmed is already trimmed.
/// @param trimmed The typed name, trimmed.
/// @param existing The names of the layouts saved.
/// @param replace Whether the user confirmed replacing a taken name.
/// @return The check.
[[nodiscard]] inline SaveLayoutCheck checkLayoutName(std::string_view trimmed,
                                                     std::span<std::string const> existing,
                                                     LayoutReplace replace)
{
    if (trimmed.empty())
        return { .name = LayoutNameState::Empty, .button = SaveLayoutButton::Disabled };
    if (std::ranges::find(existing, trimmed) == existing.end())
        return { .name = LayoutNameState::Available, .button = SaveLayoutButton::Save };
    return { .name = LayoutNameState::Taken,
             .button =
                 replace == LayoutReplace::Replace ? SaveLayoutButton::Replace : SaveLayoutButton::Disabled };
}

/// The name QML reads a layout name's state by: "empty", "available" or "taken".
[[nodiscard]] constexpr std::string_view layoutNameStateName(LayoutNameState state) noexcept
{
    switch (state)
    {
        case LayoutNameState::Empty: return "empty";
        case LayoutNameState::Available: return "available";
        case LayoutNameState::Taken: return "taken";
    }
    return "empty";
}

/// The name QML reads Save Layout's button by: "disabled", "save" or "replace".
[[nodiscard]] constexpr std::string_view saveLayoutButtonName(SaveLayoutButton button) noexcept
{
    switch (button)
    {
        case SaveLayoutButton::Disabled: return "disabled";
        case SaveLayoutButton::Save: return "save";
        case SaveLayoutButton::Replace: return "replace";
    }
    return "disabled";
}

/// What Save Layout says it saves: "3 tabs · 5 panes · working directories and commands".
/// @param tabs How many tabs the window has.
/// @param panes How many panes they hold.
[[nodiscard]] inline std::string layoutSummaryText(int tabs, int panes)
{
    return std::format("{} tab{} · {} pane{} · working directories and commands",
                       tabs,
                       tabs == 1 ? "" : "s",
                       panes,
                       panes == 1 ? "" : "s");
}

} // namespace contour::window
