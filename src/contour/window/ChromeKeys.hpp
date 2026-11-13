// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/ChromeFocus.hpp>
#include <contour/window/ProgramName.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace contour::window
{

/// The keys the chrome answers while it has focus, as the window reports them (not Qt's numbering).
enum class ChromeKey : uint8_t
{
    Other = 0,
    Left,
    Right,
    Up,
    Down,
    Home,
    End,
    Return,
    Space,
    Backspace,
    Delete,
    Escape,
    Tab, ///< Tab and, with Shift, Shift+Tab (Qt's Backtab).
    F2,
    F10,       ///< F10; with Shift, the context-menu chord of Windows and KDE.
    Menu,      ///< The Menu (application) key.
    Printable, ///< A key that types a character (other than Space), with no modifier or Shift alone.
};

/// The modifier state a chrome key arrives with.
enum class ChromeKeyModifier : uint8_t
{
    None = 0,
    Alt,   ///< Alt (Option on macOS) alone.
    Shift, ///< Shift alone.
    Other, ///< Any other modifier combination: the chrome leaves such keys alone.
};

/// What a key does to the focused chrome area.
enum class ChromeCommand : uint8_t
{
    None = 0,
    FocusPrevious, ///< Moves focus to the previous item; no wrap.
    FocusNext,     ///< Moves focus to the next item; no wrap.
    FocusFirst,    ///< Moves focus to the first item.
    FocusLast,     ///< Moves focus to the last item.
    Activate,      ///< Activates (tab) or presses (toolbar item) the focused item.
    Close,         ///< Closes the focused tab.
    MoveLeft,      ///< Moves the focused tab one place left.
    MoveRight,     ///< Moves the focused tab one place right.
    Leave,         ///< Returns focus to the terminal.
    Collapse,      ///< Collapses the focused tree row, or moves to its parent when it is collapsed.
    Expand,        ///< Expands the focused tree row, or moves to its first child when it is expanded.
    Rename,        ///< Starts renaming the focused row inline.
    Filter,        ///< Starts (or extends) the tree's type-to-filter with the typed character.
    ClearFilter,   ///< Clears the tree's type-to-filter; the keyboard stays in the tree.
    OpenMenu,      ///< Opens the focused row's context menu, as a right click on it does.
};

/// Whether the focused chrome area answered a key's command, or left the key to whoever is next.
enum class ChromeKeyOutcome : uint8_t
{
    Unhandled = 0, ///< The area has nothing to do with it.
    Handled,       ///< The area carried it out (or, for the sidebar's own commands, handed it to the tree).
};

/// A command the focused sidebar tree carries out itself, by the name its QML handles it by: what
/// WindowController.sidebarCommands publishes, and sidebarCommandRequested() carries.
struct NamedChromeCommand
{
    std::string_view name; ///< The QML name.
    ChromeCommand command; ///< The command.
};

/// The sidebar tree's own commands, by name.
inline constexpr auto SidebarCommandNames = std::array {
    NamedChromeCommand { .name = "previous", .command = ChromeCommand::FocusPrevious },
    NamedChromeCommand { .name = "next", .command = ChromeCommand::FocusNext },
    NamedChromeCommand { .name = "first", .command = ChromeCommand::FocusFirst },
    NamedChromeCommand { .name = "last", .command = ChromeCommand::FocusLast },
    NamedChromeCommand { .name = "activate", .command = ChromeCommand::Activate },
    NamedChromeCommand { .name = "collapse", .command = ChromeCommand::Collapse },
    NamedChromeCommand { .name = "expand", .command = ChromeCommand::Expand },
    NamedChromeCommand { .name = "rename", .command = ChromeCommand::Rename },
    NamedChromeCommand { .name = "filter", .command = ChromeCommand::Filter },
    NamedChromeCommand { .name = "clearFilter", .command = ChromeCommand::ClearFilter },
    NamedChromeCommand { .name = "openMenu", .command = ChromeCommand::OpenMenu },
};

/// One key of one chrome area.
struct ChromeKeyRow
{
    ChromeArea area;            ///< The area the key is answered in.
    ChromeKey key;              ///< The key.
    ChromeKeyModifier modifier; ///< The modifier it must arrive with.
    ChromeCommand command;      ///< What it does.
};

/// The keys of each chrome area. The terminal has no rows: every key there goes to the shell.
inline constexpr std::array ChromeKeyRows {
    ChromeKeyRow {
        ChromeArea::Sidebar, ChromeKey::Up, ChromeKeyModifier::None, ChromeCommand::FocusPrevious },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Down, ChromeKeyModifier::None, ChromeCommand::FocusNext },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Left, ChromeKeyModifier::None, ChromeCommand::Collapse },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Right, ChromeKeyModifier::None, ChromeCommand::Expand },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Home, ChromeKeyModifier::None, ChromeCommand::FocusFirst },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::End, ChromeKeyModifier::None, ChromeCommand::FocusLast },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Return, ChromeKeyModifier::None, ChromeCommand::Activate },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Escape, ChromeKeyModifier::None, ChromeCommand::Leave },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::F2, ChromeKeyModifier::None, ChromeCommand::Rename },
    // A row's context menu without a pointer: the Menu key, and Shift+F10 (Windows and KDE).
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Menu, ChromeKeyModifier::None, ChromeCommand::OpenMenu },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::F10, ChromeKeyModifier::Shift, ChromeCommand::OpenMenu },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Space, ChromeKeyModifier::None, ChromeCommand::Filter },
    ChromeKeyRow { ChromeArea::Sidebar, ChromeKey::Space, ChromeKeyModifier::Shift, ChromeCommand::Filter },
    ChromeKeyRow {
        ChromeArea::Sidebar, ChromeKey::Printable, ChromeKeyModifier::None, ChromeCommand::Filter },
    ChromeKeyRow {
        ChromeArea::Sidebar, ChromeKey::Printable, ChromeKeyModifier::Shift, ChromeCommand::Filter },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Left, ChromeKeyModifier::None, ChromeCommand::FocusPrevious },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Right, ChromeKeyModifier::None, ChromeCommand::FocusNext },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Home, ChromeKeyModifier::None, ChromeCommand::FocusFirst },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::End, ChromeKeyModifier::None, ChromeCommand::FocusLast },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Return, ChromeKeyModifier::None, ChromeCommand::Activate },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Space, ChromeKeyModifier::None, ChromeCommand::Activate },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Backspace, ChromeKeyModifier::None, ChromeCommand::Close },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Delete, ChromeKeyModifier::None, ChromeCommand::Close },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Left, ChromeKeyModifier::Alt, ChromeCommand::MoveLeft },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Right, ChromeKeyModifier::Alt, ChromeCommand::MoveRight },
    ChromeKeyRow { ChromeArea::Tabs, ChromeKey::Escape, ChromeKeyModifier::None, ChromeCommand::Leave },
    ChromeKeyRow {
        ChromeArea::Toolbar, ChromeKey::Left, ChromeKeyModifier::None, ChromeCommand::FocusPrevious },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::Right, ChromeKeyModifier::None, ChromeCommand::FocusNext },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::Home, ChromeKeyModifier::None, ChromeCommand::FocusFirst },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::End, ChromeKeyModifier::None, ChromeCommand::FocusLast },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::Return, ChromeKeyModifier::None, ChromeCommand::Activate },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::Space, ChromeKeyModifier::None, ChromeCommand::Activate },
    ChromeKeyRow { ChromeArea::Toolbar, ChromeKey::Escape, ChromeKeyModifier::None, ChromeCommand::Leave },
};

/// The keys the chrome takes for itself while it has focus, whether or not its area has a row for them:
/// they are never an application binding there. Every other chord (the command palette, a new tab, a user
/// binding) keeps working from inside the chrome.
inline constexpr std::array ChromeConsumedKeys { ChromeKey::Left,   ChromeKey::Right,  ChromeKey::Up,
                                                 ChromeKey::Down,   ChromeKey::Home,   ChromeKey::End,
                                                 ChromeKey::Return, ChromeKey::Space,  ChromeKey::Backspace,
                                                 ChromeKey::Delete, ChromeKey::Escape, ChromeKey::Tab };

/// Whether the chrome takes a key for itself.
/// @param key The key.
/// @param modifier The modifier state it arrived with.
/// @return True for a key of ChromeConsumedKeys with no modifier, or with Alt or Shift alone; a chord with
///         Ctrl, Meta or more than one modifier is the application's.
[[nodiscard]] constexpr bool chromeConsumes(ChromeKey key, ChromeKeyModifier modifier) noexcept
{
    return modifier != ChromeKeyModifier::Other && std::ranges::contains(ChromeConsumedKeys, key);
}

/// What @p key does in @p area.
/// @param area The focused chrome area.
/// @param key The key.
/// @param modifier The modifier state it arrived with.
/// @return The row's command, or ChromeCommand::None when the area has no such key.
[[nodiscard]] constexpr ChromeCommand chromeCommandFor(ChromeArea area,
                                                       ChromeKey key,
                                                       ChromeKeyModifier modifier) noexcept
{
    auto const row = std::ranges::find_if(ChromeKeyRows, [&](ChromeKeyRow const& candidate) {
        return candidate.area == area && candidate.key == key && candidate.modifier == modifier;
    });
    return row != ChromeKeyRows.end() ? row->command : ChromeCommand::None;
}

/// Whether the focused @p area takes a key for itself: a key every area takes (@see ChromeConsumedKeys), or
/// one this area has a row for, such as the sidebar's F2 and typed characters, which stay the application's
/// in the other areas.
/// @param area The focused chrome area.
/// @param key The key.
/// @param modifier The modifier state it arrived with.
/// @return True when the chrome handles the key.
[[nodiscard]] constexpr bool chromeConsumes(ChromeArea area,
                                            ChromeKey key,
                                            ChromeKeyModifier modifier) noexcept
{
    return chromeConsumes(key, modifier)
           || (modifier != ChromeKeyModifier::Other
               && chromeCommandFor(area, key, modifier) != ChromeCommand::None);
}

/// The index focus lands on after a focus-moving command.
/// @param command The command; anything but the four focus commands leaves the index alone.
/// @param focused The focused item's index.
/// @param count How many items the area holds.
/// @return The new index, clamped to [0, count - 1] (no wrap); @p focused when @p count is not positive.
[[nodiscard]] constexpr int focusedIndexAfter(ChromeCommand command, int focused, int count) noexcept
{
    if (count <= 0)
        return focused;
    auto const last = count - 1;
    switch (command)
    {
        case ChromeCommand::FocusPrevious: return std::clamp(focused - 1, 0, last);
        case ChromeCommand::FocusNext: return std::clamp(focused + 1, 0, last);
        case ChromeCommand::FocusFirst: return 0;
        case ChromeCommand::FocusLast: return last;
        case ChromeCommand::None:
        case ChromeCommand::Activate:
        case ChromeCommand::Close:
        case ChromeCommand::MoveLeft:
        case ChromeCommand::MoveRight:
        case ChromeCommand::Leave:
        case ChromeCommand::Collapse:
        case ChromeCommand::Expand:
        case ChromeCommand::Rename:
        case ChromeCommand::Filter:
        case ChromeCommand::ClearFilter:
        case ChromeCommand::OpenMenu: break;
    }
    return std::clamp(focused, 0, last);
}

/// Whether a pane has a program running in front of its shell.
enum class PaneActivity : uint8_t
{
    Idle = 0, ///< The foreground process is the pane's shell.
    Running,  ///< A program other than the shell is in the foreground.
    Unknown,  ///< No answer (a remote pane, Windows): cannot be told idle, so closing asks.
};

/// Classifies a pane from its foreground process.
/// @param shellProgram The profile's shell program: a path or a name, possibly with arguments ("bash -l").
/// @param foreground The pane's foreground process name; nullopt when it cannot be known.
/// @return Unknown without a name, Idle when it is the shell (base name, login '-' removed), else Running.
[[nodiscard]] inline PaneActivity paneActivityOf(std::string_view shellProgram,
                                                 std::optional<std::string> const& foreground)
{
    if (!foreground)
        return PaneActivity::Unknown;
    auto const name = programBaseName(*foreground);
    // The whole string first: a path may hold a space ("C:/Program Files/Git/bin/bash.exe"). Then its first
    // word, for a program written with its arguments.
    auto const program = shellProgram.substr(0, shellProgram.find(' '));
    return name == programBaseName(shellProgram) || name == programBaseName(program) ? PaneActivity::Idle
                                                                                     : PaneActivity::Running;
}

/// Whether closing a tab should ask first.
enum class CloseConfirmation : uint8_t
{
    None = 0, ///< Close at once.
    Ask,      ///< Ask: a program is running, or none can be ruled out.
};

/// Whether closing a tab holding @p panes should ask.
/// @param panes The activity of every pane of the tab.
/// @return None only when every pane is known to be Idle; Ask otherwise. A pane whose program cannot be
///         known (every pane under daemon hosting) fails toward keeping work: it may be running vim.
[[nodiscard]] constexpr CloseConfirmation closeConfirmationFor(std::span<PaneActivity const> panes) noexcept
{
    return std::ranges::all_of(panes, [](PaneActivity activity) { return activity == PaneActivity::Idle; })
               ? CloseConfirmation::None
               : CloseConfirmation::Ask;
}

} // namespace contour::window
