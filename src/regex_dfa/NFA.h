// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/State.h>
#include <regex_dfa/util/UnboxedRange.h>

#include <map>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace regex_dfa
{

class Alphabet;
class DotVisitor;
class DFA;

/**
 * NFA Builder with the Thompson's Construction properties.
 *
 * <ul>
 *   <li> There is exactly one initial state and exactly one accepting state..
 *   <li> No transition other than the initial transition enters the initial state.
 *   <li> The accepting state has no leaving edges
 *   <li> An ε-transition always connects two states that were (earlier in the construction process)
 *        the initial state and the accepting state of NFAs for some component REs.
 *   <li> Each state has at most two entering states and at most two leaving states.
 * </ul>
 */
class NFA
{
  private:
    NFA(const NFA& other) = default;
    NFA& operator=(const NFA& other) = default;

  public:
    //! represent a transition table for a specific state
    using TransitionMap = std::map<Symbol, StateIdVec>;

    //! defines a set of states within one NFA. the index represents the state Id.
    using StateVec = std::vector<TransitionMap>;

    //! defines a mapping between accept state ID and another (prior) ID to track roll back the input stream
    //! to.
    using BacktrackingMap = std::map<StateId, StateId>;

    NFA(NFA&&) = default;
    NFA& operator=(NFA&&) = default;

    //! Constructs an empty NFA.
    NFA(): states_ {}, initialState_ { 0 }, acceptState_ { 0 }, backtrackStates_ {}, acceptTags_ {} {}

    /**
     * Constructs an NFA for a single character transition.
     *
     * *No* acceptState flag is set on the accepting node!
     */
    explicit NFA(Symbol value): NFA {}
    {
        initialState_ = createState();
        acceptState_ = createState();
        addTransition(initialState_, value, acceptState_);
    }

    explicit NFA(SymbolSet value): NFA {}
    {
        initialState_ = createState();
        acceptState_ = createState();
        for (Symbol s: value)
            addTransition(initialState_, s, acceptState_);
    }

    void addTransition(StateId from, Symbol s, StateId to) { states_[from][s].push_back(to); }

    static NFA join(const std::map<std::string, NFA>& mappings);

    /**
     * Traverses all states and edges in this NFA and calls @p visitor for each state & edge.
     *
     * Use this function to e.g. get a GraphViz dot-file drawn.
     */
    void visit(DotVisitor& visitor) const;

    //! Tests whether or not this is an empty NFA.
    bool empty() const noexcept { return states_.empty(); }

    //! Retrieves the number of states of this NFA.
    size_t size() const noexcept { return states_.size(); }

    //! Retrieves the one and only initial state. This value is nullptr iff the NFA is empty.
    StateId initialStateId() const noexcept { return initialState_; }

    //! Retrieves the one and only accept state. This value is nullptr iff the NFA is empty.
    StateId acceptStateId() const noexcept { return acceptState_; }

    //! Retrieves the list of states this FA contains.
    const StateVec& states() const { return states_; }
    StateVec& states() { return states_; }

    //! Retrieves the alphabet of this finite automaton.
    Alphabet alphabet() const;

    //! Clones this NFA.
    NFA clone() const;

    /**
     * Constructs an NFA where @p rhs is following but backtracking to @c acceptState(this) when
     * when @p rhs is fully matched.
     *
     * This resembles the syntax r/s (or r(?=s) in Perl) where r is matched when also s is following.
     */
    NFA& lookahead(NFA&& rhs);

    //! Reconstructs this FA to alternate between this FA and the @p other FA.
    NFA& alternate(NFA&& other);

    //! Concatenates the right FA's initial state with this FA's accepting state.
    NFA& concatenate(NFA&& rhs);

    //! Reconstructs this FA to allow optional input. X -> X?
    NFA& optional();

    //! Reconstructs this FA with the given @p quantifier factor.
    NFA& times(unsigned quantifier);

    //! Reconstructs this FA to allow recurring input. X -> X*
    NFA& recurring();

    //! Reconstructs this FA to be recurring at least once. X+ = XX*
    NFA& positive();

    //! Reconstructs this FA to be repeatable between range [minimum, maximum].
    NFA& repeat(unsigned minimum, unsigned maximum);

    //! Retrieves transitions for state with the ID @p id.
    const TransitionMap& stateTransitions(StateId id) const { return states_[id]; }

    //! Retrieves all states that can be reached from @p S with one single input Symbol @p c.
    StateIdVec delta(const StateIdVec& S, Symbol c) const;
    StateIdVec* delta(const StateIdVec& S, Symbol c, StateIdVec* result) const;

    //! Retrieves all states that can be directly or indirectly accessed via epsilon-transitions exclusively.
    StateIdVec epsilonClosure(const StateIdVec& S) const;
    void epsilonClosure(const StateIdVec& S, StateIdVec* result) const;

    TransitionMap& stateTransitions(StateId s) { return states_[s]; }

    //! Flags given state as accepting-state with given Tag @p acceptTag.
    void setAccept(Tag acceptTag) { acceptTags_[acceptState_] = acceptTag; }

    void setAccept(StateId state, Tag tag) { acceptTags_[state] = tag; }

    std::optional<Tag> acceptTag(StateId s) const
    {
        if (auto i = acceptTags_.find(s); i != acceptTags_.end())
            return i->second;

        return std::nullopt;
    }

    bool isAccepting(StateId s) const { return acceptTags_.find(s) != acceptTags_.end(); }

    /**
     * Returns whether or not the StateSet @p Q contains at least one State that is also "accepting".
     */
    bool isAnyAccepting(const StateIdVec& Q) const
    {
        for (StateId q: Q)
            if (isAccepting(q))
                return true;

        return false;
    }

    const AcceptMap& acceptMap() const noexcept { return acceptTags_; }
    AcceptMap& acceptMap() noexcept { return acceptTags_; }

    std::optional<StateId> backtrack(StateId s) const
    {
        if (auto i = backtrackStates_.find(s); i != backtrackStates_.end())
            return i->second;

        return std::nullopt;
    }

    /**
     * Checks if @p Q contains a state that is flagged as backtracking state in the NFA and returns
     * the target state within the NFA or @c std::nullopt if not a backtracking state.
     */
    std::optional<StateId> containsBacktrackState(const StateIdVec& Q) const
    {
        for (StateId q: Q)
            if (std::optional<StateId> t = backtrack(q); t.has_value())
                return *t;

        return std::nullopt;
    }

  private:
    StateId createState();
    void visit(DotVisitor& v, StateId s, std::unordered_map<StateId, size_t>& registry) const;
    void prepareStateIds(StateId baseId);

    //! Retrieves all epsilon-transitions directly connected to State @p s.
    StateIdVec epsilonTransitions(StateId s) const;

  private:
    StateVec states_;
    StateId initialState_;
    StateId acceptState_;
    BacktrackingMap backtrackStates_;
    AcceptMap acceptTags_;
};

} // namespace regex_dfa
