// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Box.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/ScrollableSelection.hpp>
#include <core/tui/StyledText.hpp>
#include <core/tui/Theme.hpp>
#include <core/tui/completer/CompletionItem.hpp>

#include <cstddef>
#include <string>
#include <vector>

namespace core::tui
{

/// @brief Result of processing an input event in CompletionPopup.
enum class CompletionAction : std::uint8_t
{
    Changed,   ///< Selection changed, re-render needed.
    Accepted,  ///< User accepted the selected completion.
    Dismissed, ///< User dismissed the popup (Escape or unhandled key).
};

/// @brief A popup menu for showing completion suggestions.
///
/// This is a proper TUI widget that manages its own visibility state,
/// handles keyboard navigation, and renders using the Canvas API.
/// It's designed to be shown below the cursor in a text input context.
///
/// As a Component, CompletionPopup can be added to a Screen's component tree
/// and will receive events through the standard event dispatch mechanism.
///
/// Usage:
/// @code
///   CompletionPopup popup;
///   popup.show(completions);  // Show with items
///
///   // In event loop:
///   auto action = popup.processEvent(event);
///   if (action == CompletionAction::Accepted) {
///       auto item = popup.selectedItem();
///       // Use item->text for insertion
///   }
///
///   // Rendering happens automatically via Component::render(Canvas&)
/// @endcode
class CompletionPopup: public Component
{
  public:
    /// @brief Constructs an initially hidden completion popup framed in single lines.
    CompletionPopup() = default;

    /// @brief Constructs an initially hidden completion popup framed in @p border.
    /// @param border The frame's style; its T-junctions join the detail panel's frame to the menu's.
    explicit CompletionPopup(BorderStyle border);

    /// @brief Returns the style the popup is framed in.
    [[nodiscard]] BorderStyle border() const noexcept { return _border; }
    ~CompletionPopup() override = default;

    // --- Component Interface ---

    /// @brief Renders the popup to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    /// @param event The input event.
    /// @return EventResult for event bubbling.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief CompletionPopup can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size based on items.
    [[nodiscard]] Size preferredSize() const override;

    // ========================================================================
    // Visibility and Items
    // ========================================================================

    /// @brief Shows the popup with the given completion items.
    /// @param items The completion items to display.
    /// @note If items is empty, the popup remains hidden.
    void show(std::vector<completer::CompletionItem> items);

    /// @brief Hides the popup and clears items.
    void hide();

    /// @brief Updates the completion items, preserving selection if possible.
    ///
    /// If the currently selected item (by text) exists in the new list,
    /// keeps it selected. Otherwise, selects the first item (best match).
    /// If the new list is empty, hides the popup.
    ///
    /// @param items The new completion items.
    void updateItems(std::vector<completer::CompletionItem> items);

    /// @brief Returns whether the popup is currently visible.
    /// @note Overrides Component::visible() to also check for non-empty items.
    [[nodiscard]] bool visible() const noexcept override;

    /// @brief Returns the number of items in the popup.
    [[nodiscard]] size_t itemCount() const noexcept;

    /// @brief Returns true if the popup has no items.
    [[nodiscard]] bool empty() const noexcept;

    /// @brief Returns a const reference to the completion items.
    [[nodiscard]] std::vector<completer::CompletionItem> const& items() const noexcept;

    // ========================================================================
    // Selection
    // ========================================================================

    /// @brief Returns the currently selected index.
    [[nodiscard]] size_t selectedIndex() const noexcept;

    /// @brief Returns the currently selected item, or nullptr if none.
    [[nodiscard]] completer::CompletionItem const* selectedItem() const noexcept;

    /// @brief Returns the item at the given index, or nullptr if out of bounds.
    [[nodiscard]] completer::CompletionItem const* itemAt(size_t index) const noexcept;

    /// @brief Selects the next item (with wrap-around).
    void selectNext();

    /// @brief Selects the previous item (with wrap-around).
    void selectPrev();

    /// @brief Moves selection down by one page.
    void pageDown();

    /// @brief Moves selection up by one page.
    void pageUp();

    /// @brief Selects the first item.
    void selectFirst();

    /// @brief Selects the last item.
    void selectLast();

    // ========================================================================
    // Configuration
    // ========================================================================

    /// @brief Sets the maximum number of visible items.
    /// @param maxVisible Maximum items to show (default: 10).
    void setMaxVisible(int maxVisible);

    /// @brief Returns the maximum number of visible items.
    [[nodiscard]] int maxVisible() const noexcept;

    // ========================================================================
    // Event Handling
    // ========================================================================

    /// @brief Processes an input event.
    ///
    /// Handles keyboard navigation:
    /// - Up/Down or Ctrl+K/J: Navigate items
    /// - PageUp/PageDown: Page navigation
    /// - Enter/Tab: Accept selection
    /// - Escape: Dismiss popup
    ///
    /// @param event The input event to process.
    /// @return The resulting action.
    [[nodiscard]] CompletionAction processEvent(InputEvent const& event);

    /// @brief Returns the height of the last rendered popup (including border).
    [[nodiscard]] int renderedHeight() const noexcept;

    /// @brief Returns the width of the last rendered popup (including border).
    [[nodiscard]] int renderedWidth() const noexcept;

  private:
    BorderStyle _border = BorderStyle::Single;
    std::vector<completer::CompletionItem> _items;
    ScrollableSelection _selection { 10 }; ///< Selection and scroll state.
    int _renderedHeight = 0;
    int _renderedWidth = 0;
    bool _visible = false;

    // Detail panel state
    StyledText _detailContent;   ///< Parsed markdown for selected item's detail.
    int _detailScrollOffset = 0; ///< Scroll offset within detail panel.

    /// @brief Calculates the width needed for the popup.
    [[nodiscard]] int calculateWidth(int maxWidth) const;

    /// @brief Handles a key event.
    [[nodiscard]] CompletionAction handleKey(KeyEvent const& key);

    /// @brief Updates detail content from the currently selected item.
    void updateDetailContent();

    /// @brief Returns the width of the detail panel based on content.
    [[nodiscard]] int detailPanelWidth() const;
};

} // namespace core::tui
