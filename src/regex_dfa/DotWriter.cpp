// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/DotWriter.h>
#include <regex_dfa/Symbols.h>

#include <fmt/format.h>

#include <algorithm>
#include <cassert>
#include <sstream>

using namespace std;

namespace regex_dfa
{

template <typename StringType>
static string escapeString(const StringType& str)
{
    stringstream stream_;
    for (char ch: str)
    {
        // \t\n\r is already converted to escape sequence
        switch (ch)
        {
            case '\\': stream_ << "\\\\"; break;
            case '"': stream_ << "\\\""; break;
            default: stream_ << ch; break;
        }
    }
    return stream_.str();
}

void DotWriter::start(StateId initialState)
{
    initialState_ = initialState;
    stream_ << "digraph {\n";
    stream_ << "  rankdir=LR;\n";
    // stream_ << "  label=\"" << escapeString("FA" /*TODO*/) << "\";\n";
}

void DotWriter::visitNode(StateId number, bool start, bool accept)
{
    if (start)
    {
        const string_view shape = accept ? "doublecircle" : "circle";
        stream_ << "  \"\" [shape=plaintext];\n";
        stream_ << "  node [shape=" << shape << ",color=red];\n";
        stream_ << "  \"\" -> " << stateLabelPrefix_ << number << ";\n";
        stream_ << "  node [color=black];\n";
    }
    else if (accept)
    {
        stream_ << "  node [shape=doublecircle]; " << stateLabelPrefix_ << number << ";\n";
        stream_ << "  node [shape=circle,color=black];\n";
    }
    else
    {
        // stream_ << stateLabelPrefix_ << number << ";\n";
    }
}

void DotWriter::visitEdge(StateId /*from*/, StateId to, Symbol s)
{
    transitionGroups_[to].push_back(s);
}

void DotWriter::endVisitEdge(StateId from, StateId to)
{
    auto& tgroup = transitionGroups_[to];
    if (!tgroup.empty())
    {
        if (from == initialState_ && initialStates_ != nullptr)
        {
            for (Symbol s: tgroup)
            {
                const string label = [this, s]() {
                    for (const auto& p: *initialStates_)
                        if (p.second == static_cast<StateId>(s))
                            return fmt::format("<{}>", p.first);
                    return prettySymbol(s);
                }();
                stream_ << fmt::format("  {}{} -> {}{} [label=\"{}\"];\n",
                                       stateLabelPrefix_,
                                       from,
                                       stateLabelPrefix_,
                                       to,
                                       escapeString(label));
            }
        }
        else
        {
            string label = groupCharacterClassRanges(std::move(tgroup));
            stream_ << fmt::format("  {}{} -> {}{} [label=\"{}\"];\n",
                                   stateLabelPrefix_,
                                   from,
                                   stateLabelPrefix_,
                                   to,
                                   escapeString(label));
        }
        tgroup.clear();
    }
}

void DotWriter::end()
{
    stream_ << "}\n";
}

} // namespace regex_dfa
