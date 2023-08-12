// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/Alphabet.h>
#include <regex_dfa/State.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace regex_dfa
{

class NFA;
class DFABuilder;
class DotVisitor;

/**
 * Represents a deterministic finite automaton.
 */
class DFA
{
  public:
    using TransitionMap = std::map<Symbol, StateId>;
    struct State
    {
        // std::vector<StateId> states;
        TransitionMap transitions;
    };
    using StateVec = std::vector<State>;

    //! defines a mapping between accept state ID and another (prior) ID to track roll back the input stream
    //! to.
    using BacktrackingMap = std::map<StateId, StateId>;

    DFA(const DFA& other) = delete;
    DFA& operator=(const DFA& other) = delete;
    DFA(DFA&&) = default;
    DFA& operator=(DFA&&) = default;
    ~DFA() = default;

    DFA(): states_ {}, initialState_ { 0 }, backtrackStates_ {}, acceptTags_ {} {}

    [[nodiscard]] bool empty() const noexcept { return states_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return states_.size(); }

    [[nodiscard]] StateId lastState() const noexcept
    {
        assert(!empty());
        return states_.size() - 1;
    }

    //! Retrieves the alphabet of this finite automaton.
    [[nodiscard]] Alphabet alphabet() const;

    //! Retrieves the initial state.
    [[nodiscard]] StateId initialState() const { return initialState_; }

    //! Retrieves the list of available states.
    [[nodiscard]] const StateVec& states() const { return states_; }
    [[nodiscard]] StateVec& states() { return states_; }

    [[nodiscard]] StateIdVec stateIds() const
    {
        StateIdVec v;
        v.reserve(states_.size());
        for (size_t i = 0, e = states_.size(); i != e; ++i)
            v.push_back(i); // funny, I know
        return v;
    }

    //! Retrieves the list of accepting states.
    [[nodiscard]] std::vector<StateId> acceptStates() const;

    /**
     * Traverses all states and edges in this NFA and calls @p visitor for each state & edge.
     *
     * Use this function to e.g. get a GraphViz dot-file drawn.
     */
    void visit(DotVisitor& visitor) const;

    void createStates(size_t count);

    void setInitialState(StateId state);

    [[nodiscard]] const TransitionMap& stateTransitions(StateId id) const
    {
        return states_[static_cast<size_t>(id)].transitions;
    }

    // {{{ backtracking (for lookahead)
    void setBacktrack(StateId from, StateId to) { backtrackStates_[from] = to; }

    [[nodiscard]] std::optional<StateId> backtrack(StateId acceptState) const
    {
        if (auto i = backtrackStates_.find(acceptState); i != backtrackStates_.end())
            return i->second;

        return std::nullopt;
    }

    [[nodiscard]] const BacktrackingMap& backtracking() const noexcept { return backtrackStates_; }
    // }}}

    //! Flags given state as accepting-state with given Tag @p acceptTag.
    void setAccept(StateId state, Tag acceptTag) { acceptTags_[state] = acceptTag; }

    [[nodiscard]] bool isAccepting(StateId s) const { return acceptTags_.find(s) != acceptTags_.end(); }

    [[nodiscard]] std::optional<Tag> acceptTag(StateId s) const
    {
        if (auto i = acceptTags_.find(s); i != acceptTags_.end())
            return i->second;

        return std::nullopt;
    }

    [[nodiscard]] std::optional<StateId> delta(StateId state, Symbol symbol) const
    {
        const auto& T = states_[state].transitions;
        if (auto i = T.find(symbol); i != T.end())
            return i->second;

        return std::nullopt;
    }

    void setTransition(StateId from, Symbol symbol, StateId to);
    void removeTransition(StateId from, Symbol symbol);

    [[nodiscard]] StateIdVec nonAcceptStates() const
    {
        StateIdVec result;
        result.reserve(
            std::abs(static_cast<long int>(states_.size()) - static_cast<long int>(acceptTags_.size())));

        for (StateId s = 0, sE = size(); s != sE; ++s)
            if (!isAccepting(s))
                result.push_back(s);

        return result;
    }

    [[nodiscard]] bool isAcceptor(Tag t) const
    {
        for (std::pair<StateId, Tag> p: acceptTags_)
            if (p.second == t)
                return true;

        return false;
    }

    StateId append(DFA&& other, StateId q0);

  private:
    void prepareStateIds(StateId baseId, StateId q0);

  private:
    StateVec states_;
    StateId initialState_;
    BacktrackingMap backtrackStates_;
    AcceptMap acceptTags_;
};

} // namespace regex_dfa
