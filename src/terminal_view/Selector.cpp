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
    active_ = true;
    from_ = to_ = _from;
}

bool Selector::extend(Coordinate const& _to)
{
    assert(active_ && "In order extend a selection, the selector must be active (started).");
    to_ = _to;

    return false; // TODO: indicates whether or not a scroll action must take place.
}

void Selector::stop()
{
    active_ = false;
}

void Selector::copy(Terminal const& _source, Renderer _render) const
{
    for (Range const& range : ranges())
        for (cursor_pos_t col = range.fromColumn; col <= range.toColumn; ++col)
            _render(range.line, col, _source.absoluteAt(range.line, col));
}

// ==========================================================================

vector<Selector::Range> LinearSelector::ranges() const
{
    vector<Range> result;

    auto const [from, to] = [this]() {
        if (to_ < from_)
            return pair{to_, from_};
        else
            return pair{from_, to_};
    }();

    auto const numLines = to.row - from.row + 1;
    result.reserve(numLines);

    switch (numLines)
    {
        case 1:
            result.emplace_back(Range{from.row, from.column, to.column});
            break;
        case 2:
            // Render first line partial from selected column to end.
            result.emplace_back(Range{from.row, from.column, viewport_.columns});
            // Render last (second) line partial from beginning to last selected column.
            result.emplace_back(Range{to.row, 1, to.column});
            break;
        default:
            // Render first line partial from selected column to end.
            result.emplace_back(Range{from.row, from.column, viewport_.columns});

            // Render inner full.
            for (cursor_pos_t lineOffset = from.row + 1; lineOffset < to.row; ++lineOffset)
                result.emplace_back(Range{lineOffset, 1, viewport_.columns});

            // Render last (second) line partial from beginning to last selected column.
            result.emplace_back(Range{to.row, 1, to.column});
            break;
    }

    return result;
}
