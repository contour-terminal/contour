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
#include <terminal_view/TextScheduler.h>
#include <crispy/times.h>

namespace terminal::view {

using crispy::times;
using unicode::out;

TextScheduler::TextScheduler(Flusher _flusher)
    : flusher_{ move(_flusher) }
{
}

void TextScheduler::reset()
{
    state_ = State::Empty;

    row_ = 1;
    startColumn_ = 1;
    attributes_ = {};
    codepoints_.clear();
    clusters_.clear();
}

void TextScheduler::reset(cursor_pos_t _row, cursor_pos_t _col, ScreenBuffer::GraphicsAttributes const& _attr)
{
    state_ = State::Filling;
    row_ = _row;
    startColumn_ = _col;
    attributes_ = _attr;
    codepoints_.clear();
    clusters_.clear();
}

void TextScheduler::extend(ScreenBuffer::Cell const& _cell, cursor_pos_t _column)
{
    for (size_t const i: times(_cell.codepointCount()))
    {
        codepoints_.emplace_back(_cell.codepoint(i));
        clusters_.emplace_back(_column);
    }
}

void TextScheduler::schedule(cursor_pos_t _row, cursor_pos_t _col, Screen::Cell const& _cell)
{
    // TODO: new scheduling algo with given procedure
    //
    // 1) fill up one line (& split words by spaces, right here?)
    //      case (State.Space, Char.Space) -> skip
    //      case (State.Space, Char.NoSpace) -> state <- Fill
    // 2) segment line into runs
    // 3) render each run
    // 4) start next line

    constexpr char32_t SP = 0x20;

    switch (state_)
    {
        case State::Empty:
            if (_cell.codepoint() != SP)
            {
                reset(_row, _col, _cell.attributes());
                state_ = State::Filling;
                extend(_cell, _col);
            }
            break;
        case State::Filling:
            if (row_ == _row && attributes_ == _cell.attributes() && _cell.codepoint() != SP)
                extend(_cell, _col);
            else
            {
                flush();
                if (_cell.codepoint() == SP)
                {
                    state_ = State::Empty;
                    reset();
                }
                else // i.o.w.: cell attributes OR row number changed
                {
                    reset(_row, _col, _cell.attributes());
                    extend(_cell, _col);
                }
            }
            break;
    }
}

void TextScheduler::flush()
{
    if (codepoints_.size() == 0)
        return;

    auto rs = unicode::run_segmenter(codepoints_.data(), codepoints_.size());
    while (rs.consume(out(run_)))
        flusher_(*this);
}

} // end namespace
