// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Alphabet.h>
#include <regex_dfa/DotVisitor.h>
#include <regex_dfa/NFA.h>

#include <fmt/format.h>

#include <algorithm>
#include <iostream>
#include <stack>
#include <vector>

using namespace std;

namespace regex_dfa
{

#if 0
    #define DEBUG(msg, ...)                                \
        do                                                 \
        {                                                  \
            cerr << fmt::format(msg, __VA_ARGS__) << "\n"; \
        } while (0)
#else
    #define DEBUG(msg, ...) \
        do                  \
        {                   \
        } while (0)
#endif

Alphabet NFA::alphabet() const
{
    Alphabet alphabet;

    for (const TransitionMap& transitions: states_)
    {
        for (auto const& t: transitions)
        {
            switch (t.first)
            {
                case Symbols::Epsilon: break;
                default: alphabet.insert(t.first);
            }
        }
    }

    return alphabet;
}

NFA NFA::clone() const
{
    return *this;
}

StateId NFA::createState()
{
    states_.emplace_back();
    return states_.size() - 1;
}

StateIdVec NFA::delta(const StateIdVec& S, Symbol c) const
{
    StateIdVec result;
    delta(S, c, &result);
    return result;
}

StateIdVec* NFA::delta(const StateIdVec& S, Symbol c, StateIdVec* result) const
{
    for (StateId s: S)
    {
        const TransitionMap& transitions = stateTransitions(s);
        for (const auto& transition: transitions)
        {
            if (transition.first == c)
            {
                for (StateId targetState: transition.second)
                {
                    result->push_back(targetState);
                }
            }
        }
    }

    return result;
}

StateIdVec NFA::epsilonTransitions(StateId s) const
{
    StateIdVec t;

    const TransitionMap& transitions = stateTransitions(s);
    for (auto&& [p, q]: transitions)
        if (p == Symbols::Epsilon)
            t.insert(t.end(), q.begin(), q.end());

    return t;
}

StateIdVec NFA::epsilonClosure(const StateIdVec& S) const
{
    StateIdVec eclosure;
    epsilonClosure(S, &eclosure);
    return eclosure;
}

void NFA::epsilonClosure(const StateIdVec& S, StateIdVec* eclosure) const
{
    *eclosure = S;
    vector<bool> availabilityCheck(1 + size(), false);
    stack<StateId> workList;
    for (StateId s: S)
    {
        workList.push(s);
        availabilityCheck[s] = true;
    }

    while (!workList.empty())
    {
        const StateId s = workList.top();
        workList.pop();

        for (StateId t: epsilonTransitions(s))
        {
            if (!availabilityCheck[t])
            {
                eclosure->push_back(t);
                workList.push(t);
            }
        }
    }

    sort(eclosure->begin(), eclosure->end());
}

void NFA::prepareStateIds(StateId baseId)
{
    // adjust transition state IDs
    // traverse through each state's transition set
    //    traverse through each transition in the transition set
    //        traverse through each element and add BASE_ID

    // for each state's transitions
    for (StateId i = 0, e = size(); i != e; ++i)
    {
        TransitionMap& transitions = states_[i];

        // for each vector of target-state-id per transition-symbol
        for (auto t = transitions.begin(), tE = transitions.end(); t != tE; ++t)
        {
            StateIdVec& transition = t->second;

            // for each target state ID
            for (StateId k = 0, kE = transition.size(); k != kE; ++k)
            {
                transition[k] += baseId;
            }
        }
    }

    initialState_ += baseId;
    acceptState_ += baseId;

    AcceptMap remapped;
    for (auto& a: acceptTags_)
        remapped[baseId + a.first] = a.second;
    acceptTags_ = std::move(remapped);

    BacktrackingMap backtracking;
    for (const auto& bt: backtrackStates_)
        backtracking[baseId + bt.first] = baseId + bt.second;
    backtrackStates_ = std::move(backtracking);
}

NFA NFA::join(const map<string, NFA>& mappings)
{
    if (mappings.size() == 1)
        return mappings.begin()->second;

    NFA multi;

    for (size_t i = 0; i <= mappings.size(); ++i)
        (void) multi.createState();

    Symbol transitionSymbol = 0;
    for (const auto& mapping: mappings)
    {
        transitionSymbol++;

        NFA rhs = mapping.second.clone();
        rhs.prepareStateIds(multi.size());

        multi.states_.reserve(multi.size() + rhs.size());
        multi.states_.insert(multi.states_.end(), rhs.states_.begin(), rhs.states_.end());
        multi.acceptTags_.insert(rhs.acceptTags_.begin(), rhs.acceptTags_.end());

        multi.addTransition(multi.initialState_, transitionSymbol, rhs.initialState_);
        multi.backtrackStates_[rhs.acceptState_] = multi.acceptState_;
        multi.acceptState_ = rhs.acceptState_;
    }

    return multi;
}

NFA& NFA::lookahead(NFA&& rhs)
{
    if (empty())
    {
        *this = std::move(rhs);
        backtrackStates_[acceptState_] = initialState_;
    }
    else
    {
        rhs.prepareStateIds(states_.size());
        states_.reserve(size() + rhs.size());
        states_.insert(states_.end(), rhs.states_.begin(), rhs.states_.end());
        acceptTags_.insert(rhs.acceptTags_.begin(), rhs.acceptTags_.end());

        addTransition(acceptState_, Symbols::Epsilon, rhs.initialState_);
        backtrackStates_[rhs.acceptState_] = acceptState_;
        acceptState_ = rhs.acceptState_;
    }

    return *this;
}

NFA& NFA::alternate(NFA&& rhs)
{
    StateId newStart = createState();
    StateId newEnd = createState();

    rhs.prepareStateIds(states_.size());
    states_.insert(states_.end(), rhs.states_.begin(), rhs.states_.end());
    acceptTags_.insert(rhs.acceptTags_.begin(), rhs.acceptTags_.end());
    backtrackStates_.insert(rhs.backtrackStates_.begin(), rhs.backtrackStates_.end());

    addTransition(newStart, Symbols::Epsilon, initialState_);
    addTransition(newStart, Symbols::Epsilon, rhs.initialState_);

    addTransition(acceptState_, Symbols::Epsilon, newEnd);
    addTransition(rhs.acceptState_, Symbols::Epsilon, newEnd);

    initialState_ = newStart;
    acceptState_ = newEnd;

    return *this;
}

NFA& NFA::concatenate(NFA&& rhs)
{
    rhs.prepareStateIds(states_.size());
    states_.reserve(size() + rhs.size());
    states_.insert(states_.end(), rhs.states_.begin(), rhs.states_.end());
    acceptTags_.insert(rhs.acceptTags_.begin(), rhs.acceptTags_.end());
    backtrackStates_.insert(rhs.backtrackStates_.begin(), rhs.backtrackStates_.end());

    addTransition(acceptState_, Symbols::Epsilon, rhs.initialState_);
    acceptState_ = rhs.acceptState_;

    return *this;
}

NFA& NFA::optional()
{
    StateId newStart = createState();
    StateId newEnd = createState();

    addTransition(newStart, Symbols::Epsilon, initialState_);
    addTransition(newStart, Symbols::Epsilon, newEnd);
    addTransition(acceptState_, Symbols::Epsilon, newEnd);

    initialState_ = newStart;
    acceptState_ = newEnd;

    return *this;
}

NFA& NFA::recurring()
{
    // {0, inf}
    StateId newStart = createState();
    StateId newEnd = createState();

    addTransition(newStart, Symbols::Epsilon, initialState_);
    addTransition(newStart, Symbols::Epsilon, newEnd);

    addTransition(acceptState_, Symbols::Epsilon, initialState_);
    addTransition(acceptState_, Symbols::Epsilon, newEnd);

    initialState_ = newStart;
    acceptState_ = newEnd;

    return *this;
}

NFA& NFA::positive()
{
    return concatenate(std::move(clone().recurring()));
}

NFA& NFA::times(unsigned factor)
{
    assert(factor != 0);

    if (factor == 1)
        return *this;

    NFA base = clone();
    for (unsigned n = 2; n <= factor; ++n)
        concatenate(base.clone());

    return *this;
}

NFA& NFA::repeat(unsigned minimum, unsigned maximum)
{
    assert(minimum <= maximum);

    NFA factor = clone();

    if (minimum != 0)
        times(minimum);

    for (unsigned n = minimum + 1; n <= maximum; n++)
        alternate(std::move(factor.clone().times(n)));

    if (minimum == 0)
        optional();

    return *this;
}

void NFA::visit(DotVisitor& v) const
{
    v.start(initialState_);

    // initial state
    v.visitNode(initialState_, true, acceptTags_.find(initialState_) != acceptTags_.end());

    // accepting states
    for (pair<StateId, Tag> acceptTag: acceptTags_)
        if (acceptTag.first != initialState_)
            v.visitNode(acceptTag.first, false, true);

    // other states
    for (StateId i = 0, e = size(); i != e; ++i)
        if (i != initialState_ && acceptTags_.find(i) == acceptTags_.end())
            v.visitNode(i, false, false);

    // transitions
    for (StateId sourceState = 0, sE = size(); sourceState != sE; ++sourceState)
    {
        map<StateId, vector<Symbol>> reversed;
        for (pair<Symbol, StateIdVec> transitions: states_[sourceState])
            for (StateId targetState: transitions.second)
                reversed[targetState].push_back(transitions.first /* symbol */);

        for (pair<StateId, vector<Symbol>> tr: reversed)
        {
            StateId targetState = tr.first;
            const vector<Symbol>& T = tr.second;
            for_each(T.begin(), T.end(), [&](const Symbol t) { v.visitEdge(sourceState, targetState, t); });
            v.endVisitEdge(sourceState, targetState);
        }
    }
    v.end();
}

} // namespace regex_dfa
