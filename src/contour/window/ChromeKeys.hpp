// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/ChromeFocus.hpp>

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
    Home,
    End,
    Return,
    Space,
    Backspace,
    Delete,
    Escape,
};

/// The modifier state a chrome key arrives with.
enum class ChromeKeyModifier : uint8_t
{
    None = 0,
    Alt,   ///< Alt (Option on macOS) alone.
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
        case ChromeCommand::Leave: break;
    }
    return std::clamp(focused, 0, last);
}

/// Whether a pane has a program running in front of its shell.
enum class PaneActivity : uint8_t
{
    Idle = 0, ///< The foreground process is the pane's shell.
    Running,  ///< A program other than the shell is in the foreground.
    Unknown,  ///< No answer (a remote pane, Windows): never worth asking about.
};

namespace detail
{
    /// The part of @p path after its last separator, minus a leading '-' (a login shell's argv[0]).
    [[nodiscard]] inline std::string_view programBaseName(std::string_view path) noexcept
    {
        if (auto const slash = path.find_last_of("/\\"); slash != std::string_view::npos)
            path.remove_prefix(slash + 1);
        if (path.starts_with('-'))
            path.remove_prefix(1);
        return path;
    }
} // namespace detail

/// Classifies a pane from its foreground process.
/// @param shellProgram The profile's shell program (a path or a name).
/// @param foreground The pane's foreground process name; nullopt when it cannot be known.
/// @return Unknown without a name, Idle when it is the shell (base name, login '-' removed), else Running.
[[nodiscard]] inline PaneActivity paneActivityOf(std::string_view shellProgram,
                                                 std::optional<std::string> const& foreground)
{
    if (!foreground)
        return PaneActivity::Unknown;
    return detail::programBaseName(*foreground) == detail::programBaseName(shellProgram)
               ? PaneActivity::Idle
               : PaneActivity::Running;
}

/// Whether closing a tab should ask first.
enum class CloseConfirmation : uint8_t
{
    None = 0, ///< Close at once.
    Ask,      ///< Ask: a program is still running.
};

/// Whether closing a tab holding @p panes should ask.
/// @param panes The activity of every pane of the tab.
/// @return Ask when any pane is Running; None otherwise (Unknown does not ask).
[[nodiscard]] constexpr CloseConfirmation closeConfirmationFor(std::span<PaneActivity const> panes) noexcept
{
    return std::ranges::contains(panes, PaneActivity::Running) ? CloseConfirmation::Ask
                                                               : CloseConfirmation::None;
}

} // namespace contour::window
