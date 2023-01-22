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

struct SelectionHelper
{
    virtual ~SelectionHelper() = default;
    [[nodiscard]] virtual PageSize pageSize() const noexcept = 0;
    [[nodiscard]] virtual bool wordDelimited(CellLocation pos) const noexcept = 0;
    [[nodiscard]] virtual bool wrappedLine(LineOffset line) const noexcept = 0;
    [[nodiscard]] virtual bool cellEmpty(CellLocation pos) const noexcept = 0;
    [[nodiscard]] virtual int cellWidth(CellLocation pos) const noexcept = 0;
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

    /// Include the given pos
    [[nodiscard]] virtual bool extendStart(CellLocation from);

    /// Constructs a vector of ranges for this selection.
    [[nodiscard]] virtual std::vector<Range> ranges() const;

    /// Marks the selection as completed.
    void complete();

    /// Applies any scroll action to the line offsets.
    void applyScroll(LineOffset value, LineCount historyLineCount);

    static CellLocation stretchedColumn(SelectionHelper const& gridHelper, CellLocation coord) noexcept;

  protected:
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
    for (Selection::Range const& range: selection.ranges())
        for (auto const col: crispy::times(*range.fromColumn, *range.length()))
            render(CellLocation { range.line, ColumnOffset::cast_from(col) });
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
    auto format(State state, FormatContext& ctx)
    {
        switch (state)
        {
            case State::Waiting: return fmt::format_to(ctx.out(), "Waiting");
            case State::InProgress: return fmt::format_to(ctx.out(), "InProgress");
            case State::Complete: return fmt::format_to(ctx.out(), "Complete");
        }
        return fmt::format_to(ctx.out(), "{}", static_cast<unsigned>(state));
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
    auto format(const terminal::Selection& selector, FormatContext& ctx)
    {
        return fmt::format_to(
            ctx.out(),
            "{}({} from {} to {})",
            dynamic_cast<terminal::WordWiseSelection const*>(&selector)      ? "WordWiseSelection"
            : dynamic_cast<terminal::FullLineSelection const*>(&selector)    ? "FullLineSelection"
            : dynamic_cast<terminal::RectangularSelection const*>(&selector) ? "RectangularSelection"
            : dynamic_cast<terminal::LinearSelection const*>(&selector)      ? "LinearSelection"
                                                                             : "Selection",
            selector.state(),
            selector.from(),
            selector.to());
    }
};
} // namespace fmt
