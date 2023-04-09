// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/MultiDFA.h>

using namespace std;

namespace regex_dfa
{

MultiDFA constructMultiDFA(map<string, DFA> many)
{
    MultiDFA multiDFA {};
    multiDFA.dfa.createStates(1 + many.size());
    multiDFA.dfa.setInitialState(0);

    StateId q0 = 1;
    for (pair<const string, DFA>& p: many)
    {
        multiDFA.dfa.append(move(p.second), q0);
        multiDFA.initialStates[p.first] = q0;
        multiDFA.dfa.setTransition(0, static_cast<Symbol>(q0), q0);
        q0++;
    }

    return multiDFA;
}

} // namespace regex_dfa
