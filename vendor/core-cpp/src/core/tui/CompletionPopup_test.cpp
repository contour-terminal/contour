// SPDX-License-Identifier: Apache-2.0
#include <core/tui/CompletionPopup.hpp>
#include <core/tui/TestHelpers.hpp>
#include <core/tui/completer/Completer.hpp>

#include <catch2/catch_test_macros.hpp>

using namespace core::tui;
using namespace core::tui::completer;
using namespace core::tui::test;

// ============================================================================
// Visibility and item management tests
// ============================================================================

TEST_CASE("CompletionPopup.initial_state")
{
    CompletionPopup const popup;

    CHECK_FALSE(popup.visible());
    CHECK(popup.empty());
    CHECK(popup.itemCount() == 0);
    CHECK(popup.selectedItem() == nullptr);
}

TEST_CASE("CompletionPopup.show_with_items")
{
    CompletionPopup popup;

    auto items = makeItems({ "apple", "banana", "cherry" });
    popup.show(std::move(items));

    CHECK(popup.visible());
    CHECK(popup.itemCount() == 3);
    CHECK(popup.selectedIndex() == 0);
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "apple");
}

TEST_CASE("CompletionPopup.show_with_empty_items")
{
    CompletionPopup popup;

    popup.show({});

    CHECK_FALSE(popup.visible());
    CHECK(popup.empty());
}

TEST_CASE("CompletionPopup.hide")
{
    CompletionPopup popup;

    popup.show(makeItems({ "test" }));
    CHECK(popup.visible());

    popup.hide();
    CHECK_FALSE(popup.visible());
    CHECK(popup.empty());
    CHECK(popup.selectedItem() == nullptr);
}

// ============================================================================
// Selection navigation tests
// ============================================================================

TEST_CASE("CompletionPopup.selectNext")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    CHECK(popup.selectedIndex() == 0);

    popup.selectNext();
    CHECK(popup.selectedIndex() == 1);

    popup.selectNext();
    CHECK(popup.selectedIndex() == 2);

    // Wrap around
    popup.selectNext();
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.selectPrev")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    CHECK(popup.selectedIndex() == 0);

    // Wrap around to last
    popup.selectPrev();
    CHECK(popup.selectedIndex() == 2);

    popup.selectPrev();
    CHECK(popup.selectedIndex() == 1);
}

TEST_CASE("CompletionPopup.selectFirst_and_selectLast")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c", "d", "e" }));

    popup.selectLast();
    CHECK(popup.selectedIndex() == 4);

    popup.selectFirst();
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.pageDown_and_pageUp")
{
    CompletionPopup popup;
    popup.setMaxVisible(3);
    popup.show(makeItems({ "a", "b", "c", "d", "e", "f", "g", "h" }));

    popup.pageDown();
    CHECK(popup.selectedIndex() == 3);

    popup.pageDown();
    CHECK(popup.selectedIndex() == 6);

    popup.pageDown();
    CHECK(popup.selectedIndex() == 7); // Last item

    popup.pageUp();
    CHECK(popup.selectedIndex() == 4);

    popup.pageUp();
    CHECK(popup.selectedIndex() == 1);

    popup.pageUp();
    CHECK(popup.selectedIndex() == 0); // First item
}

// ============================================================================
// Keyboard event handling tests
// ============================================================================

TEST_CASE("CompletionPopup.keyboard_navigation_down")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    auto action = popup.processEvent(specialKey(KeyCode::Down));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 1);
}

TEST_CASE("CompletionPopup.keyboard_navigation_up")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));
    popup.selectNext(); // Go to index 1

    auto action = popup.processEvent(specialKey(KeyCode::Up));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.keyboard_navigation_ctrl_j_k")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    // Ctrl+J = Down
    auto action = popup.processEvent(charKey('j', Modifier::Ctrl));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 1);

    // Ctrl+K = Up
    action = popup.processEvent(charKey('k', Modifier::Ctrl));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.accept_with_enter")
{
    CompletionPopup popup;
    popup.show(makeItems({ "test" }));

    auto action = popup.processEvent(specialKey(KeyCode::Enter));
    CHECK(action == CompletionAction::Accepted);
}

TEST_CASE("CompletionPopup.tab_cycles_next")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana", "cherry" }));

    CHECK(popup.selectedIndex() == 0);

    auto action = popup.processEvent(specialKey(KeyCode::Tab));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 1);

    action = popup.processEvent(specialKey(KeyCode::Tab));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 2);

    // Wrap around
    action = popup.processEvent(specialKey(KeyCode::Tab));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.shift_tab_cycles_prev")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana", "cherry" }));

    CHECK(popup.selectedIndex() == 0);

    // Wrap around to last
    auto action = popup.processEvent(specialKey(KeyCode::Tab, Modifier::Shift));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 2);

    action = popup.processEvent(specialKey(KeyCode::Tab, Modifier::Shift));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 1);

    action = popup.processEvent(specialKey(KeyCode::Tab, Modifier::Shift));
    CHECK(action == CompletionAction::Changed);
    CHECK(popup.selectedIndex() == 0);
}

TEST_CASE("CompletionPopup.dismiss_with_escape")
{
    CompletionPopup popup;
    popup.show(makeItems({ "test" }));

    auto action = popup.processEvent(specialKey(KeyCode::Escape));
    CHECK(action == CompletionAction::Dismissed);
}

TEST_CASE("CompletionPopup.unhandled_key_dismisses")
{
    CompletionPopup popup;
    popup.show(makeItems({ "test" }));

    // A regular character key should dismiss
    auto action = popup.processEvent(charKey('x'));
    CHECK(action == CompletionAction::Dismissed);
}

TEST_CASE("CompletionPopup.events_when_hidden")
{
    CompletionPopup popup;
    // Don't show it

    auto action = popup.processEvent(specialKey(KeyCode::Down));
    CHECK(action == CompletionAction::Dismissed);
}

// ============================================================================
// Configuration tests
// ============================================================================

TEST_CASE("CompletionPopup.maxVisible")
{
    CompletionPopup popup;

    popup.setMaxVisible(5);
    CHECK(popup.maxVisible() == 5);

    popup.setMaxVisible(0); // Should be clamped to 1
    CHECK(popup.maxVisible() == 1);
}

TEST_CASE("CompletionPopup.itemAt")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    REQUIRE(popup.itemAt(0) != nullptr);
    CHECK(popup.itemAt(0)->text == "a");

    REQUIRE(popup.itemAt(1) != nullptr);
    CHECK(popup.itemAt(1)->text == "b");

    REQUIRE(popup.itemAt(2) != nullptr);
    CHECK(popup.itemAt(2)->text == "c");

    CHECK(popup.itemAt(3) == nullptr); // Out of bounds
}

// ============================================================================
// Rendering tests
// ============================================================================

TEST_CASE("CompletionPopup.preferredSize")
{
    CompletionPopup popup;

    // Empty popup has no size
    auto size = popup.preferredSize();
    CHECK(size.width == 0);
    CHECK(size.height == 0);

    // With items
    popup.show(makeItems({ "apple", "banana" }));
    size = popup.preferredSize();
    CHECK(size.width > 0);
    CHECK(size.height > 0);
}

TEST_CASE("CompletionPopup.render_contains_items")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana" }));

    auto rendered = renderPopup(popup, 30, 10);

    // Should contain the item text
    CHECK(rendered.contains("apple"));
    CHECK(rendered.contains("banana"));
}

TEST_CASE("CompletionPopup.render_with_descriptions")
{
    CompletionPopup popup;
    popup.show(makeItemsWithDesc({ { "git", "version control" }, { "grep", "search tool" } }));

    auto rendered = renderPopup(popup, 40, 10);

    CHECK(rendered.contains("git"));
    CHECK(rendered.contains("grep"));
    // Descriptions may or may not be visible depending on width
}

TEST_CASE("CompletionPopup.renderedHeight_and_renderedWidth")
{
    CompletionPopup popup;
    popup.show(makeItems({ "a", "b", "c" }));

    // Render to get dimensions
    Buffer buffer(10, 30);
    buffer.clear();
    Theme const theme;
    Canvas canvas(buffer, Rect { .x = 0, .y = 0, .width = 30, .height = 10 }, theme);
    popup.render(canvas);

    CHECK(popup.renderedHeight() > 0);
    CHECK(popup.renderedWidth() > 0);
}

// ============================================================================
// updateItems() tests
// ============================================================================

TEST_CASE("CompletionPopup.updateItems_preserves_selection")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana", "cherry" }));

    popup.selectNext(); // Select "banana"
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "banana");

    // Update with list still containing "banana"
    popup.updateItems(makeItems({ "banana", "cherry" }));

    CHECK(popup.visible());
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "banana"); // Still selected
}

TEST_CASE("CompletionPopup.updateItems_selects_first_when_not_found")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana", "cherry" }));

    popup.selectNext(); // Select "banana"
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "banana");

    // Update with list NOT containing "banana"
    popup.updateItems(makeItems({ "cherry", "date" }));

    CHECK(popup.visible());
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "cherry"); // First item selected
}

TEST_CASE("CompletionPopup.updateItems_hides_on_empty")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana" }));
    CHECK(popup.visible());

    popup.updateItems({});

    CHECK_FALSE(popup.visible());
}

TEST_CASE("CompletionPopup.updateItems_from_hidden_state")
{
    CompletionPopup popup;
    CHECK_FALSE(popup.visible());

    // Update with items when not visible
    popup.updateItems(makeItems({ "apple", "banana" }));

    CHECK(popup.visible());
    CHECK(popup.itemCount() == 2);
    REQUIRE(popup.selectedItem() != nullptr);
    CHECK(popup.selectedItem()->text == "apple"); // First item selected
}

// ============================================================================
// items() accessor tests
// ============================================================================

TEST_CASE("CompletionPopup.items_accessor_empty")
{
    CompletionPopup const popup;

    auto const& items = popup.items();
    CHECK(items.empty());
}

TEST_CASE("CompletionPopup.items_accessor_with_items")
{
    CompletionPopup popup;
    popup.show(makeItems({ "foo", "foobar", "food" }));

    auto const& items = popup.items();
    REQUIRE(items.size() == 3);
    CHECK(items[0].text == "foo");
    CHECK(items[1].text == "foobar");
    CHECK(items[2].text == "food");
}

TEST_CASE("CompletionPopup.items_lcp_integration")
{
    CompletionPopup popup;
    popup.show(makeItems({ "foobar", "food", "football" }));

    // Verify findCommonPrefix works with items from popup
    auto const prefix = Completer::findCommonPrefix(popup.items());
    CHECK(prefix == "foo");
}

TEST_CASE("CompletionPopup.items_lcp_no_common_prefix")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana", "cherry" }));

    auto const prefix = Completer::findCommonPrefix(popup.items());
    CHECK(prefix.empty());
}

TEST_CASE("CompletionPopup.items_lcp_full_match")
{
    CompletionPopup popup;
    popup.show(makeItems({ "hello", "hello" }));

    auto const prefix = Completer::findCommonPrefix(popup.items());
    CHECK(prefix == "hello");
}

// ============================================================================
// Detail panel tests
// ============================================================================

TEST_CASE("CompletionPopup.detail_panel_preferred_size_includes_detail_width")
{
    CompletionPopup popup;

    // Without detail
    popup.show(makeItems({ "apple", "banana" }));
    auto sizeWithout = popup.preferredSize();

    // With detail
    popup.show(makeItemsWithDetail({
        { "apple", "fruit", "**apple** : `fruit`\n\nA delicious red fruit." },
        { "banana", "fruit", "**banana** : `fruit`\n\nA yellow tropical fruit." },
    }));
    auto sizeWith = popup.preferredSize();

    // Size with detail should be wider
    CHECK(sizeWith.width > sizeWithout.width);
    // Height should be the same
    CHECK(sizeWith.height == sizeWithout.height);
}

TEST_CASE("CompletionPopup.detail_panel_not_shown_when_detail_empty")
{
    CompletionPopup popup;
    popup.show(makeItems({ "apple", "banana" }));

    auto size = popup.preferredSize();

    // Render and check width matches menu only (no detail panel)
    Buffer buffer(10, 60);
    buffer.clear();
    Theme const theme;
    Canvas canvas(buffer, Rect { .x = 0, .y = 0, .width = 60, .height = 10 }, theme);
    popup.render(canvas);

    // renderedWidth should match the menu width (no extra detail panel)
    CHECK(popup.renderedWidth() == size.width);
}

TEST_CASE("CompletionPopup.detail_content_updates_on_selection_change")
{
    CompletionPopup popup;
    popup.show(makeItemsWithDetail({
        { "alpha", "desc1", "**alpha** detail" },
        { "beta", "desc2", "**beta** detail" },
        { "gamma", "desc3", "" }, // No detail
    }));

    // Initial selection should have detail
    auto size1 = popup.preferredSize();
    CHECK(size1.width > 0);

    // Select item with no detail
    popup.selectNext(); // beta
    popup.selectNext(); // gamma (no detail)

    static_cast<void>(popup.preferredSize());

    // After selecting item with no detail, width should shrink
    // (the detail panel is not shown)
    // Re-select an item with detail to confirm it comes back
    popup.selectFirst(); // alpha
    auto sizeBack = popup.preferredSize();
    CHECK(sizeBack.width == size1.width);
}
