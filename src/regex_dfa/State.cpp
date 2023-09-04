// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/State.h>

#include <sstream>

using namespace std;

namespace regex_dfa
{

string to_string(const StateIdVec& S, string_view stateLabelPrefix)
{
    StateIdVec names = S;
    sort(names.begin(), names.end());

    stringstream sstr;
    sstr << "{";
    int i = 0;
    for (StateId name: names)
    {
        if (i)
            sstr << ", ";
        sstr << stateLabelPrefix << name;
        i++;
    }
    sstr << "}";

    return sstr.str();
}

} // namespace regex_dfa
