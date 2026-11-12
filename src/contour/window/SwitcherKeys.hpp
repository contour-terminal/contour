// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a key does in the switcher (the redesign's spec, §Keys), as one table: the first row whose key,
/// modifier and condition match decides the command. Pure and Qt-free; the controller maps Qt's key event
/// onto SwitcherKey and runs what this returns. A key no row takes goes to the field (it edits the text or
/// moves the caret).

#include <contour/window/SwitcherRows.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace contour::window
{

/// A key the switcher handles itself; every other key is Other.
enum class SwitcherKey : uint8_t
{
    Other = 0,
    Up,
    Down,
    Home,
    End,
    Return, ///< Return or Enter.
    Right,
    Left,
    Backspace,
    Escape,
    Menu, ///< The context-menu key.
    F10,
    J, ///< With Control: the next row, vim-style.
    K, ///< With Control: the previous row, vim-style.
};

/// The modifiers held with a key, as far as the table tells them apart.
enum class SwitcherKeyModifier : uint8_t
{
    None = 0,
    Shift,
    Control, ///< Control alone: Qt's ControlModifier, which is ⌘ on macOS (see HeldModifier).
    Other,   ///< Any other combination (Alt, Meta, two at once): no row of the table takes it.
};

/// The modifier keys held with a key, as bits, named by Qt's meaning (Qt::KeyboardModifiers): Control is
/// Qt's ControlModifier, which on macOS is ⌘ (Qt swaps the two there), and Meta is then the physical Ctrl
/// key. So Ctrl+J on Linux and ⌘J on macOS both move to the next row, as the palette's own keys do.
enum class HeldModifier : uint8_t
{
    None = 0,
    Shift = 0b0001,
    Control = 0b0010,
    Alt = 0b0100,
    Meta = 0b1000,
};

/// @return Both sets of modifiers held.
[[nodiscard]] constexpr HeldModifier operator|(HeldModifier a, HeldModifier b) noexcept
{
    return static_cast<HeldModifier>(static_cast<uint8_t>(a) | static_cast<uint8_t>(b));
}

/// @param held The modifiers held with a key.
/// @return How the key table tells them apart: None, Shift alone, Control alone, else Other.
[[nodiscard]] constexpr SwitcherKeyModifier switcherKeyModifierOf(HeldModifier held) noexcept
{
    switch (held)
    {
        case HeldModifier::None: return SwitcherKeyModifier::None;
        case HeldModifier::Shift: return SwitcherKeyModifier::Shift;
        case HeldModifier::Control: return SwitcherKeyModifier::Control;
        default: return SwitcherKeyModifier::Other;
    }
}

/// Whether something is typed in the field.
enum class FilterText : uint8_t
{
    Empty = 0,
    Typed,
};

/// Where the field's caret stands.
enum class CaretAt : uint8_t
{
    Inside = 0,  ///< Between two characters.
    Start,       ///< Before the first character of a non-empty text.
    End,         ///< After the last character of a non-empty text.
    StartAndEnd, ///< In an empty field: at both.
};

/// What the switcher looks like where the key is pressed.
struct SwitcherKeyContext
{
    FilterText filter = FilterText::Empty;           ///< Whether something is typed.
    CaretAt caret = CaretAt::StartAndEnd;            ///< Where the caret stands.
    SwitcherLevel level = SwitcherLevel::Top;        ///< The level shown.
    SwitcherOrigin origin = SwitcherOrigin::Centred; ///< Where the popup opened from.
    SwitcherScope scope = SwitcherScope::Everything; ///< Every daemon, or one, at the top level.
    /// The selected row's kind; none while the list is empty.
    std::optional<SwitcherRowKind> selected = std::nullopt;
};

/// What a key makes the switcher do.
enum class SwitcherCommand : uint8_t
{
    None = 0,            ///< Nothing: the field takes the key.
    Previous,            ///< Select the previous row.
    Next,                ///< Select the next row.
    First,               ///< Select the first row.
    Last,                ///< Select the last row.
    Activate,            ///< Choose the selected row (drill in on a drill row).
    ActivateInNewWindow, ///< Choose the selected row in a new window.
    DrillIn,             ///< Enter the selected drill row's level.
    Back,                ///< Leave the level, or narrow the widened chip back.
    ClearFilter,         ///< Clear what is typed.
    Close,               ///< Close the popup.
    OpenRowMenu,         ///< Open the selected row's menu.
};

/// When a row of the key table applies.
enum class KeyCondition : uint8_t
{
    Always = 0,         ///< Whatever the context.
    FilterEmpty,        ///< Nothing is typed.
    Nested,             ///< In a level, or in the chip's dropdown widened to every daemon.
    NestedFilterEmpty,  ///< Nested, and nothing is typed.
    CaretEndOnDrillRow, ///< The caret is at the end, on a Daemon, New Daemon… or All daemons… row.
    CaretStartNested,   ///< The caret is at the start, nested.
    WorkspaceRow,       ///< A workspace row is selected.
    NewWindowRow,       ///< A row that can open in a new window is selected.
};

/// One row of the key table.
struct SwitcherKeyRow
{
    SwitcherKey key = SwitcherKey::Other;                     ///< The key.
    SwitcherKeyModifier modifier = SwitcherKeyModifier::None; ///< The modifiers held, exactly.
    KeyCondition when = KeyCondition::Always;                 ///< When it applies.
    SwitcherCommand command = SwitcherCommand::None;          ///< What it does.
};

/// The switcher's keys (spec §Keys). The first row that matches wins, so a narrower row comes before a wider
/// one of the same key.
inline constexpr auto SwitcherKeyRows = std::array {
    SwitcherKeyRow { .key = SwitcherKey::Up,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Previous },
    SwitcherKeyRow { .key = SwitcherKey::Down,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Next },
    SwitcherKeyRow { .key = SwitcherKey::K,
                     .modifier = SwitcherKeyModifier::Control,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Previous },
    SwitcherKeyRow { .key = SwitcherKey::J,
                     .modifier = SwitcherKeyModifier::Control,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Next },
    SwitcherKeyRow { .key = SwitcherKey::Home,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::FilterEmpty,
                     .command = SwitcherCommand::First },
    SwitcherKeyRow { .key = SwitcherKey::End,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::FilterEmpty,
                     .command = SwitcherCommand::Last },
    SwitcherKeyRow { .key = SwitcherKey::Return,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Activate },
    SwitcherKeyRow { .key = SwitcherKey::Return,
                     .modifier = SwitcherKeyModifier::Shift,
                     .when = KeyCondition::NewWindowRow,
                     .command = SwitcherCommand::ActivateInNewWindow },
    SwitcherKeyRow { .key = SwitcherKey::Return,
                     .modifier = SwitcherKeyModifier::Shift,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::Activate },
    SwitcherKeyRow { .key = SwitcherKey::Right,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::CaretEndOnDrillRow,
                     .command = SwitcherCommand::DrillIn },
    SwitcherKeyRow { .key = SwitcherKey::Left,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::CaretStartNested,
                     .command = SwitcherCommand::Back },
    SwitcherKeyRow { .key = SwitcherKey::Backspace,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::NestedFilterEmpty,
                     .command = SwitcherCommand::Back },
    SwitcherKeyRow { .key = SwitcherKey::Escape,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::NestedFilterEmpty,
                     .command = SwitcherCommand::Back },
    SwitcherKeyRow { .key = SwitcherKey::Escape,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::FilterEmpty,
                     .command = SwitcherCommand::Close },
    SwitcherKeyRow { .key = SwitcherKey::Escape,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::Always,
                     .command = SwitcherCommand::ClearFilter },
    SwitcherKeyRow { .key = SwitcherKey::Menu,
                     .modifier = SwitcherKeyModifier::None,
                     .when = KeyCondition::WorkspaceRow,
                     .command = SwitcherCommand::OpenRowMenu },
    SwitcherKeyRow { .key = SwitcherKey::F10,
                     .modifier = SwitcherKeyModifier::Shift,
                     .when = KeyCondition::WorkspaceRow,
                     .command = SwitcherCommand::OpenRowMenu },
};

/// @param context Where the key is pressed.
/// @return Whether the popup is nested: in a level, or in the chip's dropdown widened to every daemon, from
///         where Back returns rather than closing.
[[nodiscard]] constexpr bool isNested(SwitcherKeyContext const& context) noexcept
{
    return context.level != SwitcherLevel::Top
           || (context.origin == SwitcherOrigin::Chip && context.scope == SwitcherScope::Everything);
}

/// @param condition A row's condition.
/// @param context Where the key is pressed.
/// @return Whether the condition holds there.
[[nodiscard]] constexpr bool holds(KeyCondition condition, SwitcherKeyContext const& context) noexcept
{
    auto const empty = context.filter == FilterText::Empty;
    auto const atEnd = context.caret == CaretAt::End || context.caret == CaretAt::StartAndEnd;
    auto const atStart = context.caret == CaretAt::Start || context.caret == CaretAt::StartAndEnd;
    // Nothing selected reads as a row that neither drills in nor opens a window: the failure row.
    auto const& rule = switcherRowKindRule(context.selected.value_or(SwitcherRowKind::Failure));
    auto const onDrillRow = rule.drillsIn == DrillsIn::Yes;
    switch (condition)
    {
        case KeyCondition::Always: return true;
        case KeyCondition::FilterEmpty: return empty;
        case KeyCondition::Nested: return isNested(context);
        case KeyCondition::NestedFilterEmpty: return empty && isNested(context);
        case KeyCondition::CaretEndOnDrillRow: return atEnd && onDrillRow;
        case KeyCondition::CaretStartNested: return atStart && isNested(context);
        case KeyCondition::WorkspaceRow: return context.selected == SwitcherRowKind::Workspace;
        case KeyCondition::NewWindowRow: return rule.newWindow == OpensInNewWindow::Yes;
    }
    return false;
}

/// What a key does in the switcher.
/// @param key The key.
/// @param modifier The modifiers held with it.
/// @param context Where it is pressed.
/// @return The first matching SwitcherKeyRows row's command; None when no row takes the key.
[[nodiscard]] constexpr SwitcherCommand switcherKeyCommand(SwitcherKey key,
                                                           SwitcherKeyModifier modifier,
                                                           SwitcherKeyContext const& context) noexcept
{
    auto const found = std::ranges::find_if(SwitcherKeyRows, [&](SwitcherKeyRow const& row) {
        return row.key == key && row.modifier == modifier && holds(row.when, context);
    });
    return found != SwitcherKeyRows.end() ? found->command : SwitcherCommand::None;
}

/// What Escape does where the user is: what the footer's Escape hint names.
/// @param context Where it would be pressed.
/// @return ClearFilter, Back or Close.
[[nodiscard]] constexpr SwitcherCommand switcherEscapeCommand(SwitcherKeyContext const& context) noexcept
{
    return switcherKeyCommand(SwitcherKey::Escape, SwitcherKeyModifier::None, context);
}

} // namespace contour::window
