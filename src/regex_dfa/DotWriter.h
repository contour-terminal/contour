// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/DotVisitor.h>
#include <regex_dfa/MultiDFA.h>
#include <regex_dfa/State.h>

#include <fstream>
#include <map>
#include <memory>
#include <ostream>
#include <string>
#include <string_view>

namespace regex_dfa
{

class DotWriter: public DotVisitor
{
  public:
    DotWriter(std::ostream& os, std::string stateLabelPrefix):
        ownedStream_ {},
        stream_ { os },
        stateLabelPrefix_ { stateLabelPrefix },
        transitionGroups_ {},
        initialStates_ { nullptr },
        initialState_ { 0 }
    {
    }

    DotWriter(const std::string& filename, std::string stateLabelPrefix):
        ownedStream_ { std::make_unique<std::ofstream>(filename) },
        stream_ { *ownedStream_.get() },
        stateLabelPrefix_ { stateLabelPrefix },
        transitionGroups_ {},
        initialStates_ { nullptr },
        initialState_ { 0 }
    {
    }

    DotWriter(std::ostream& os, std::string stateLabelPrefix, const MultiDFA::InitialStateMap& initialStates):
        ownedStream_ {},
        stream_ { os },
        stateLabelPrefix_ { stateLabelPrefix },
        transitionGroups_ {},
        initialStates_ { &initialStates },
        initialState_ { 0 }
    {
    }

    DotWriter(const std::string& filename,
              std::string stateLabelPrefix,
              const MultiDFA::InitialStateMap& initialStates):
        ownedStream_ { std::make_unique<std::ofstream>(filename) },
        stream_ { *ownedStream_.get() },
        stateLabelPrefix_ { stateLabelPrefix },
        transitionGroups_ {},
        initialStates_ { &initialStates },
        initialState_ { 0 }
    {
    }

  public:
    void start(StateId initialState) override;
    void visitNode(StateId number, bool start, bool accept) override;
    void visitEdge(StateId from, StateId to, Symbol s) override;
    void endVisitEdge(StateId from, StateId to) override;
    void end() override;

  private:
    std::unique_ptr<std::ostream> ownedStream_;
    std::ostream& stream_;
    std::string stateLabelPrefix_;
    std::map<StateId /*target state*/, std::vector<Symbol> /*transition symbols*/> transitionGroups_;
    const MultiDFA::InitialStateMap* initialStates_;
    StateId initialState_;
};

} // namespace regex_dfa
