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
#include <terminal_view/Selector.h>
#include <terminal/Terminal.h>

using namespace std;
using namespace terminal;

Selector::Selector(WindowSize const& _viewport, Coordinate const& _from) :
    viewport_{_viewport},
    from_{_from},
    to_{_from}
{
}

void Selector::restart(Coordinate const& _from)
{
    state_ = State::Waiting;
    from_ = to_ = _from;
}

bool Selector::extend(Coordinate const& _to)
{
    assert(state_ != State::Complete && "In order extend a selection, the selector must be active (started).");
    state_ = State::InProgress;
    to_ = _to;

    // TODO: indicates whether or not a scroll action must take place.
    return false;
}

void Selector::stop()
{
    if (state_ == State::InProgress)
        state_ = State::Complete;
}

void copy(vector<Selector::Range> const& _ranges,
          Terminal const& _source,
          Selector::Renderer _render)
{
    for (auto const& range : _ranges)
        for (auto col = range.fromColumn; col <= range.toColumn; ++col)
            _render(range.line, col, _source.absoluteAt(range.line, col));
}

// ==========================================================================

tuple<vector<Selector::Range>, terminal::Coordinate const, terminal::Coordinate const>
prepare(Selector const& _selector)
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

vector<Selector::Range> linear(Selector const& _selector)
{
    auto [result, from, to] = prepare(_selector);

    switch (result.size())
    {
        case 1:
            result[0] = Selector::Range{from.row, from.column, to.column};
            break;
        case 2:
            // Render first line partial from selected column to end.
            result[0] = Selector::Range{from.row, from.column, _selector.viewport().columns};
            // Render last (second) line partial from beginning to last selected column.
            result[1] = Selector::Range{to.row, 1, to.column};
            break;
        default:
            // Render first line partial from selected column to end.
            result[0] = Selector::Range{from.row, from.column, _selector.viewport().columns};

            // Render inner full.
            for (size_t n = 1; n < result.size(); ++n)
                result[n] = Selector::Range{from.row + static_cast<cursor_pos_t>(n), 1, _selector.viewport().columns};

            // Render last (second) line partial from beginning to last selected column.
            result[result.size() - 1] = Selector::Range{to.row, 1, to.column};
            break;
    }

    return result;
}

vector<Selector::Range> lines(Selector const& _selector)
{
    auto [result, from, to] = prepare(_selector);

    for (cursor_pos_t row = 0; row < result.size(); ++row)
    {
        result[row] = Selector::Range{
            from.row + row,
            1,
            _selector.viewport().columns
        };
    }

    return result;
}

vector<Selector::Range> rectangular(Selector const& _selector)
{
    auto [result, from, to] = prepare(_selector);

    for (cursor_pos_t row = 0; row < result.size(); ++row)
    {
        result[row] = Selector::Range{
            from.row + row,
            from.column,
            to.column
        };
    }

    return result;
}
