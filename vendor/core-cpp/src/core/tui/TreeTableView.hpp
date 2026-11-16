// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file TreeTableView.hpp
/// @brief Generic, data-driven tree/table browser component with vim-style navigation.
///
/// `TreeTableView` renders a scrollable, column-oriented table of the children of a
/// "current node" and lets the user move a cursor, descend into a child, ascend to the
/// parent, and re-sort — the classic ncdu / file-manager interaction, made reusable.
/// It owns no domain data: everything it shows and every action it takes is delegated
/// to an injected @ref TreeTableModel. A disk-usage browser, a process tree, a JSON
/// explorer, etc. all reuse this component by implementing the model.

#include <core/Clock.hpp>
#include <core/Defines.hpp>
#include <core/tui/Component.hpp>
#include <core/tui/InputEvent.hpp>
#include <core/tui/TerminalOutput.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace core::tui
{

/// Opaque handle identifying a row in the model's current view. The view never
/// interprets it; it only passes it back to the model.
using RowId = std::uint64_t;

/// How a column claims horizontal space.
enum class ColumnWidth : std::uint8_t
{
    Fixed, ///< Exactly @c width cells.
    Flex,  ///< At least @c width cells; absorbs leftover width (one Flex column).
};

/// Text alignment within a column.
enum class ColumnAlign : std::uint8_t
{
    Left,
    Right,
};

/// A column the view lays out. The cell text is produced by the model per row, so the
/// view stays domain-agnostic; the spec here is pure layout data.
struct TableColumn
{
    std::string_view header; ///< Header label.
    ColumnWidth width;       ///< Fixed or flex sizing.
    int size;                ///< Fixed width, or minimum width for Flex.
    ColumnAlign align;       ///< Cell alignment.
};

/// The styling the model wants applied to one row's text.
struct RowStyle
{
    Style style;      ///< Foreground/attributes for the row.
    bool selected {}; ///< Whether the row is the cursor row (the view may invert it).
};

/// The data + navigation seam a @ref TreeTableView renders and drives.
///
/// Implementations adapt any hierarchical, column-describable data to the view. The
/// view calls these to paint and to act; it holds no domain state itself (only the
/// cursor/scroll position).
class TreeTableModel
{
  public:
    virtual ~TreeTableModel() = default;

    /// @return The columns to lay out, left to right (layout data only).
    [[nodiscard]] virtual std::vector<TableColumn> columns() const = 0;

    /// @return The rows of the current node, in display (sorted) order.
    [[nodiscard]] virtual std::vector<RowId> rows() const = 0;

    /// @return The text for @p row in column index @p column.
    [[nodiscard]] virtual std::string cellText(RowId row, std::size_t column) const = 0;

    /// @return The style to render @p row with (color rules, etc.).
    [[nodiscard]] virtual RowStyle rowStyle(RowId row, bool selected) const = 0;

    /// @return true if @p row can be descended into (e.g. a non-empty directory).
    [[nodiscard]] virtual bool canDescend(RowId row) const = 0;

    /// Descends into @p row, making it the current node. No-op if not descendable.
    /// @return true if the current node changed.
    virtual bool descend(RowId row) = 0;

    /// Ascends to the parent of the current node. No-op at the top.
    /// @return true if the current node changed.
    virtual bool ascend() = 0;

    /// @return A one-line title for the current node (e.g. its path), for the header.
    [[nodiscard]] virtual std::string currentTitle() const = 0;

    /// Applies the sort the model associates with @p sortKey to the current rows.
    /// The view forwards a small integer chosen by the host's key bindings; the model
    /// decides what each key means (data-driven on the model side).
    /// @param sortKey Host-defined sort selector.
    virtual void sortBy(int sortKey) = 0;
};

/// Configuration knobs for a @ref TreeTableView (all data, no behavior).
struct TreeTableConfig
{
    bool showHeader = true; ///< Draw the column header row.
    int barColumn = -1;     ///< Index of a column the view fills as a proportional bar (-1 = none).
};

/// A generic, reusable tree/table browser. Owns only cursor + scroll state; all data
/// and navigation come from an injected @ref TreeTableModel.
class TreeTableView: public Component
{
  public:
    /// @param model The data/navigation seam (not owned; must outlive the view).
    /// @param config Layout configuration.
    /// @param clock The clock double-clicks are timed on (not owned; must outlive the view).
    explicit TreeTableView(TreeTableModel& model,
                           TreeTableConfig config = {},
                           IClock const& clock CORE_LIFETIMEBOUND = defaultSteadyClock()) noexcept;

    /// A clock is kept, not copied: a temporary would dangle, so an rvalue clock is refused.
    TreeTableView(TreeTableModel& model, TreeTableConfig config, IClock const&& clock) = delete;

    void render(Canvas& canvas) override;
    [[nodiscard]] EventResult onEvent(InputEvent const& event) override;

    [[nodiscard]] bool focusable() const override { return true; }

    /// Moves the cursor by @p delta rows, clamped to the row range.
    void moveCursor(int delta);

    /// Moves the cursor to the first / last row.
    void moveToTop();
    void moveToBottom();

    /// Scrolls by a page (viewport height) in @p direction (-1 up, +1 down).
    void pageBy(int direction);

    /// Scrolls by half a page (viewport height / 2) in @p direction (-1 up, +1 down),
    /// like vim's Ctrl+U / Ctrl+D.
    void halfPageBy(int direction);

    /// Descends into the cursor row (if descendable), resetting the cursor.
    void descendCursor();

    /// Ascends to the parent of the current node, restoring the remembered cursor.
    void ascendCursor();

    /// Forwards @p sortKey to the model and keeps the cursor on the same row when possible.
    void sortBy(int sortKey);

    /// @return The row currently under the cursor, or 0 with @p valid=false if empty.
    [[nodiscard]] RowId cursorRow(bool& valid) const;

    /// @return The cursor's index within the current rows (0-based).
    [[nodiscard]] std::size_t cursorIndex() const noexcept { return _cursor; }

    /// Callback invoked when the cursor row changes or the current node changes.
    void onSelectionChanged(std::function<void()> callback) { _onChanged = std::move(callback); }

  private:
    /// Clamps the cursor and scroll offset to the current row count and viewport.
    void clampCursor();

    /// Renders one row at canvas line @p line for model row @p row.
    void renderRow(Canvas& canvas, int line, RowId row, bool selected, std::vector<int> const& widths);

    /// Computes per-column widths for canvas width @p totalWidth (distributes Flex).
    [[nodiscard]] std::vector<int> computeWidths(int totalWidth) const;

    /// Moves the cursor to row index @p index (clamped) and notifies listeners.
    void selectIndex(std::size_t index);

    /// Maps a component-relative, 1-based @p row coordinate to a model row index.
    /// @param row The component-relative row (1-based, as delivered in a MouseEvent).
    /// @return The row index, or std::nullopt if @p row is the header or below the last row.
    [[nodiscard]] std::optional<std::size_t> rowIndexAt(int row) const;

    TreeTableModel& _model;           ///< Data + navigation seam.
    TreeTableConfig _config;          ///< Layout configuration.
    IClock const& _clock;             ///< The clock double-clicks are timed on.
    std::size_t _cursor = 0;          ///< Cursor index into the current rows.
    std::size_t _scroll = 0;          ///< Index of the first visible row.
    std::function<void()> _onChanged; ///< Selection/navigation change callback.

    /// Remembered cursor index per current-node depth, restored on ascend. Keyed by the
    /// model's title so revisiting a node returns to where the user left off.
    std::vector<std::pair<std::string, std::size_t>> _cursorHistory;

    // Double-click tracking: a left-press on the already-clicked row within the interval
    // activates (descends into) it, like a desktop double-click.
    bool _hadClick = false;         ///< A prior left-press may begin a double-click.
    std::size_t _lastClickRow = 0;  ///< Row index of the prior left-press.
    SteadyTimePoint _lastClickTime; ///< When the prior left-press happened.

    static constexpr auto DoubleClickInterval = std::chrono::milliseconds { 400 };
};

} // namespace core::tui
