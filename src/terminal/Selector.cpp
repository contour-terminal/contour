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
#include <terminal/Selector.h>
#include <terminal/Screen.h>
#include <terminal/Terminal.h>
#include <crispy/times.h>
#include <cassert>

using namespace std;

namespace terminal {

Selector::Selector(Mode _mode,
				   GetCellAt _getCellAt,
				   std::u32string const& _wordDelimiters,
				   int _totalRowCount,
				   int _columnCount,
				   Coordinate const& _from) :
	mode_{_mode},
	getCellAt_{move(_getCellAt)},
	wordDelimiters_{_wordDelimiters},
	totalRowCount_{_totalRowCount},
    columnCount_{_columnCount},
	start_{_from},
	from_{_from},
	to_{_from}
{
	if (_mode == Mode::FullLine)
	{
		extend({from_.row, 1u});
		swapDirection();
		extend({from_.row, columnCount_});
	}
	else if (isWordWiseSelection())
	{
		state_ = State::InProgress;
		extendSelectionBackward();
		swapDirection();
		extendSelectionForward();
	}
}

Selector::Selector(Mode _mode,
                   std::u32string const& _wordDelimiters,
                   Screen const& _screen,
                   Coordinate const& _from) :
    Selector{
        _mode,
        [screen = std::ref(_screen)](Coordinate const& _pos) -> Cell const* {
            assert(_pos.row >= 0 && "must be absolute coordinate");
            auto const& buffer = screen.get();
            // convert line number  from absolute line to relative line number.
            auto const row = _pos.row - buffer.historyLineCount() + 1;
            if (row <= buffer.size().height)
                return &buffer.at({row, _pos.column});
            else
                return nullptr;
        },
        _wordDelimiters,
        _screen.size().height + static_cast<int>(_screen.historyLineCount()),
        _screen.size().width,
        _from
    }
{
}

Coordinate Selector::stretchedColumn(Coordinate _coord) const noexcept
{
    Coordinate stretched = _coord;
    if (Cell const* cell = at(_coord); cell && cell->width() > 1)
    {
        // wide character
        stretched.column += cell->width() - 1;
        return stretched;
    }

    while (stretched.column < columnCount_)
    {
        if (Cell const* cell = at(stretched); cell)
        {
            if (cell->empty())
                stretched.column++;
            else
            {
                if (cell->width() > 1)
                    stretched.column += cell->width() - 1;
                break;
            }
        }
        else
            break;
    }

    return stretched;
}

bool Selector::extend(Coordinate const& _coord)
{
    assert(state_ != State::Complete && "In order extend a selection, the selector must be active (started).");

    auto const coord = Coordinate{
        _coord.row,
        clamp(_coord.column, 1, columnCount_)
    };

    state_ = State::InProgress;

	if (!isWordWiseSelection())
        to_ = stretchedColumn(coord);
	else if (coord > start_)
	{
		to_ = coord;
		extendSelectionForward();
	}
	else
	{
		to_ = coord;
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
        Cell const* cell = at(_coord);
        return !cell || cell->empty() || wordDelimiters_.find(cell->codepoint(0)) != wordDelimiters_.npos;
    };

    auto last = to_;
    auto current = last;
    for (;;) {
        if (current.column > 1)
            current.column--;
        else if (current.row > 1)
        {
            current.row--;
            current.column = columnCount_;
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
        Cell const* cell = at(_coord);
        return !cell || cell->empty() || wordDelimiters_.find(cell->codepoint(0)) != wordDelimiters_.npos;
    };

    auto last = to_;
    auto current = last;
    for (;;) {
        if (current.column < columnCount_)
        {
            current = stretchedColumn({current.row, current.column + 1});
        }
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

    to_ = stretchedColumn(last);
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
            result[0] = Range{from.row, from.column, columnCount_};
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Range{to.row, 1, to.column};
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Range{from.row, from.column, columnCount_};

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Range{from.row + static_cast<int>(n), 1, columnCount_};

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Range{to.row, 1, to.column};
            break;
    }

    return result;
}

vector<Selector::Range> Selector::lines() const
{
    auto [result, from, to] = prepare(*this);

    for (int row = 0; row < static_cast<int>(result.size()); ++row)
    {
        result[row] = Range{
            from.row + row,
            1,
            columnCount_
        };
    }

    return result;
}

vector<Selector::Range> Selector::rectangular() const
{
    auto [result, from, to] = prepare(*this);

    for (int row = 0; row < static_cast<int>(result.size()); ++row)
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
