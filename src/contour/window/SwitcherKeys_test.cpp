// SPDX-License-Identifier: Apache-2.0
//
// The switcher's keys (spec §Keys) are one table: every row of the spec's table is a row here, checked with
// no window and no Qt.

#include <contour/window/SwitcherKeys.hpp>

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>

using namespace contour::window;

namespace
{

/// The top level, opened centred, nothing typed, on a workspace row.
constexpr auto TopEmpty = SwitcherKeyContext { .filter = FilterText::Empty,
                                               .caret = CaretAt::StartAndEnd,
                                               .level = SwitcherLevel::Top,
                                               .origin = SwitcherOrigin::Centred,
                                               .scope = SwitcherScope::Everything,
                                               .selected = SwitcherRowKind::Workspace };

[[nodiscard]] constexpr SwitcherKeyContext typed(SwitcherKeyContext context, CaretAt caret = CaretAt::End)
{
    context.filter = FilterText::Typed;
    context.caret = caret;
    return context;
}

[[nodiscard]] constexpr SwitcherKeyContext on(SwitcherKeyContext context, SwitcherRowKind selected)
{
    context.selected = selected;
    return context;
}

[[nodiscard]] constexpr SwitcherKeyContext inDaemonLevel(SwitcherKeyContext context)
{
    context.level = SwitcherLevel::Daemon;
    return context;
}

[[nodiscard]] constexpr SwitcherKeyContext chip(SwitcherKeyContext context, SwitcherScope scope)
{
    context.origin = SwitcherOrigin::Chip;
    context.scope = scope;
    return context;
}

struct KeyCase
{
    SwitcherKey key {};
    SwitcherKeyModifier modifier {};
    SwitcherKeyContext context {};
    SwitcherCommand expected {};
};

using enum SwitcherKey;
using enum SwitcherCommand;
constexpr auto NoModifier = SwitcherKeyModifier::None;
constexpr auto Shift = SwitcherKeyModifier::Shift;
constexpr auto Ctrl = SwitcherKeyModifier::Control;
constexpr auto OtherModifier = SwitcherKeyModifier::Other;

/// The list is empty: nothing is selected.
[[nodiscard]] constexpr SwitcherKeyContext nothingSelected(SwitcherKeyContext context)
{
    context.selected = std::nullopt;
    return context;
}

/// The cases, built at run time rather than as a constexpr table: a context with nothing selected copies a
/// disengaged std::optional, which libstdc++ 14 cannot do in a constant expression under Clang.
[[nodiscard]] auto keyCases()
{
    return std::array {
        // ↑ / ↓ and Ctrl+K / Ctrl+J move, whatever is typed.
        KeyCase { .key = Up, .modifier = NoModifier, .context = TopEmpty, .expected = Previous },
        KeyCase { .key = Down, .modifier = NoModifier, .context = typed(TopEmpty), .expected = Next },
        KeyCase { .key = K, .modifier = Ctrl, .context = TopEmpty, .expected = Previous },
        KeyCase { .key = J, .modifier = Ctrl, .context = typed(TopEmpty), .expected = Next },
        KeyCase { .key = J, .modifier = NoModifier, .context = TopEmpty, .expected = None },
        // Home / End jump with the field empty, and move the caret otherwise.
        KeyCase { .key = Home, .modifier = NoModifier, .context = TopEmpty, .expected = First },
        KeyCase { .key = End, .modifier = NoModifier, .context = TopEmpty, .expected = Last },
        KeyCase { .key = Home, .modifier = NoModifier, .context = typed(TopEmpty), .expected = None },
        // Return activates; the controller drills in on a drill row.
        KeyCase { .key = Return, .modifier = NoModifier, .context = TopEmpty, .expected = Activate },
        KeyCase { .key = Return,
                  .modifier = NoModifier,
                  .context = on(TopEmpty, SwitcherRowKind::Daemon),
                  .expected = Activate },
        // Shift+Return opens a new window from a row that can, and is Return elsewhere.
        KeyCase { .key = Return, .modifier = Shift, .context = TopEmpty, .expected = ActivateInNewWindow },
        KeyCase { .key = Return,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::Daemon),
                  .expected = ActivateInNewWindow },
        KeyCase { .key = Return,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::NewWorkspace),
                  .expected = ActivateInNewWindow },
        KeyCase { .key = Return,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::LabelAction),
                  .expected = ActivateInNewWindow },
        KeyCase { .key = Return,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::NewDaemon),
                  .expected = Activate },
        KeyCase { .key = Return,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::AllDaemons),
                  .expected = Activate },
        // → drills in with the caret at the end, on a drill row only.
        KeyCase { .key = Right,
                  .modifier = NoModifier,
                  .context = on(typed(TopEmpty, CaretAt::Inside), SwitcherRowKind::Daemon),
                  .expected = None },
        KeyCase { .key = Right,
                  .modifier = NoModifier,
                  .context = on(typed(TopEmpty, CaretAt::End), SwitcherRowKind::Daemon),
                  .expected = DrillIn },
        KeyCase { .key = Right,
                  .modifier = NoModifier,
                  .context = on(TopEmpty, SwitcherRowKind::NewDaemon),
                  .expected = DrillIn },
        KeyCase { .key = Right,
                  .modifier = NoModifier,
                  .context = on(TopEmpty, SwitcherRowKind::AllDaemons),
                  .expected = DrillIn },
        KeyCase { .key = Right,
                  .modifier = NoModifier,
                  .context = typed(TopEmpty, CaretAt::End),
                  .expected = None },
        // ← goes back with the caret at the start, in a level or the widened chip.
        KeyCase { .key = Left, .modifier = NoModifier, .context = inDaemonLevel(TopEmpty), .expected = Back },
        KeyCase { .key = Left,
                  .modifier = NoModifier,
                  .context = typed(inDaemonLevel(TopEmpty), CaretAt::Start),
                  .expected = Back },
        KeyCase { .key = Left,
                  .modifier = NoModifier,
                  .context = typed(inDaemonLevel(TopEmpty), CaretAt::End),
                  .expected = None },
        KeyCase { .key = Left, .modifier = NoModifier, .context = TopEmpty, .expected = None },
        KeyCase { .key = Left,
                  .modifier = NoModifier,
                  .context = chip(TopEmpty, SwitcherScope::Everything),
                  .expected = Back },
        // ⌫ goes back from an empty field in a level or the widened chip, and edits text otherwise.
        KeyCase {
            .key = Backspace, .modifier = NoModifier, .context = inDaemonLevel(TopEmpty), .expected = Back },
        KeyCase { .key = Backspace,
                  .modifier = NoModifier,
                  .context = chip(TopEmpty, SwitcherScope::Everything),
                  .expected = Back },
        KeyCase { .key = Backspace,
                  .modifier = NoModifier,
                  .context = typed(inDaemonLevel(TopEmpty)),
                  .expected = None },
        KeyCase { .key = Backspace, .modifier = NoModifier, .context = TopEmpty, .expected = None },
        KeyCase { .key = Backspace, .modifier = Ctrl, .context = inDaemonLevel(TopEmpty), .expected = None },
        // Alt+⌫ (and any other combination) is the field's: it deletes a word, and never goes back.
        KeyCase { .key = Backspace,
                  .modifier = OtherModifier,
                  .context = inDaemonLevel(TopEmpty),
                  .expected = None },
        KeyCase { .key = Return, .modifier = OtherModifier, .context = TopEmpty, .expected = None },
        // ⎋ clears what is typed, else goes back from a level or the widened chip, else closes.
        KeyCase {
            .key = Escape, .modifier = NoModifier, .context = typed(TopEmpty), .expected = ClearFilter },
        KeyCase { .key = Escape,
                  .modifier = NoModifier,
                  .context = typed(inDaemonLevel(TopEmpty)),
                  .expected = ClearFilter },
        KeyCase {
            .key = Escape, .modifier = NoModifier, .context = inDaemonLevel(TopEmpty), .expected = Back },
        KeyCase { .key = Escape,
                  .modifier = NoModifier,
                  .context = chip(TopEmpty, SwitcherScope::Everything),
                  .expected = Back },
        KeyCase { .key = Escape,
                  .modifier = NoModifier,
                  .context = chip(TopEmpty, SwitcherScope::OneDaemon),
                  .expected = Close },
        KeyCase { .key = Escape, .modifier = NoModifier, .context = TopEmpty, .expected = Close },
        // Menu and Shift+F10 open a workspace row's menu.
        KeyCase { .key = Menu, .modifier = NoModifier, .context = TopEmpty, .expected = OpenRowMenu },
        KeyCase { .key = F10, .modifier = Shift, .context = TopEmpty, .expected = OpenRowMenu },
        KeyCase { .key = F10,
                  .modifier = Shift,
                  .context = on(TopEmpty, SwitcherRowKind::Daemon),
                  .expected = None },
        KeyCase { .key = F10, .modifier = NoModifier, .context = TopEmpty, .expected = None },
        // Nothing selected: no row's menu opens, and Shift+Return is Return.
        KeyCase { .key = F10, .modifier = Shift, .context = nothingSelected(TopEmpty), .expected = None },
        KeyCase {
            .key = Menu, .modifier = NoModifier, .context = nothingSelected(TopEmpty), .expected = None },
        KeyCase {
            .key = Return, .modifier = Shift, .context = nothingSelected(TopEmpty), .expected = Activate },
        KeyCase {
            .key = Right, .modifier = NoModifier, .context = nothingSelected(TopEmpty), .expected = None },
        KeyCase { .key = Other, .modifier = NoModifier, .context = TopEmpty, .expected = None },
    };
}

} // namespace

TEST_CASE("switcherKeyCommand follows the spec's key table", "[switcher][chromekeys]")
{
    for (auto const [index, each]: std::views::zip(std::views::iota(std::size_t { 0 }), keyCases()))
    {
        INFO("case " << index);
        CHECK(switcherKeyCommand(each.key, each.modifier, each.context) == each.expected);
    }
}

TEST_CASE("switcherEscapeCommand is what Escape does", "[switcher][chromekeys]")
{
    for (auto const& each: keyCases())
        if (each.key == Escape && each.modifier == NoModifier)
            CHECK(switcherEscapeCommand(each.context) == each.expected);
    STATIC_REQUIRE(switcherEscapeCommand(TopEmpty) == Close);
}

TEST_CASE("switcherKeyModifierOf tells Shift and Control alone from every other combination",
          "[switcher][chromekeys]")
{
    struct ModifierCase
    {
        HeldModifier held;
        SwitcherKeyModifier expected;
    };
    // Control is Qt's ControlModifier: Ctrl on Linux and Windows, ⌘ on macOS, where Meta is the Ctrl key.
    constexpr auto Cases = std::array {
        ModifierCase { .held = HeldModifier::None, .expected = SwitcherKeyModifier::None },
        ModifierCase { .held = HeldModifier::Shift, .expected = SwitcherKeyModifier::Shift },
        ModifierCase { .held = HeldModifier::Control, .expected = SwitcherKeyModifier::Control },
        ModifierCase { .held = HeldModifier::Alt, .expected = SwitcherKeyModifier::Other },
        ModifierCase { .held = HeldModifier::Meta, .expected = SwitcherKeyModifier::Other },
        ModifierCase { .held = HeldModifier::Shift | HeldModifier::Control,
                       .expected = SwitcherKeyModifier::Other },
        ModifierCase { .held = HeldModifier::Shift | HeldModifier::Alt,
                       .expected = SwitcherKeyModifier::Other },
    };
    for (auto const& each: Cases)
    {
        INFO("held " << static_cast<int>(each.held));
        CHECK(switcherKeyModifierOf(each.held) == each.expected);
    }
}
