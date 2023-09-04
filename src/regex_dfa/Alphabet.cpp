// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <regex_dfa/Alphabet.h>
#include <regex_dfa/Symbols.h>

#include <iomanip>
#include <iostream>
#include <sstream>

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

void Alphabet::insert(Symbol ch)
{
    if (_alphabet.find(ch) == _alphabet.end())
    {
        DEBUG("Alphabet: insert '{:}'", prettySymbol(ch));
        _alphabet.insert(ch);
    }
}

string Alphabet::to_string() const
{
    stringstream sstr;

    sstr << '{';

    for (Symbol c: _alphabet)
        sstr << prettySymbol(c);

    sstr << '}';

    return sstr.str();
}

} // namespace regex_dfa
