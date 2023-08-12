// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/State.h>
#include <regex_dfa/TransitionMap.h>

#include <map>
#include <sstream>
#include <string>

namespace regex_dfa
{

// special tags
constexpr Tag IgnoreTag = static_cast<Tag>(-1);
constexpr Tag FirstUserTag = 1;

using AcceptStateMap = std::map<StateId, Tag>;

//! defines a mapping between accept state ID and another (prior) ID to track roll back the input stream to.
using BacktrackingMap = std::map<StateId, StateId>;

struct LexerDef
{
    std::map<std::string, StateId> initialStates;
    bool containsBeginOfLineStates;
    TransitionMap transitions;
    AcceptStateMap acceptStates;
    BacktrackingMap backtrackingStates;
    std::map<Tag, std::string> tagNames;

    [[nodiscard]] std::string to_string() const;

    [[nodiscard]] bool isValidTag(Tag t) const noexcept { return tagNames.find(t) != tagNames.end(); }

    [[nodiscard]] std::string tagName(Tag t) const
    {
        auto i = tagNames.find(t);
        assert(i != tagNames.end());
        return i->second;
    }
};

inline std::string LexerDef::to_string() const
{
    std::stringstream sstr;

    sstr << fmt::format("initializerStates:\n");
    for (const std::pair<std::string, StateId> q0: initialStates)
        sstr << fmt::format("  {}: {}\n", q0.first, q0.second);
    sstr << fmt::format("totalStates: {}\n", transitions.states().size());

    sstr << "transitions:\n";
    for (StateId inputState: transitions.states())
    {
        std::map<StateId, std::vector<Symbol>> T;
        for (const std::pair<Symbol, StateId> p: transitions.map(inputState))
        {
            T[p.second].push_back(p.first);
        }
        for (auto& t: T)
        {
            sstr << fmt::format(
                "- n{} --({})--> n{}\n", inputState, groupCharacterClassRanges(std::move(t.second)), t.first);
        }
    }

    sstr << "accepts:\n";
    for (const std::pair<StateId, Tag> a: acceptStates)
        sstr << fmt::format("- n{} to {} ({})\n", a.first, a.second, tagName(a.second));

    if (!backtrackingStates.empty())
    {
        sstr << "backtracking:\n";
        for (const std::pair<StateId, StateId> bt: backtrackingStates)
            sstr << fmt::format("- n{} to n{}\n", bt.first, bt.second);
    }

    return sstr.str();
}

} // namespace regex_dfa
