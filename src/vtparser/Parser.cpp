// SPDX-License-Identifier: Apache-2.0
#include <vtparser/Parser.h>
#include <vtparser/ParserEvents.h>

#include <range/v3/view/enumerate.hpp>

#include <format>
#include <map>
#include <ostream>

namespace vtparser
{

using ranges::views::enumerate;

void parserTableDot(std::ostream& os) // {{{
{
    using Transition = std::pair<State, State>;
    using Range = ParserTable::Range;
    using RangeSet = std::vector<Range>;

    ParserTable const& table = ParserTable::get();
    // (State, Byte) -> State
    auto transitions = std::map<Transition, RangeSet> {};
    for ([[maybe_unused]] auto const&& [sourceState, sourceTransitions]: enumerate(table.transitions))
    {
        for (auto const [i, targetState]: enumerate(sourceTransitions))
        {
            auto const ch = static_cast<uint8_t>(i);
            if (targetState != State::Undefined)
            {
                // os << std::format("({}, 0x{:0X}) -> {}\n", static_cast<State>(sourceState), ch,
                //  targetState);
                auto const t = Transition { static_cast<State>(sourceState), targetState };
                if (!transitions[t].empty() && ch == transitions[t].back().last + 1)
                    transitions[t].back().last++;
                else
                    transitions[t].emplace_back(Range { ch, ch });
            }
        }
    }
    // TODO: isReachableFromAnywhere(targetState) to check if x can be reached from anywhere.

    os << "digraph {\n";
    os << "  node [shape=box];\n";
    os << "  ranksep = 0.75;\n";
    os << "  rankdir = LR;\n";
    os << "  concentrate = true;\n";

    unsigned groundCount = 0;

    for (auto const& t: transitions)
    {
        auto const sourceState = t.first.first;
        auto const targetState = t.first.second;

        if (sourceState == State::Undefined)
            continue;

        auto const targetStateName = targetState == State::Ground && targetState != sourceState
                                         ? std::format("{}_{}", targetState, ++groundCount)
                                         : std::format("{}", targetState);

        // if (isReachableFromAnywhere(targetState))
        //     os << std::format("  {} [style=dashed, style=\"rounded, filled\", fillcolor=yellow];\n",
        //     sourceStateName);

        if (targetState == State::Ground && sourceState != State::Ground)
            os << std::format("  \"{}\" [style=\"dashed, filled\", fillcolor=gray, label=\"ground\"];\n",
                              targetStateName);

        os << std::format(R"(  "{}" -> "{}" )", sourceState, targetStateName);
        os << "[";
        os << "label=\"";
        for (auto const&& [rangeCount, u]: enumerate(t.second))
        {
            if (rangeCount)
            {
                os << ", ";
                if (rangeCount % 3 == 0)
                    os << "\\n";
            }
            if (u.first == u.last)
                os << std::format("{:02X}", u.first);
            else
                os << std::format("{:02X}-{:02X}", u.first, u.last);
        }
        os << "\"";
        os << "]";
        os << ";\n";
    }

    // equal ranks
    os << "  { rank=same; ";
    for (auto const state: { State::CSI_Entry, State::DCS_Entry, State::OSC_String })
        os << std::format(R"("{}"; )", state);
    os << "};\n";

    os << "  { rank=same; ";
    for (auto const state: { State::CSI_Param, State::DCS_Param, State::OSC_String })
        os << std::format(R"("{}"; )", state);
    os << "};\n";

    os << "}\n";
}
// }}}

} // namespace vtparser

template class vtparser::Parser<vtparser::ParserEvents>;
