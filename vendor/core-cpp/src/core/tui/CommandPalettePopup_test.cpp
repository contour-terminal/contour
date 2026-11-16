// SPDX-License-Identifier: Apache-2.0
#include <core/Clock.hpp>
#include <core/tui/CommandPalettePopup.hpp>
#include <core/tui/CommandRegistry.hpp>

#include <catch2/catch_test_macros.hpp>

#include <type_traits>

using namespace core::tui;

namespace
{

/// Helper to build a registry with some test commands.
CommandRegistry makeTestRegistry()
{
    CommandRegistry reg;
    reg.add({
        .id = "nav.go_to_dir",
        .label = "Go to Directory",
        .description = "Navigate to a recent directory",
        .category = "Navigation",
        .keybinding = "Ctrl+T",
        .context = CommandContext::Shell,
        .action = [] {},
    });
    reg.add({
        .id = "session.new",
        .label = "New Session",
        .description = "Start a fresh conversation",
        .category = "Session",
        .keybinding = "",
        .context = CommandContext::Agent,
        .action = [] {},
    });
    reg.add({
        .id = "view.clear",
        .label = "Clear Screen",
        .description = "Clear the terminal",
        .category = "View",
        .keybinding = "Ctrl+L",
        .context = CommandContext::Both,
        .action = [] {},
    });
    reg.add({
        .id = "mode.agent",
        .label = "Enter Agent Mode",
        .description = "Switch to AI agent",
        .category = "Mode",
        .keybinding = "#",
        .context = CommandContext::Shell,
        .action = [] {},
    });
    return reg;
}

/// Helper to send a character key event.
InputEvent charKey(char ch)
{
    return KeyEvent {
        .key = static_cast<KeyCode>(ch),
        .modifiers = Modifier::None,
        .codepoint = static_cast<char32_t>(ch),
    };
}

/// Helper to send a special key event.
InputEvent specialKey(KeyCode code, Modifier mod = Modifier::None)
{
    return KeyEvent {
        .key = code,
        .modifiers = mod,
        .codepoint = 0,
    };
}

} // namespace

// ============================================================================
// Visibility tests
// ============================================================================

TEST_CASE("CommandPalettePopup.initial_state")
{
    CommandPalettePopup const popup;
    CHECK_FALSE(popup.visible());
}

TEST_CASE("CommandPalettePopup.show_makes_visible")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);
    CHECK(popup.visible());
}

TEST_CASE("CommandPalettePopup.show_filters_by_context")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    // Shell context should see Shell + Both commands (3 items: nav.go_to_dir, view.clear, mode.agent)
    popup.show(reg, CommandContext::Shell);
    CHECK(popup.visible());

    // Agent context should see Agent + Both commands (2 items: session.new, view.clear)
    popup.show(reg, CommandContext::Agent);
    CHECK(popup.visible());
}

TEST_CASE("CommandPalettePopup.hide")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);
    CHECK(popup.visible());

    popup.hide();
    CHECK_FALSE(popup.visible());
}

// ============================================================================
// Navigation tests
// ============================================================================

TEST_CASE("CommandPalettePopup.escape_dismisses")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);
    auto const action = popup.processEvent(specialKey(KeyCode::Escape));
    CHECK(action == CommandPaletteAction::Dismissed);
    CHECK_FALSE(popup.visible());
}

TEST_CASE("CommandPalettePopup.down_arrow_navigates")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);

    // Initial selection is 0, pressing Down should move to 1
    auto const action = popup.processEvent(specialKey(KeyCode::Down));
    CHECK(action == CommandPaletteAction::Changed);
}

TEST_CASE("CommandPalettePopup.up_arrow_navigates")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);

    // Pressing Up from 0 should wrap to last item
    auto const action = popup.processEvent(specialKey(KeyCode::Up));
    CHECK(action == CommandPaletteAction::Changed);
}

// ============================================================================
// Fuzzy filtering tests
// ============================================================================

TEST_CASE("CommandPalettePopup.typing_filters_items")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);

    // Type "clear" to filter — should match "Clear Screen"
    for (auto ch: std::string_view("clear"))
    {
        auto const action = popup.processEvent(charKey(ch));
        CHECK(action == CommandPaletteAction::Changed);
    }

    // The palette should still be visible (matches found)
    CHECK(popup.visible());
}

// ============================================================================
// Size tests
// ============================================================================

TEST_CASE("CommandPalettePopup.preferredSize_reasonable")
{
    CommandPalettePopup popup;
    auto reg = makeTestRegistry();

    popup.show(reg, CommandContext::Shell);
    auto const size = popup.preferredSize();

    // Width should be within MinWidth..MaxWidth range
    CHECK(size.width >= 50);
    CHECK(size.width <= 80);

    // Height: borders(2) + filter(1) + separator(1) + items(N)
    CHECK(size.height >= 4); // Minimum: borders + filter + separator
}

TEST_CASE("CommandPalettePopup.preferredSize_when_hidden")
{
    CommandPalettePopup const popup;
    auto const size = popup.preferredSize();

    CHECK(size.width == 0);
    CHECK(size.height == 0);
}

// ============================================================================
// Enter executes
// ============================================================================

TEST_CASE("CommandPalettePopup.enter_executes_selected")
{
    CommandPalettePopup popup;
    auto executed = false;

    CommandRegistry reg;
    reg.add({
        .id = "test.exec",
        .label = "Execute Me",
        .description = "",
        .category = "Test",
        .keybinding = "",
        .context = CommandContext::Shell,
        .action = [&] { executed = true; },
    });

    popup.show(reg, CommandContext::Shell);
    auto const action = popup.processEvent(specialKey(KeyCode::Enter));
    CHECK(action == CommandPaletteAction::Executed);
    CHECK(executed);
    CHECK_FALSE(popup.visible());
}

// ============================================================================
// Sub-menu tests
// ============================================================================

TEST_CASE("CommandPalettePopup.showSubMenu")
{
    CommandPalettePopup popup;

    std::vector<CommandEntry> items;
    items.push_back({
        .id = "sub.a",
        .label = "Item A",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Both,
        .action = [] {},
    });
    items.push_back({
        .id = "sub.b",
        .label = "Item B",
        .description = "",
        .category = "",
        .keybinding = "",
        .context = CommandContext::Both,
        .action = [] {},
    });

    popup.showSubMenu(std::move(items), "Pick One");
    CHECK(popup.visible());
}

TEST_CASE("CommandPalettePopup.injectedClockReachesTheFilterField")
{
    // The filter field's multi-click window runs on the clock the palette was given.
    auto const clock = core::ManualClock {};
    auto const palette = CommandPalettePopup { clock };
    CHECK(&palette.clock() == &clock);
    CHECK(&CommandPalettePopup {}.clock() == &core::defaultSteadyClock());

    // A clock is kept, so a temporary one is refused at compile time.
    STATIC_CHECK_FALSE(std::is_constructible_v<CommandPalettePopup, core::ManualClock>);
}
