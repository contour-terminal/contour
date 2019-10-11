/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
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
#include <terminal/Selector.h>
#include <terminal/Terminal.h>

using namespace std;

namespace terminal {

Selector::Selector(Mode _mode,
				   GetCellAt _getCellAt,
				   std::u32string const& _wordDelimiters,
				   cursor_pos_t _totalRowCount,
				   WindowSize const& _viewport,
				   Coordinate const& _from) :
	mode_{_mode},
	getCellAt_{move(_getCellAt)},
	wordDelimiters_{_wordDelimiters},
	totalRowCount_{_totalRowCount},
	viewport_{_viewport},
	start_{_from},
	from_{_from},
	to_{_from}
{
	if (_mode == Mode::FullLine)
	{
		extend({from_.row, 1u});
		swapDirection();
		extend({from_.row, viewport_.columns});
	}
	else if (isWordWiseSelection())
	{
		state_ = State::InProgress;
		extendSelectionBackward();
		swapDirection();
		extendSelectionForward();
	}
}

bool Selector::extend(Coordinate const& _coord)
{
    assert(state_ != State::Complete && "In order extend a selection, the selector must be active (started).");

    state_ = State::InProgress;

	if (!isWordWiseSelection())
		to_ = _coord;

	else if (_coord > start_)
	{
		to_ = _coord;
		extendSelectionForward();
	}
	else
	{
		to_ = _coord;
		extendSelectionBackward();
		swapDirection();
		to_ = start_;
		extendSelectionForward();
	}

    // TODO: indicates whether or not a scroll action must take place.
    return false;
}

void Selector::extendSelectionBackward()
{
    auto const isWordDelimiterAt = [this](Coordinate const& _coord) -> bool {
        auto const& cell = at(_coord);
        return !cell.character || wordDelimiters_.find(cell.character) != wordDelimiters_.npos;
    };

    auto last = to_;
    auto current = last;
    for (;;) {
        if (current.column > 1)
            current.column--;
        else if (current.row > 1)
        {
            current.row--;
            current.column = viewport_.columns;
        }
        else
            break;

        if (isWordDelimiterAt(current))
            break;
        last = current;
    }

    if (to_ < from_)
    {
        swapDirection();
		to_ = last;
    }
    else
        to_ = last;
}

void Selector::extendSelectionForward()
{
    auto const isWordDelimiterAt = [this](Coordinate const& _coord) -> bool {
        auto const& cell = at(_coord);
        return !cell.character || wordDelimiters_.find(cell.character) != wordDelimiters_.npos;
    };

    auto last = to_;
    auto current = last;
    for (;;) {
        if (current.column < viewport_.columns)
            current.column++;
        else if (current.row < totalRowCount_)
        {
            current.row++;
            current.column = 1;
        }
        else
            break;

        if (isWordDelimiterAt(current))
            break;
        last = current;
    }
	to_ = last;
}

void Selector::stop()
{
    if (state_ == State::InProgress)
        state_ = State::Complete;
}

tuple<vector<Selector::Range>, Coordinate const, Coordinate const> prepare(Selector const& _selector)
{
    vector<Selector::Range> result;

    auto const [from, to] = [&]() {
        if (_selector.to() < _selector.from())
            return pair{_selector.to(), _selector.from()};
        else
            return pair{_selector.from(), _selector.to()};
    }();

    auto const numLines = to.row - from.row + 1;
    result.resize(numLines);

    return {move(result), from, to};
}

vector<Selector::Range> Selector::selection() const
{
	switch (mode_)
	{
		case Mode::FullLine:
			return lines();
		case Mode::Linear:
		case Mode::LinearWordWise:
			return linear();
		case Mode::Rectangular:
			return rectangular();
	}
	return {};
}

void Selector::render(Selector::Renderer _render)
{
    for (auto const& range : selection())
        for (auto col = range.fromColumn; col <= range.toColumn; ++col)
            _render(range.line, col, at({range.line, col}));
}

vector<Selector::Range> Selector::linear() const
{
    auto [result, from, to] = prepare(*this);

    switch (result.size())
    {
        case 1:
            result[0] = Range{from.row, from.column, to.column};
            break;
        case 2:
            // Render first line partial from selected column to end.
            result[0] = Range{from.row, from.column, viewport().columns};
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Range{to.row, 1, to.column};
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Range{from.row, from.column, viewport().columns};

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Range{from.row + static_cast<cursor_pos_t>(n), 1, viewport().columns};

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Range{to.row, 1, to.column};
            break;
    }

    return result;
}

vector<Selector::Range> Selector::lines() const
{
    auto [result, from, to] = prepare(*this);

    for (cursor_pos_t row = 0; row < result.size(); ++row)
    {
        result[row] = Range{
            from.row + row,
            1,
            viewport().columns
        };
    }

    return result;
}

vector<Selector::Range> Selector::rectangular() const
{
    auto [result, from, to] = prepare(*this);

    for (cursor_pos_t row = 0; row < result.size(); ++row)
    {
        result[row] = Range{
            from.row + row,
            from.column,
            to.column
        };
    }

    return result;
}

} // namespace terminal
