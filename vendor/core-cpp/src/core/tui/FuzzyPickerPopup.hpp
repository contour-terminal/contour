// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Box.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/InputField.hpp>
#include <core/tui/ScrollableSelection.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// @brief Result of processing an input event in FuzzyPickerPopup.
enum class FuzzyPickerAction : std::uint8_t
{
    Changed,   ///< Filter or selection changed, re-render needed.
    Accepted,  ///< User accepted the selected item (Enter).
    Dismissed, ///< User dismissed the picker (Escape).
};

/// @brief A popup fuzzy picker for selecting from a list of string items.
///
/// Renders a bordered popup with a filter input field at the top and a
/// scrollable list of items below. Items are fuzzy-matched against
/// the filter text using completer::FuzzyMatch::matchSmartCase(). Each item shows
/// its text with matched character highlighting.
///
/// Designed to be used for file path selection (fzf replacement) but is
/// generic enough for any list of strings.
class FuzzyPickerPopup: public Component
{
  public:
    /// @brief Constructs a hidden picker timed by the process's steady clock.
    FuzzyPickerPopup();

    /// @brief Constructs a hidden picker whose filter field is timed by @p clock.
    /// @param clock The clock the filter field measures on (not owned; must outlive the picker).
    explicit FuzzyPickerPopup(IClock const& clock CORE_LIFETIMEBOUND);

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    explicit FuzzyPickerPopup(IClock const&& clock) = delete;

    /// @brief Constructs a hidden picker framed in @p border, its filter field timed by @p clock.
    /// @param border The frame's style.
    /// @param clock The clock the filter field measures on (not owned; must outlive the picker).
    explicit FuzzyPickerPopup(BorderStyle border,
                              IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    FuzzyPickerPopup(BorderStyle border, IClock const&& clock) = delete;

    /// @brief Returns the style the picker is framed in.
    [[nodiscard]] BorderStyle border() const noexcept { return _border; }

    ~FuzzyPickerPopup() override = default;

    /// @brief Returns the clock the input field's multi-clicks are measured on.
    [[nodiscard]] IClock const& clock() const noexcept { return _filterField.clock(); }

    // --- Component Interface ---

    /// @brief Renders the picker to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief FuzzyPickerPopup can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size based on content.
    [[nodiscard]] Size preferredSize() const override;

    // ========================================================================
    // Visibility and Items
    // ========================================================================

    /// @brief Shows the picker with the given items.
    ///
    /// Sets Component visibility to true and populates the item list.
    /// @param items The items to display and filter.
    /// @param title Optional title shown in the filter field prompt (e.g., "File> ").
    void show(std::vector<std::string> items, std::string_view title = "");

    /// @brief Hides the picker and clears state.
    ///
    /// Sets Component visibility to false.
    void hide();

    /// @brief Returns a pointer to the currently selected item, or nullptr if none.
    [[nodiscard]] std::string const* selectedItem() const;

    // ========================================================================
    // Event Handling
    // ========================================================================

    /// @brief Processes an input event and returns the resulting action.
    /// @param event The input event to process.
    /// @return The resulting action.
    [[nodiscard]] FuzzyPickerAction processEvent(InputEvent const& event);

    /// @brief Returns the rendered height (including borders).
    [[nodiscard]] int renderedHeight() const noexcept;

    /// @brief Returns the rendered width (including borders).
    [[nodiscard]] int renderedWidth() const noexcept;

  private:
    BorderStyle _border = BorderStyle::Rounded;

    /// @brief A filtered item with fuzzy match positions for highlighting.
    struct FilteredItem
    {
        std::string const* item = nullptr;  ///< Pointer to the original item string.
        std::vector<size_t> matchPositions; ///< Grapheme indices for highlighting.
        int score = 0;                      ///< Fuzzy match score for sorting.
    };

    InputField _filterField;                            ///< Text input for filtering.
    std::vector<std::string> _allItems;                 ///< All available items (owned).
    std::vector<FilteredItem> _filteredItems;           ///< Filtered and sorted items.
    ScrollableSelection _selection { MaxVisibleItems }; ///< Selection and scroll state.
    int _renderedHeight = 0;                            ///< Last rendered height.
    int _renderedWidth = 0;                             ///< Last rendered width.
    std::string _title;                                 ///< Optional title for the filter prompt.

    static constexpr size_t MaxVisibleItems = 15; ///< Maximum number of visible items.
    static constexpr int MinPickerWidth = 50;     ///< Minimum picker width.
    static constexpr int MaxPickerWidth = 100;    ///< Maximum picker width.

    /// @brief Recomputes _filteredItems from _allItems and current filter text.
    void refilter();

    /// @brief Calculates the width needed for the picker.
    [[nodiscard]] int calculateWidth(int maxWidth) const;

    /// @brief Handles a key event for navigation and actions.
    [[nodiscard]] FuzzyPickerAction handleKey(KeyEvent const& key);
};

} // namespace core::tui
