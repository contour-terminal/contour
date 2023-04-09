// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/DFA.h>
#include <regex_dfa/DFABuilder.h>
#include <regex_dfa/NFA.h>
#include <regex_dfa/State.h>

#include <algorithm>
#include <deque>
#include <iostream>
#include <sstream>
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

struct DFABuilder::TransitionTable
{ // {{{
    void insert(StateId q, Symbol c, StateId t);
    unordered_map<StateId, unordered_map<Symbol, StateId>> transitions;
};

inline void DFABuilder::TransitionTable::insert(StateId q, Symbol c, StateId t)
{
    transitions[q][c] = t;
}
// }}}

/* DFA construction visualization
  REGEX:      a(b|c)*

  NFA:        n0 --(a)--> n1 --> n2 -----------------------------------> "n7"
                                  \                                       ^
                                   \---> n3 <------------------------    /
                                         \ \                         \  /
                                          \ \----> n4 --(b)--> n5 --> n6
                                           \                          ^
                                            \----> n8 --(c)--> n9 ---/

  DFA:
                                            <---
              d0 --(a)--> "d1" ----(b)--> "d2"--(b)
                             \             |^
                              \         (c)||(b)
                               \           v|
                                \--(c)--> "d3"--(c)
                                            <---


  TABLE:

    set   | DFA   | NFA                 |
    name  | state | state               | 'a'                 | 'b'                 | 'c'
    --------------------------------------------------------------------------------------------------------
    q0    | d0    | {n0}                | {n1,n2,n3,n4,n7,n8} | -none-              | -none-
    q1    | d1    | {n1,n2,n3,n4,n7,n8} | -none-              | {n3,n4,n5,n6,n7,n8} | {n3,n4,n6,n7,n8,n9}
    q2    | d2    | {n3,n4,n5,n6,n7,n8} | -none-              | q2                  | q3
    q3    | d3    | {n3,n4,n6,n7,n8,n9} | -none-              | q2                  | q3
*/

DFA DFABuilder::construct(OvershadowMap* overshadows)
{
    const StateIdVec q_0 = nfa_.epsilonClosure({ nfa_.initialStateId() });
    vector<StateIdVec> Q = { q_0 }; // resulting states
    deque<StateIdVec> workList = { q_0 };
    TransitionTable T;

    const Alphabet alphabet = nfa_.alphabet();

    StateIdVec eclosure;
    StateIdVec delta;
    while (!workList.empty())
    {
        const StateIdVec q =
            move(workList.front()); // each set q represents a valid configuration from the NFA
        workList.pop_front();
        const StateId q_i = *configurationNumber(Q, q);

        for (Symbol c: alphabet)
        {
            nfa_.epsilonClosure(*nfa_.delta(q, c, &delta), &eclosure);
            if (!eclosure.empty())
            {
                if (optional<StateId> t_i = configurationNumber(Q, eclosure); t_i.has_value())
                    T.insert(q_i, c, *t_i); // T[q][c] = eclosure;
                else
                {
                    Q.emplace_back(eclosure);
                    t_i = StateId { Q.size() - 1 }; // equal to configurationNumber(Q, eclosure);
                    T.insert(q_i, c, *t_i);         // T[q][c] = eclosure;
                    workList.emplace_back(move(eclosure));
                }
                eclosure.clear();
            }
            delta.clear();
        }
    }

    // Q now contains all the valid configurations and T all transitions between them
    return constructDFA(Q, T, overshadows);
}

DFA DFABuilder::constructDFA(const vector<StateIdVec>& Q,
                             const TransitionTable& T,
                             OvershadowMap* overshadows) const
{
    DFA dfa;
    dfa.createStates(Q.size());

    // build remaps table (used as cache for quickly finding DFA StateIds from NFA StateIds)
    unordered_map<StateId, StateId> remaps;
    for_each(begin(Q), end(Q), [q_i = StateId { 0 }, &remaps](StateIdVec const& q) mutable {
        for_each(begin(q), end(q), [&](StateId s) { remaps[s] = q_i; });
        q_i++;
    });

    // map q_i to d_i and flag accepting states
    map<Tag, Tag> overshadowing;
    StateId q_i = 0;
    for (const StateIdVec& q: Q)
    {
        // d_i represents the corresponding state in the DFA for all states of q from the NFA
        const StateId d_i = q_i;
        // cerr << fmt::format("map q{} to d{} for {} states, {}.\n", q_i, d_i->id(), q.size(),
        // to_string(q, "d"));

        // if q contains an accepting state, then d is an accepting state in the DFA
        if (nfa_.isAnyAccepting(q))
        {
            optional<Tag> tag = determineTag(q, &overshadowing);
            assert(tag.has_value() && "DFA accept state merged from input states with different tags.");
            // DEBUG("determineTag: q{} tag {} from {}.", q_i, *tag, q);
            dfa.setAccept(d_i, *tag);
        }

        if (optional<StateId> bt = nfa_.containsBacktrackState(q); bt.has_value())
        {
            // TODO: verify: must not contain more than one backtracking mapping
            assert(dfa.isAccepting(d_i));
            dfa.setBacktrack(d_i, remaps[*bt]);
        }

        q_i++;
    }

    // observe mapping from q_i to d_i
    for (auto const& [q_i, branch]: T.transitions)
        for (auto const [c, t_i]: branch)
            dfa.setTransition(q_i, c, t_i);

    // q_0 becomes d_0 (initial state)
    dfa.setInitialState(0);

    if (overshadows)
    {
        // check if tag is an acceptor in NFA but not in DFA, hence, it was overshadowed by another rule
        for (const pair<StateId, Tag> a: nfa_.acceptMap())
        {
            const Tag tag = a.second;
            if (!dfa.isAcceptor(tag))
                if (auto i = overshadowing.find(tag); i != overshadowing.end())
                    overshadows->emplace_back(tag, i->second);
        }
    }

    return dfa;
}

optional<StateId> DFABuilder::configurationNumber(const vector<StateIdVec>& Q, const StateIdVec& t)
{
    if (auto i = find(begin(Q), end(Q), t); i != end(Q))
        return distance(begin(Q), i);
    else
        return nullopt;
}

optional<Tag> DFABuilder::determineTag(const StateIdVec& qn, map<Tag, Tag>* overshadows) const
{
    deque<Tag> tags;

    for (StateId s: qn)
        if (optional<Tag> t = nfa_.acceptTag(s); t.has_value())
            tags.push_back(*t);

    if (tags.empty())
        return nullopt;

    sort(begin(tags), end(tags));

    optional<Tag> lowestTag = tags.front();
    tags.erase(begin(tags));

    for (Tag tag: tags)
        (*overshadows)[tag] = *lowestTag; // {tag} is overshadowed by {lowestTag}

    return lowestTag;
}

} // namespace regex_dfa
