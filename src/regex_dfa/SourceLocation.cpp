// This file is part of the "klex" project, http://github.com/christianparpart/klex>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <fstream>

#include <regex_dfa/SourceLocation.h>

using namespace std;

namespace regex_dfa
{

string SourceLocation::source() const // TODO
{
    string code;
    ifstream ifs(filename);
    ifs.seekg(offset, ifs.beg);
    code.resize(count);
    ifs.read(&code[0], count);
    return code;
}

} // namespace regex_dfa
