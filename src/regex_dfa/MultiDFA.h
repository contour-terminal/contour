// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT
#pragma once

#include <regex_dfa/DFA.h>
#include <regex_dfa/State.h>
#include <regex_dfa/Symbols.h>

#include <map>
#include <string>

namespace regex_dfa
{

struct MultiDFA
{
    using InitialStateMap = std::map<std::string, StateId>;

    InitialStateMap initialStates;
    DFA dfa;
};

MultiDFA constructMultiDFA(std::map<std::string, DFA> many);

} // namespace regex_dfa
