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

#include <terminal/InputGenerator.h>

#include <crispy/size.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <functional>
#include <vector>
#include <utility>

namespace terminal {

struct SelectionHelper
{
    virtual ~SelectionHelper() = default;
    virtual PageSize pageSize() const noexcept = 0;
    virtual bool wordDelimited(Coordinate _pos) const noexcept = 0;
    virtual bool wrappedLine(LineOffset _line) const noexcept = 0;
    virtual bool cellEmpty(Coordinate _pos) const noexcept = 0;
    virtual int cellWidth(Coordinate _pos) const noexcept = 0;
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
    enum class State
    {
        /// Inactive, but waiting for the selection to be started (by moving the cursor).
        Waiting,
        /// Active, with selection in progress.
        InProgress,
        /// Inactive, with selection available.
        Complete,
    };

    /// Defines a columnar range at a given line.
    struct Range
    {
        LineOffset line;
        ColumnOffset fromColumn;
        ColumnOffset toColumn;

        constexpr ColumnCount length() const noexcept
        {
            return boxed_cast<ColumnCount>(toColumn - fromColumn + 1);
        }
    };

    Selection(SelectionHelper const& _helper, Coordinate _start):
        helper_{_helper}, from_{_start}, to_{_start} {}

    virtual ~Selection() = default;

    constexpr Coordinate from() const noexcept { return from_; }
    constexpr Coordinate to() const noexcept { return to_; }

    /// @returns boolean indicating whether or not given absolute coordinate is within the range of the selection.
    virtual bool contains(Coordinate _coord) const noexcept;
    virtual bool intersects(Rect _area) const noexcept;

    /// Tests whether the a selection is currently in progress.
    constexpr State state() const noexcept { return state_; }

    /// Extends the selection to the given coordinate.
    virtual void extend(Coordinate _to);

	/// Constructs a vector of ranges for this selection.
	virtual std::vector<Range> ranges() const;

    /// Marks the selection as completed.
    void complete();

    /// Applies any scroll action to the line offsets.
    void applyScroll(LineOffset _value, LineCount _historyLineCount);

    static Coordinate stretchedColumn(SelectionHelper const& _helper, Coordinate _coord) noexcept;

protected:
    State state_ = State::Waiting;
    Coordinate from_;
    Coordinate to_;
    SelectionHelper const& helper_;
};

class RectangularSelection: public Selection
{
public:
    RectangularSelection(SelectionHelper const& _helper, Coordinate _start);
    bool contains(Coordinate _coord) const noexcept override;
    bool intersects(Rect _area) const noexcept override;
	std::vector<Range> ranges() const override;
};

class LinearSelection: public Selection
{
public:
    LinearSelection(SelectionHelper const& _helper, Coordinate _start);
};

class WordWiseSelection: public Selection
{
public:
    WordWiseSelection(SelectionHelper const& _helper, Coordinate _start);

    void extend(Coordinate _to) override;

    Coordinate extendSelectionBackward(Coordinate _pos) const noexcept;
    Coordinate extendSelectionForward(Coordinate _pos) const noexcept;
};

class FullLineSelection: public Selection
{
public:
    explicit FullLineSelection(SelectionHelper const& _helper, Coordinate _start);
    void extend(Coordinate _to) override;
};

template <typename Renderer>
void renderSelection(Selection const& _selection, Renderer&& _render);

// {{{ impl
inline void Selection::applyScroll(LineOffset _value, LineCount _historyLineCount)
{
    auto const n = -boxed_cast<LineOffset>(_historyLineCount);

    from_.line = std::max(from_.line - _value, n);
    to_.line = std::max(to_.line - _value, n);
}

template <typename Renderer>
void renderSelection(Selection const& _selection, Renderer&& _render)
{
    for (Selection::Range const& range: _selection.ranges())
        for (auto const col: crispy::times(*range.fromColumn, *range.length()))
            _render(Coordinate{range.line, ColumnOffset::cast_from(col)});
}
// }}}

} // namespace terminal

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Selection::State>
    {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }
        using State = terminal::Selection::State;
        template <typename FormatContext>
        auto format(State _state, FormatContext& ctx)
        {
            switch (_state)
            {
                case State::Waiting:
                    return format_to(ctx.out(), "Waiting");
                case State::InProgress:
                    return format_to(ctx.out(), "InProgress");
                case State::Complete:
                    return format_to(ctx.out(), "Complete");
            }
            return format_to(ctx.out(), "{}", static_cast<unsigned>(_state));
        }
    };

    template <>
    struct formatter<terminal::Selection>
    {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::Selection& _selector, FormatContext& ctx)
        {
            return format_to(ctx.out(),
                             "{}({} from {} to {})",
                             dynamic_cast<terminal::WordWiseSelection const*>(&_selector) ? "WordWiseSelection" :
                             dynamic_cast<terminal::FullLineSelection const*>(&_selector) ? "FullLineSelection" :
                             dynamic_cast<terminal::RectangularSelection const*>(&_selector) ? "RectangularSelection" :
                             dynamic_cast<terminal::LinearSelection const*>(&_selector) ? "LinearSelection" :
                             "Selection",
                             _selector.state(),
                             _selector.from(),
                             _selector.to());
        }
    };
} // }}}
