// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Box.hpp>
#include <core/tui/CommandRegistry.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/InputField.hpp>
#include <core/tui/ScrollableSelection.hpp>
#include <core/tui/TuiCoreExport.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace core::tui
{

/// @brief Result of processing an input event in CommandPalettePopup.
enum class CommandPaletteAction : std::uint8_t
{
    Changed,   ///< Filter or selection changed, re-render needed.
    Executed,  ///< User executed the selected command.
    Dismissed, ///< User dismissed the palette (Escape).
};

/// @brief A popup command palette with fuzzy search filtering.
///
/// Renders a bordered popup with a filter input field at the top and a
/// scrollable list of commands below. Commands are fuzzy-matched against
/// the filter text using completer::FuzzyMatch::matchSmartCase(). Each item shows
/// category, label (with match highlighting), description, and keybinding hint.
///
/// Follows the CompletionPopup pattern: manages its own visibility state,
/// handles keyboard navigation, and renders using the Canvas API.
class CORE_TUI_CORE_API CommandPalettePopup: public Component
{
  public:
    /// @brief Constructs a hidden palette timed by the process's steady clock.
    CommandPalettePopup();

    /// @brief Constructs a hidden palette whose filter field is timed by @p clock.
    /// @param clock The clock the filter field measures on (not owned; must outlive the palette).
    explicit CommandPalettePopup(IClock const& clock CORE_LIFETIMEBOUND);

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    explicit CommandPalettePopup(IClock const&& clock) = delete;

    /// @brief Constructs a hidden palette framed in @p border, its filter field timed by @p clock.
    /// @param border The frame's style.
    /// @param clock The clock the filter field measures on (not owned; must outlive the palette).
    explicit CommandPalettePopup(BorderStyle border,
                                 IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock());

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    CommandPalettePopup(BorderStyle border, IClock const&& clock) = delete;

    /// @brief Returns the style the palette is framed in.
    [[nodiscard]] BorderStyle border() const noexcept { return _border; }

    ~CommandPalettePopup() override = default;

    /// @brief Returns the clock the input field's multi-clicks are measured on.
    [[nodiscard]] IClock const& clock() const noexcept { return _filterField.clock(); }

    // --- Component Interface ---

    /// @brief Renders the palette to the canvas.
    void render(Canvas& canvas) override;

    /// @brief Handles input events via Component interface.
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    /// @brief CommandPalettePopup can receive keyboard focus.
    [[nodiscard]] bool focusable() const override { return true; }

    /// @brief Returns preferred size based on content.
    [[nodiscard]] Size preferredSize() const override;

    // ========================================================================
    // Visibility and Items
    // ========================================================================

    /// @brief Shows the palette with commands from the given registry and context.
    /// @param registry The command registry to display.
    /// @param context The current mode context for filtering.
    void show(CommandRegistry const& registry, CommandContext context);

    /// @brief Shows a sub-menu with a custom set of commands and title.
    /// @param items The commands to display.
    /// @param title Title shown in the filter field prompt.
    void showSubMenu(std::vector<CommandEntry> items, std::string_view title);

    /// @brief Hides the palette and clears state.
    void hide();

    /// @brief Returns whether the palette is currently visible.
    [[nodiscard]] bool visible() const noexcept override;

    // ========================================================================
    // Event Handling
    // ========================================================================

    /// @brief Processes an input event and returns the resulting action.
    /// @param event The input event to process.
    /// @return The resulting action.
    [[nodiscard]] CommandPaletteAction processEvent(InputEvent const& event);

    /// @brief Returns the rendered height (including borders).
    [[nodiscard]] int renderedHeight() const noexcept;

    /// @brief Returns the rendered width (including borders).
    [[nodiscard]] int renderedWidth() const noexcept;

  private:
    BorderStyle _border = BorderStyle::Rounded;

    /// @brief A filtered command item with fuzzy match positions for highlighting.
    struct FilteredItem
    {
        CommandEntry const* entry = nullptr; ///< Pointer to the original command entry.
        std::vector<size_t> matchPositions;  ///< Grapheme indices for label highlighting.
        int score = 0;                       ///< Fuzzy match score for sorting.
    };

    InputField _filterField;                            ///< Text input for filtering.
    std::vector<CommandEntry> _allItems;                ///< All available commands (snapshot).
    std::vector<FilteredItem> _filteredItems;           ///< Filtered and sorted commands.
    ScrollableSelection _selection { MaxVisibleItems }; ///< Selection and scroll state.
    bool _visible = false;                              ///< Whether the palette is shown.
    int _renderedHeight = 0;                            ///< Last rendered height.
    int _renderedWidth = 0;                             ///< Last rendered width.
    std::string _title;                                 ///< Optional title for sub-menus.

    static constexpr size_t MaxVisibleItems = 12; ///< Maximum number of visible items.
    static constexpr int MinPaletteWidth = 50;    ///< Minimum palette width.
    static constexpr int MaxPaletteWidth = 80;    ///< Maximum palette width.

    /// @brief Recomputes _filteredItems from _allItems and current filter text.
    void refilter();

    /// @brief Calculates the width needed for the palette.
    [[nodiscard]] int calculateWidth(int maxWidth) const;

    /// @brief Handles a key event for navigation and actions.
    [[nodiscard]] CommandPaletteAction handleKey(KeyEvent const& key);

    /// @brief Executes the currently selected command.
    void executeSelected();
};

} // namespace core::tui
