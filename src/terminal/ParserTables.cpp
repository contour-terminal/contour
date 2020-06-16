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
#include <terminal/ParserTables.h>
#include <crispy/indexed.h>

#include <map>
#include <ostream>

using namespace std;

namespace terminal {

using State = Parser::State;
using Transition = pair<State, State>;
using Range = ParserTable::Range;
using RangeSet = std::vector<Range>;

void dot(std::ostream& _os, ParserTable const& _table)
{
    // (State, Byte) -> State
    auto transitions = map<Transition, RangeSet>{};
    for ([[maybe_unused]] auto const && [sourceState, sourceTransitions] : crispy::indexed(_table.transitions))
    {
        for (auto const [i, targetState] : crispy::indexed(sourceTransitions))
        {
            auto const ch = static_cast<uint8_t>(i);
            if (targetState != State::Undefined)
            {
                //_os << fmt::format("({}, 0x{:0X}) -> {}\n", static_cast<State>(sourceState), ch, targetState);
                auto const t = Transition{static_cast<State>(sourceState), targetState};
                if (!transitions[t].empty() && ch == transitions[t].back().last + 1)
                    transitions[t].back().last++;
                else
                    transitions[t].emplace_back(Range{ch, ch});
            }
        }
    }

    _os << "digraph {\n";
    _os << "  node [shape=box];\n";
    _os << "  ranksep = 0.75;\n";
    _os << "  rankdir = LR;\n";
    _os << "  concentrate = true;\n";
    for (auto const& t : transitions)
    {
        if (t.first.first == State::Undefined)
            continue;

        _os << fmt::format("  {} -> {} ", t.first.first, t.first.second);
        _os << "[";
        _os << "label=\"";
        for (auto const && [rangeCount, u] : crispy::indexed(t.second))
        {
            if (rangeCount)
            {
                _os << ", ";
                if (rangeCount % 3 == 0)
                    _os << "\\n";
            }
            if (u.first == u.last)
                _os << fmt::format("{:02X}", u.first);
            else
                _os << fmt::format("{:02X}-{:02X}", u.first, u.last);
        }
        _os << "\"";
        _os << "]";
        _os << ";\n";
    }

    // equal ranks
    _os << "  { rank=same; ";
    for (auto const state : {State::CSI_Entry, State::DCS_Entry, State::OSC_String})
        _os << fmt::format("{}; ", state);
    _os << "};\n";

    _os << "  { rank=same; ";
    for (auto const state : {State::CSI_Param, State::DCS_Param, State::OSC_String})
        _os << fmt::format("{}; ", state);
    _os << "};\n";

    _os << "}\n";
}

}

