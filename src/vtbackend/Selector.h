/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <vtbackend/InputGenerator.h>
#include <vtbackend/ViInputHandler.h>

#include <crispy/size.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <functional>
#include <utility>
#include <vector>

namespace terminal
{

struct selection_helper
{
    virtual ~selection_helper() = default;
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;
    [[nodiscard]] virtual bool wordDelimited(cell_location pos) const noexcept = 0;
    [[nodiscard]] virtual bool wrappedLine(line_offset line) const noexcept = 0;
    [[nodiscard]] virtual bool cellEmpty(cell_location pos) const noexcept = 0;
    [[nodiscard]] virtual int cellWidth(cell_location pos) const noexcept = 0;
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
class selection
{
  public:
    enum class state
    {
        /// Inactive, but waiting for the selection to be started (by moving the cursor).
        Waiting,
        /// Active, with selection in progress.
        InProgress,
        /// Inactive, with selection available.
        Complete,
    };

    /// Defines a columnar range at a given line.
    using range = column_range;

    using on_selection_updated = std::function<void()>;

    selection(selection_helper const& helper,
              vi_mode viMode,
              cell_location start,
              on_selection_updated onSelectionUpdated):
        _helper { helper },
        _viMode { viMode },
        _onSelectionUpdated { std::move(onSelectionUpdated) },
        _from { start },
        _to { start }
    {
    }

    virtual ~selection() = default;

    constexpr cell_location from() const noexcept { return _from; }
    constexpr cell_location to() const noexcept { return _to; }

    /// @returns boolean indicating whether or not given absolute coordinate is within the range of the
    /// selection.
    [[nodiscard]] virtual bool contains(cell_location coord) const noexcept;
    [[nodiscard]] bool containsLine(line_offset line) const noexcept;
    [[nodiscard]] virtual bool intersects(rect area) const noexcept;

    [[nodiscard]] vi_mode viMode() const noexcept { return _viMode; }

    /// Tests whether the a selection is currently in progress.
    [[nodiscard]] constexpr state getState() const noexcept { return _state; }

    /// Extends the selection to the given coordinate.
    [[nodiscard]] virtual bool extend(cell_location to);

    /// Constructs a vector of ranges for this selection.
    [[nodiscard]] virtual std::vector<range> ranges() const;

    /// Marks the selection as completed.
    void complete();

    /// Applies any scroll action to the line offsets.
    void applyScroll(line_offset value, LineCount historyLineCount);

    static cell_location stretchedColumn(selection_helper const& gridHelper, cell_location coord) noexcept;

  protected:
    state _state = state::Waiting;
    selection_helper const& _helper;
    vi_mode _viMode;
    on_selection_updated _onSelectionUpdated;
    cell_location _from;
    cell_location _to;
};

class rectangular_selection: public selection
{
  public:
    rectangular_selection(selection_helper const& helper,
                          cell_location start,
                          on_selection_updated onSelectionUpdated);
    [[nodiscard]] bool contains(cell_location coord) const noexcept override;
    [[nodiscard]] bool intersects(rect area) const noexcept override;
    [[nodiscard]] std::vector<range> ranges() const override;
};

class linear_selection final: public selection
{
  public:
    linear_selection(selection_helper const& helper,
                     cell_location start,
                     on_selection_updated onSelectionUpdated);
};

class word_wise_selection final: public selection
{
  public:
    word_wise_selection(selection_helper const& helper,
                        cell_location start,
                        on_selection_updated onSelectionUpdated);

    bool extend(cell_location to) override;

    [[nodiscard]] cell_location extendSelectionBackward(cell_location pos) const noexcept;
    [[nodiscard]] cell_location extendSelectionForward(cell_location pos) const noexcept;
};

class full_line_selection final: public selection
{
  public:
    explicit full_line_selection(selection_helper const& helper,
                                 cell_location start,
                                 on_selection_updated onSelectionUpdated);
    bool extend(cell_location to) override;
};

template <typename Renderer>
void renderSelection(selection const& selection, Renderer&& render);

// {{{ impl
inline void selection::applyScroll(line_offset value, LineCount historyLineCount)
{
    auto const n = -boxed_cast<line_offset>(historyLineCount);

    _from.line = std::max(_from.line - value, n);
    _to.line = std::max(_to.line - value, n);
}

template <typename Renderer>
void renderSelection(selection const& selection, Renderer&& render)
{
    for (selection::range const& range: selection.ranges())
        for (auto const col: crispy::times(*range.fromColumn, *range.length()))
            render(cell_location { range.line, column_offset::cast_from(col) });
}
// }}}

} // namespace terminal

// {{{ fmtlib custom formatter support
template <>
struct fmt::formatter<terminal::selection::state>: formatter<std::string_view>
{
    using state = terminal::selection::state;
    auto format(state state, format_context& ctx) -> format_context::iterator
    {
        string_view name;
        switch (state)
        {
            case state::Waiting: name = "Waiting"; break;
            case state::InProgress: name = "InProgress"; break;
            case state::Complete: name = "Complete"; break;
        }
        return formatter<string_view>::format(name, ctx);
    }
};

template <>
struct fmt::formatter<terminal::selection>: formatter<std::string>
{
    auto format(const terminal::selection& selector, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(
            fmt::format("{}({} from {} to {})",
                        dynamic_cast<terminal::word_wise_selection const*>(&selector)   ? "WordWiseSelection"
                        : dynamic_cast<terminal::full_line_selection const*>(&selector) ? "FullLineSelection"
                        : dynamic_cast<terminal::rectangular_selection const*>(&selector)
                            ? "RectangularSelection"
                        : dynamic_cast<terminal::linear_selection const*>(&selector) ? "LinearSelection"
                                                                                     : "Selection",
                        selector.getState(),
                        selector.from(),
                        selector.to()),
            ctx);
    }
};
// }}}
