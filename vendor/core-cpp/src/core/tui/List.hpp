// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/Theme.hpp>

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace core::tui
{

/// @brief A single item in a List.
struct ListItem
{
    std::string label {};       ///< Display text for the item.
    std::string description {}; ///< Optional secondary description.
    std::string filterText {};  ///< Text used for filtering (defaults to label if empty).
    bool enabled = true;        ///< Whether the item can be selected.
};

/// @brief Result of processing an input event in List.
enum class ListAction : std::uint8_t
{
    None,      ///< No action taken.
    Changed,   ///< Selection or scroll position changed, re-render needed.
    Selected,  ///< User selected an item (Enter pressed).
    Cancelled, ///< User cancelled (Escape pressed).
    Toggled,   ///< An item was toggled (multi-select Space key).
};

/// @brief Configuration for List styling.
struct ListStyle
{
    Style normal;                        ///< Style for normal (unselected) items.
    Style selected;                      ///< Style for the selected item.
    Style disabled;                      ///< Style for disabled items.
    Style description;                   ///< Style for item descriptions.
    std::string_view cursor = "\u25B6 "; ///< Cursor indicator (▶ )
    std::string_view noCursor = "  ";    ///< Padding when not selected.
    std::string_view checked = "[x] ";   ///< Multi-select: checked prefix.
    std::string_view unchecked = "[ ] "; ///< Multi-select: unchecked prefix.
    bool showDescription = true;         ///< Whether to show descriptions.
};

/// @brief A scrollable, selectable list component.
///
/// Supports keyboard navigation (Up/Down, j/k, Home/End, PageUp/PageDown),
/// filtering, and selection. As a Component, List can be added to a Screen's
/// component tree and will receive events through the standard event dispatch.
class List: public Component
{
  public:
    /// @brief Constructs an empty list.
    List() = default;
    ~List() override = default;

    /// @brief Constructs a list with the given items.
    /// @param items The items to display.
    explicit List(std::vector<ListItem> items);

    // --- Component Interface ---

    /// @brief Renders the list to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    /// @param event The input event.
    /// @return EventResult for event bubbling.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief List can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size based on items.
    [[nodiscard]] Size preferredSize() const override;

    /// @brief Sets the items in the list.
    /// @param items The items to display.
    void setItems(std::vector<ListItem> items);

    /// @brief Returns the items in the list.
    [[nodiscard]] auto items() const noexcept -> std::vector<ListItem> const&;

    /// @brief Returns the number of items.
    [[nodiscard]] auto size() const noexcept -> std::size_t;

    /// @brief Returns true if the list is empty.
    [[nodiscard]] auto empty() const noexcept -> bool;

    /// @brief Returns the index of the currently selected item.
    [[nodiscard]] auto selectedIndex() const noexcept -> std::size_t;

    /// @brief Returns the currently selected item, or nullopt if empty.
    [[nodiscard]] auto selectedItem() const noexcept -> std::optional<ListItem const*>;

    /// @brief Sets the selected index.
    /// @param index The index to select.
    void setSelectedIndex(std::size_t index);

    /// @brief Sets the filter text for fuzzy matching.
    /// @param filter The filter text.
    void setFilter(std::string_view filter);

    /// @brief Returns the current filter text.
    [[nodiscard]] auto filter() const noexcept -> std::string_view;

    /// @brief Clears the filter.
    void clearFilter();

    /// @brief Returns the filtered/visible items.
    [[nodiscard]] auto visibleItems() const noexcept -> std::vector<std::size_t> const&;

    /// @brief Processes an input event and returns the resulting action.
    /// @param event The input event to process.
    /// @return The action resulting from the event.
    [[nodiscard]] auto processEvent(InputEvent const& event) -> ListAction;

    /// @brief Sets the list style configuration.
    /// @param style The style configuration.
    void setStyle(ListStyle style);

    /// @brief Returns the current style configuration.
    [[nodiscard]] auto style() const noexcept -> ListStyle const&;

    /// @brief Selects the next enabled item.
    void selectNext();

    /// @brief Selects the previous enabled item.
    void selectPrevious();

    /// @brief Selects the first enabled item.
    void selectFirst();

    /// @brief Selects the last enabled item.
    void selectLast();

    /// @brief Moves selection down by a page.
    /// @param pageSize Number of items per page.
    void pageDown(int pageSize);

    /// @brief Moves selection up by a page.
    /// @param pageSize Number of items per page.
    void pageUp(int pageSize);

    /// @brief Enables or disables multi-select mode.
    /// @param enabled True to enable multi-select (checkbox) mode.
    void setMultiSelect(bool enabled);

    /// @brief Returns whether multi-select mode is enabled.
    [[nodiscard]] bool multiSelect() const noexcept;

    /// @brief Returns indices of all checked items.
    [[nodiscard]] auto checkedIndices() const -> std::vector<std::size_t>;

    /// @brief Returns whether the item at the given index is checked.
    /// @param index The item index.
    [[nodiscard]] bool isChecked(std::size_t index) const;

    /// @brief Sets the checked state of an item.
    /// @param index The item index.
    /// @param checked True to check, false to uncheck.
    void setChecked(std::size_t index, bool checked);

    /// @brief Toggles the checked state of the currently selected item.
    void toggleChecked();

  private:
    std::vector<ListItem> _items;
    std::vector<std::size_t> _visibleIndices; ///< Indices of items matching the filter.
    std::size_t _selectedVisibleIndex = 0;    ///< Index into _visibleIndices.
    mutable std::size_t _scrollOffset = 0;    ///< First visible item in scroll window.
    std::string _filter;
    ListStyle _style;
    bool _multiSelect = false;  ///< Whether multi-select (checkbox) mode is enabled.
    std::vector<bool> _checked; ///< Parallel to _items, tracks checked state.

    void rebuildVisibleIndices();
    void ensureSelectionVisible(int maxRows) const;
    [[nodiscard]] auto matchesFilter(ListItem const& item) const -> bool;
    [[nodiscard]] auto handleKey(KeyEvent const& key) -> ListAction;
};

/// @brief Returns a default list style with sensible defaults.
[[nodiscard]] auto defaultListStyle() -> ListStyle;

} // namespace core::tui
