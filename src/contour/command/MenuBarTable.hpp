// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/command/ContextMenu.hpp>
#include <contour/config/Config.hpp>
#include <contour/config/TabBarMode.hpp>
#include <contour/config/WindowControlStyle.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace contour::command
{

/// The menus of the menu bar, left to right.
enum class MenuBarMenu : std::uint8_t
{
    Application = 0, ///< "Contour": About, Settings, Quit.
    Shell,
    Edit,
    View,
    Window,
    Help,
};

/// How many menus the menu bar has.
inline constexpr std::size_t MenuBarMenuCount = static_cast<std::size_t>(MenuBarMenu::Help) + 1;

/// Whether a host shows a menu bar of the application, apart from its windows.
enum class MenuBarPlacement : std::uint8_t
{
    None = 0, ///< No menu bar of its own: the windows carry the menus.
    Global,   ///< One bar for the whole application, at the top of the screen.
};

/// The menu bar placement @p host's conventions call for.
/// @param host The host the build runs on.
/// @return Where its menu bar lives.
[[nodiscard]] constexpr MenuBarPlacement menuBarPlacementFor(config::HostPlatform host) noexcept
{
    switch (host)
    {
        case config::HostPlatform::MacOS: return MenuBarPlacement::Global;
        case config::HostPlatform::Windows:
        case config::HostPlatform::KdePlasma:
        case config::HostPlatform::Other: return MenuBarPlacement::None;
    }
    return MenuBarPlacement::None;
}

/// Whether a terminal window is focused for the menu bar to act on.
enum class WindowPresence : std::uint8_t
{
    Absent = 0, ///< No window: only application-level rows can run.
    Present,    ///< A window is key.
};

/// Whether the focused pane has a selection.
enum class SelectionPresence : std::uint8_t
{
    Absent = 0, ///< Nothing is selected.
    Present,    ///< Some cells are selected.
};

/// Whether this build and machine can speak.
enum class SpeechAvailability : std::uint8_t
{
    Unavailable = 0, ///< No speech engine or voice.
    Available,       ///< Speech is available.
};

/// Whether the focused pane's read-only lock is on.
enum class InputProtection : std::uint8_t
{
    Off = 0, ///< Input reaches the application.
    On,      ///< The user's read-only lock holds.
};

/// The world, as the menu bar needs to see it.
struct MenuBarState
{
    WindowPresence window = WindowPresence::Absent;              ///< Whether a window is key.
    SelectionPresence selection = SelectionPresence::Absent;     ///< Whether the pane has a selection.
    SpeechAvailability speech = SpeechAvailability::Unavailable; ///< Whether speech is available.
    InputProtection inputProtection = InputProtection::Off;      ///< The pane's read-only lock.
    int tabCount = 0;                                            ///< Tabs in the window.
    int activeTab = -1;                                          ///< The active tab's index, or -1.
    int paneCount = 0;                                           ///< Panes in the active tab.
    std::vector<std::string> tabTitles {};                       ///< One per tab, in order.
    config::TabBarVisibility tabBarVisibility = config::TabBarVisibility::Always; ///< Mode in force.
    config::TabBarPosition tabBarPosition = config::TabBarPosition::Top;          ///< Placement in force.
    std::string activeProfile {};             ///< The active pane's profile.
    std::vector<std::string> profileNames {}; ///< Configured profiles, sorted.
    std::vector<std::string> hostNames {};    ///< Saved hosts, sorted.
    std::vector<std::string> layoutNames {};  ///< Saved layouts, sorted.
};

/// The part of the state that the configuration decides: profiles, hosts and layouts, each sorted.
/// The window fields keep their defaults.
/// @param config The configuration.
/// @return The state to fill in with the window's facts.
[[nodiscard]] MenuBarState menuBarStateOf(config::Config const& config);

/// The menu bar for @p state.
/// @param state The world the bar is drawn over.
/// @return Exactly MenuBarMenuCount submenu entries, in MenuBarMenu order.
[[nodiscard]] std::vector<ContextMenuEntry> buildMenuBar(MenuBarState const& state);

/// An action no menu offers, and why.
struct MenuReachabilityExemption
{
    std::string_view action; ///< The action's catalogue name.
    std::string_view reason; ///< Why no menu row fits it.
};

/// The catalogue actions that appear in no menu; every other one is reachable from a menu.
inline constexpr std::array MenuReachabilityExemptions {
    MenuReachabilityExemption { .action = "CreateSelection", .reason = "needs user-written text" },
    MenuReachabilityExemption { .action = "SendChars", .reason = "needs user-written text" },
    MenuReachabilityExemption { .action = "WriteScreen", .reason = "needs user-written text" },
    MenuReachabilityExemption { .action = "MoveTabTo",
                                .reason = "the drag and Move Tab Left/Right cover it" },
    MenuReachabilityExemption { .action = "OpenContextMenu",
                                .reason = "it opens the menu it would be a row of" },
};

} // namespace contour::command
