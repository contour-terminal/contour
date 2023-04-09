// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/Alphabet.h>
#include <regex_dfa/MultiDFA.h>
#include <regex_dfa/State.h>

#include <cassert>
#include <cstdlib>
#include <list>
#include <optional>
#include <unordered_map>
#include <vector>

namespace regex_dfa
{

class DFA;

class DFAMinimizer
{
  public:
    explicit DFAMinimizer(const DFA& dfa);
    explicit DFAMinimizer(const MultiDFA& multiDFA);

    DFA constructDFA();
    MultiDFA constructMultiDFA();

  private:
    using PartitionVec = std::list<StateIdVec>;

    void constructPartitions();
    StateIdVec nonAcceptStates() const;
    bool containsInitialState(const StateIdVec& S) const;
    bool isMultiInitialState(StateId s) const;
    PartitionVec::iterator findGroup(StateId s);
    int partitionId(StateId s) const;
    PartitionVec split(const StateIdVec& S) const;
    DFA constructFromPartitions(const PartitionVec& P) const;
    std::optional<StateId> containsBacktrackState(const StateIdVec& Q) const;

    static void dumpGroups(const PartitionVec& T);

    StateId targetStateId(StateId oldId) const
    {
        auto i = targetStateIdMap_.find(oldId);
        assert(i != targetStateIdMap_.end());
        return i->second;
    }

  private:
    const DFA& dfa_;
    const MultiDFA::InitialStateMap initialStates_;
    const Alphabet alphabet_;
    PartitionVec T;
    PartitionVec P;
    std::unordered_map<StateId, StateId> targetStateIdMap_;
};

} // namespace regex_dfa
