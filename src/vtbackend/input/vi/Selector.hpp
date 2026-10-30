// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/input/InputGenerator.hpp>
#include <vtbackend/input/vi/ViInputHandler.hpp>

#include <crispy/Size.hpp>

#include <core/Times.hpp>
#include <core/Utils.hpp>

#include <cstddef>
#include <format>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

namespace vtbackend
{

struct SelectionHelper
{
    virtual ~SelectionHelper() = default;
    std::function<bool(CellLocation)> wordDelimited;
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;
    /// Whether row @p line continues the row above it after a soft wrap, that row still being in the grid.
    [[nodiscard]] virtual bool wrappedLine(LineOffset line) const noexcept = 0;
    [[nodiscard]] virtual bool cellEmpty(CellLocation pos) const noexcept = 0;
    [[nodiscard]] virtual int cellWidth(CellLocation pos) const noexcept = 0;
    /// Whether the cell at @p pos holds text of its own: not the second cell of a wide character, a cell a
    /// text-sizing block reaches into, or the padding a wide character left at a row's end
    /// (NoTextCellFlags) -- the cells a copy leaves out.
    [[nodiscard]] virtual bool cellHoldsText(CellLocation pos) const noexcept = 0;
};

/// The most cells Selection::extent() looks at to count a selection's characters: a larger selection
/// reports its lines alone. The status line asks on every frame, under the terminal's lock, so the count
/// must cost a bounded amount however much is selected.
inline constexpr std::size_t SelectionCharacterCountLimit = 10'000;

/// How much a selection holds, as the status line says it.
struct SelectionExtent
{
    /// The rows it spans, the first and the last included.
    LineCount lines {};
    /// The characters a copy of it yields -- each row's cells up to its last non-blank one, the spaces
    /// between words included, a cell with no text of its own (a wide character's second cell) not -- or
    /// nullopt when it spans @ref SelectionCharacterCountLimit cells or more.
    std::optional<std::size_t> characters {};

    bool operator==(SelectionExtent const&) const = default;
};

/**
 * Selector API.
 *
 * A Selector can select a range of text. The range can be linear with partial start/end lines, or full lines,
 * or a block based selector, that is capable of selecting all lines partially.
 *
 * The Selector operates on the Screen by accumulating a scrolling offset, that determines
 * the view port of that Screen.
 *
 * When the screen is being modified while selecting text, the selection regions must be preserved,
 * that is, when the selection start is inside the screen, then new lines are added, which causes the screen
 * to move the screen contents up, then also the selection's begin (and extend) is being moved up.
 *
 * This is achieved by using absolute coordinates from the top history line.
 *
 * How Selection usually works
 * ===========================
 *
 * First mouse press ->
 * Second mouse press AND on same coordinate as first mouse press -> selects word
 * Third mouse press AND on same coordinate as prior mouse presses -> reselects line
 * Mouse moves -> resets last recorded mouse press coordinate
 */
class Selection
{
  public:
    enum class State : uint8_t
    {
        /// Inactive, but waiting for the selection to be started (by moving the cursor).
        Waiting,
        /// Active, with selection in progress.
        InProgress,
        /// Inactive, with selection available.
        Complete,
    };

    /// Defines a columnar range at a given line.
    using Range = ColumnRange;

    using OnSelectionUpdated = std::function<void()>;

    Selection(SelectionHelper const& helper,
              ViMode viMode,
              CellLocation start,
              OnSelectionUpdated onSelectionUpdated):
        _helper { helper },
        _viMode { viMode },
        _onSelectionUpdated { std::move(onSelectionUpdated) },
        _from { start },
        _to { start }
    {
    }

    virtual ~Selection() = default;

    constexpr CellLocation from() const noexcept { return _from; }
    constexpr CellLocation to() const noexcept { return _to; }

    /// @returns boolean indicating whether or not given absolute coordinate is within the range of the
    /// selection.
    [[nodiscard]] virtual bool contains(CellLocation coord) const noexcept;
    [[nodiscard]] bool containsLine(LineOffset line) const noexcept;
    [[nodiscard]] virtual bool intersects(Rect area) const noexcept;

    [[nodiscard]] ViMode viMode() const noexcept { return _viMode; }

    /// Tests whether the a selection is currently in progress.
    [[nodiscard]] constexpr State state() const noexcept { return _state; }

    /// Extends the selection to the given coordinate.
    [[nodiscard]] virtual bool extend(CellLocation to);

    /// Constructs a vector of ranges for this selection.
    [[nodiscard]] virtual std::vector<Range> ranges() const;

    /// How much this selection holds: its rows always, in O(1), and its characters as a copy yields them
    /// only while its rows times rowWidth() are fewer than @ref SelectionCharacterCountLimit cells -- a
    /// bound checked before any cell is read or ranges() is asked, so a call reads at most that many
    /// cells (twice: once to find each row's last non-blank cell, once to count) and allocates at most
    /// that many rows' Range. The status line calls it on every frame, under the terminal's lock.
    /// @return The extent.
    [[nodiscard]] SelectionExtent extent() const;

    /// Marks the selection as completed.
    void complete();

    /// Applies any scroll action to the line offsets.
    void applyScroll(LineOffset value, LineCount historyLineCount);

    static CellLocation stretchedColumn(SelectionHelper const& gridHelper, CellLocation coord) noexcept;

  protected:
    /// The widest any of this selection's rows is, in O(1): the page's width, as a linear selection's
    /// inner rows span it whole.
    /// @return The width.
    [[nodiscard]] virtual ColumnCount rowWidth() const noexcept;

    State _state = State::Waiting;
    SelectionHelper const& _helper;
    ViMode _viMode;
    OnSelectionUpdated _onSelectionUpdated;
    CellLocation _from;
    CellLocation _to;
};

class RectangularSelection: public Selection
{
  public:
    RectangularSelection(SelectionHelper const& helper,
                         CellLocation start,
                         OnSelectionUpdated onSelectionUpdated);
    [[nodiscard]] bool contains(CellLocation coord) const noexcept override;
    [[nodiscard]] bool intersects(Rect area) const noexcept override;
    [[nodiscard]] std::vector<Range> ranges() const override;

  protected:
    /// The rectangle's own width. A wide character on its right edge stretches a row by one more column,
    /// which the bound does not count.
    /// @return The width.
    [[nodiscard]] ColumnCount rowWidth() const noexcept override;
};

class LinearSelection final: public Selection
{
  public:
    LinearSelection(SelectionHelper const& helper, CellLocation start, OnSelectionUpdated onSelectionUpdated);
};

class WordWiseSelection final: public Selection
{
  public:
    WordWiseSelection(SelectionHelper const& helper,
                      CellLocation start,
                      OnSelectionUpdated onSelectionUpdated);

    bool extend(CellLocation to) override;

    [[nodiscard]] CellLocation extendSelectionBackward(CellLocation pos) const noexcept;
    [[nodiscard]] CellLocation extendSelectionForward(CellLocation pos) const noexcept;
};

class FullLineSelection final: public Selection
{
  public:
    explicit FullLineSelection(SelectionHelper const& helper,
                               CellLocation start,
                               OnSelectionUpdated onSelectionUpdated);
    bool extend(CellLocation to) override;
};

template <typename Renderer>
void renderSelection(Selection const& selection, Renderer&& render);

// {{{ impl
inline void Selection::applyScroll(LineOffset value, LineCount historyLineCount)
{
    auto const n = -boxed_cast<LineOffset>(historyLineCount);

    _from.line = std::max(_from.line - value, n);
    _to.line = std::max(_to.line - value, n);
}

template <typename Renderer>
void renderSelection(Selection const& selection, Renderer&& render)
{
    // @p render is invoked once per selected cell. Forward it once into a reference binding (which
    // consumes nothing) and call through that, rather than forwarding inside the loop -- the latter
    // would move from it on the first cell and use it again on every following one.
    auto&& renderer = std::forward<Renderer>(render);
    for (Selection::Range const& range: selection.ranges())
        for (auto const col: core::times(*range.fromColumn, *range.length()))
            renderer(CellLocation { .line = range.line, .column = ColumnOffset::cast_from(col) });
}
// }}}

} // namespace vtbackend

// {{{ fmt custom formatter support
template <>
struct std::formatter<vtbackend::Selection::State>: formatter<std::string_view>
{
    using State = vtbackend::Selection::State;
    auto format(State state, auto& ctx) const
    {
        string_view name;
        switch (state)
        {
            case State::Waiting: name = "Waiting"; break;
            case State::InProgress: name = "InProgress"; break;
            case State::Complete: name = "Complete"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct std::formatter<vtbackend::Selection>: formatter<std::string>
{
    auto format(vtbackend::Selection const& selector, auto& ctx) const
    {
        return formatter<std::string>::format(
            std::format(
                "{}({} from {} to {})",
                [](auto const* selector) -> std::string_view {
                    if (dynamic_cast<vtbackend::WordWiseSelection const*>(selector))
                        return "WordWiseSelection";
                    if (dynamic_cast<vtbackend::FullLineSelection const*>(selector))
                        return "FullLineSelection";
                    if (dynamic_cast<vtbackend::RectangularSelection const*>(selector))
                        return "RectangularSelection";
                    if (dynamic_cast<vtbackend::LinearSelection const*>(selector))
                        return "LinearSelection";
                    return "Selection";
                }(&selector),
                selector.state(),
                selector.from(),
                selector.to()),
            ctx);
    }
};
// }}}
