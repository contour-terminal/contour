// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/DFA.h>
#include <regex_dfa/DFAMinimizer.h>
#include <regex_dfa/State.h>

#include <algorithm>
#include <cassert>
#include <functional>
#include <map>
#include <sstream>
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

DFAMinimizer::DFAMinimizer(const DFA& dfa):
    dfa_ { dfa },
    initialStates_ { { "INITIAL", dfa.initialState() } },
    alphabet_ { dfa_.alphabet() },
    targetStateIdMap_ {}
{
}

DFAMinimizer::DFAMinimizer(const MultiDFA& multiDFA):
    dfa_ { multiDFA.dfa },
    initialStates_ { multiDFA.initialStates },
    alphabet_ { dfa_.alphabet() },
    targetStateIdMap_ {}
{
}

/**
 * Tests whether or not StateId @p s is an initial state in any of the DFAs of the MultiDFA.
 */
bool DFAMinimizer::isMultiInitialState(StateId s) const
{
    return any_of(initialStates_.begin(), initialStates_.end(), [s](const auto& p) { return p.second == s; });
}

/**
 * Tests whether any s in S is the initial state in the DFA that is to be minimized.
 */
bool DFAMinimizer::containsInitialState(const StateIdVec& S) const
{
    return any_of(S.begin(), S.end(), [this](StateId s) { return s == dfa_.initialState(); });
}

DFAMinimizer::PartitionVec::iterator DFAMinimizer::findGroup(StateId s)
{
    return find_if(begin(T), end(T), [&](StateIdVec& group) {
        return dfa_.acceptTag(group.front()) == dfa_.acceptTag(s);
    });
}

int DFAMinimizer::partitionId(StateId s) const
{
    auto i =
        find_if(P.begin(), P.end(), [s](const auto& p) { return find(p.begin(), p.end(), s) != p.end(); });
    assert(i != P.end() && "State ID must be present in any of the partition sets.");
    return static_cast<int>(distance(P.begin(), i));
}

DFAMinimizer::PartitionVec DFAMinimizer::split(const StateIdVec& S) const
{
    for (Symbol c: alphabet_)
    {
        // if c splits S into s_1 and s_2
        //      that is, phi(s_1, c) and phi(s_2, c) reside in two different p_i's (partitions)
        // then return {s_1, s_2}

        map<int /*target partition set*/, StateIdVec /*source states*/> t_i;
        for (StateId s: S)
        {
            if (const optional<StateId> t = dfa_.delta(s, c); t.has_value())
                t_i[partitionId(*t)].push_back(s);
            else
                t_i[-1].push_back(s);
        }
        if (t_i.size() > 1)
        {
            DEBUG("split: {} on character '{}' into {} sets", to_string(S), (char) c, t_i.size());
            PartitionVec result;
            for (const pair<int, StateIdVec>& t: t_i)
            {
                result.emplace_back(move(t.second));
                DEBUG(" partition {}: {}", t.first, t.second);
            }
            return result;
        }

        assert(t_i.size() == 1);

        // t_i's only element thus is a reconstruction of S.
        assert(t_i.begin()->second == S);

        for (StateId s: S)
        {
            PartitionVec result;
            StateIdVec main;

            if (isMultiInitialState(s))
                result.emplace_back(StateIdVec { s });
            else
                main.emplace_back(s);

            if (!main.empty())
                result.emplace_back(move(main));
        }
    }

    DEBUG("split: no split needed for {}", to_string(S));
    return { S };
}

void DFAMinimizer::dumpGroups(const PartitionVec& T)
{
    DEBUG("dumping groups ({})", T.size());
    int groupNr = 0;
    for (const auto& t: T)
    {
        stringstream sstr;
        sstr << "{";
        for (size_t i = 0, e = t.size(); i != e; ++i)
        {
            if (i)
                sstr << ", ";
            sstr << "n" << t[i];
        }
        sstr << "}";
        DEBUG("group {}: {}", groupNr, sstr.str());
        groupNr++;
    }
}

DFA DFAMinimizer::constructDFA()
{
    constructPartitions();
    return constructFromPartitions(P);
}

MultiDFA DFAMinimizer::constructMultiDFA()
{
    constructPartitions();
    DFA dfamin = constructFromPartitions(P);

    // patch initialStates and the master-initial-state's transition symbol
    MultiDFA::InitialStateMap initialStates;
    for (const pair<const string, StateId>& p: initialStates_)
        dfamin.removeTransition(dfamin.initialState(), static_cast<Symbol>(p.second));

    for (const pair<const string, StateId>& p: initialStates_)
    {
        const StateId t = targetStateId(p.second);
        initialStates[p.first] = t;
        dfamin.setTransition(dfamin.initialState(), static_cast<Symbol>(t), t);
    }

    return MultiDFA { move(initialStates), move(dfamin) };
}

void DFAMinimizer::constructPartitions()
{
    // group all accept states by their tag
    for (StateId s: dfa_.acceptStates())
    {
        if (auto group = findGroup(s); group != T.end())
            group->push_back(s);
        else
            T.push_back({ s });
    }

    // add another group for all non-accept states
    T.emplace_back(dfa_.nonAcceptStates());

    dumpGroups(T);

    PartitionVec splits;
    while (P != T)
    {
        swap(P, T);
        T.clear();

        for (StateIdVec& p: P)
            T.splice(T.end(), split(p));
    }

    // build up cache to quickly get target state ID from input DFA's state ID
    targetStateIdMap_ = [&]() {
        unordered_map<StateId, StateId> remaps;
        StateId p_i = 0;
        for (const StateIdVec& p: P)
        {
            for (StateId s: p)
                remaps[s] = p_i;

            p_i++;
        }
        return remaps;
    }();
}

DFA DFAMinimizer::constructFromPartitions(const PartitionVec& P) const
{
    DEBUG("minimization terminated with {} unique partition sets", P.size());

    // instanciate states
    DFA dfamin;
    dfamin.createStates(P.size());
    StateId p_i = 0;
    for (const StateIdVec& p: P)
    {
        const StateId s = *p.begin();
        const StateId q = p_i;
        DEBUG("Creating p{}: {} {}",
              p_i,
              dfa_.isAccepting(s) ? "accepting" : "rejecting",
              containsInitialState(p) ? "initial" : "");
        if (optional<Tag> tag = dfa_.acceptTag(s); tag.has_value())
            dfamin.setAccept(q, *tag);

        if (containsInitialState(p))
            dfamin.setInitialState(q);

        if (optional<StateId> bt = containsBacktrackState(p); bt.has_value())
            dfamin.setBacktrack(p_i, targetStateId(*bt));

        p_i++;
    }

    // setup transitions
    p_i = 0;
    for (const StateIdVec& p: P)
    {
        const StateId s = *p.begin();
        for (const pair<Symbol, StateId>& transition: dfa_.stateTransitions(s))
        {
            const int t_i = partitionId(transition.second);
            DEBUG("map p{} --({})--> p{}", p_i, prettySymbol(transition.first), t_i);
            dfamin.setTransition(p_i, transition.first, t_i);
        }
        p_i++;
    }

    return dfamin;
}

optional<StateId> DFAMinimizer::containsBacktrackState(const StateIdVec& Q) const
{
    for (StateId q: Q)
        if (optional<StateId> t = dfa_.backtrack(q); t.has_value())
            return *t;

    return nullopt;
}

} // namespace regex_dfa
