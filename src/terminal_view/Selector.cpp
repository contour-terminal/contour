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

Selector::Selector(Coordinate const& _from) :
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
    cerr << fmt::format("Selector range: from {} to {}.", from_, to_) << endl;
}

// ==========================================================================

void LinearSelector::copy(Terminal const& _source, size_t _scrollOffset, Renderer _render) const
{
    auto const [from, to] = [this]() {
        if (to_ < from_)
            return pair{to_, from_};
        else
            return pair{from_, to_};
    }();

    auto const numLines = to.row - from.row + 1;

    switch (numLines)
    {
        case 1:
            for (cursor_pos_t colOffset = 0; colOffset < to.column - from.column; ++colOffset)
                _render(1, colOffset + 1, _source.absoluteAt(from.row, from.column + colOffset));
            break;
        case 2:
            // Render first line partial from selected column to end.
            for (cursor_pos_t colOffset = 0; colOffset < _source.size().columns - from.column; ++colOffset)
                _render(1, colOffset + 1, _source.absoluteAt(from.row, from.column + colOffset));

            // Render last (second) line partial from beginning to last selected column.
            for (cursor_pos_t colOffset = 0; colOffset < to.column; ++colOffset)
                _render(2, colOffset + 1, _source.absoluteAt(to.row, colOffset + 1));
            break;
        default:
            // Render first line partial from selected column to end.
            for (cursor_pos_t colOffset = 0; colOffset < _source.size().columns - from.column; ++colOffset)
                _render(1, colOffset + 1, _source.absoluteAt(from.row, from.column + colOffset));

            // Render inner full.
            for (cursor_pos_t lineOffset = 1; lineOffset < numLines - 1; ++lineOffset)
                for (cursor_pos_t colOffset = 0; colOffset < _source.size().columns; ++colOffset)
                    _render(
                        lineOffset + 1,
                        colOffset + 1,
                        _source.absoluteAt(from.row + lineOffset, colOffset + 1)
                    );

            // Render last (second) line partial from beginning to last selected column.
            for (cursor_pos_t colOffset = 0; colOffset < to.column; ++colOffset)
                _render(numLines, colOffset + 1, _source.absoluteAt(to.row, colOffset + 1));
    }
}

// ==========================================================================

