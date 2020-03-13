/**
 * This file is part of the "contour" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <utility>

using namespace std;

void dump(ostream& _out, string const& _inputFile, string const& _symbolName)
{
    auto in = ifstream{_inputFile};
    if (!in.good())
    {
        cerr << "Could not open input file.\n";
        exit(EXIT_FAILURE);
    }

    in.seekg(0, in.end);
    auto length = in.tellg();
    _out << "constexpr std::array<char, " << length << "> " << _symbolName << " = {\n\t";

    in.seekg(0, in.beg);
    unsigned column = 0;
    while (length > 0)
    {
        char ch{};
        in.read(&ch, sizeof(ch));
        if (!isprint(ch) || ch == '\'')
            _out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch) << ',';
        else
            _out << '\'' << ch << '\'' << ',' << ' ';
        ++column;
        if (column % 16)
            _out << ' ';
        else
        {
            column = 0;
            _out << "\n\t";
        }
        length -= sizeof(ch);
    }
    _out << "\n};\n";
}

int main(int argc, char const* argv[])
{
    if (argc != 5)
    {
        cerr << "Usage: " << argv[0] << " <OUTPUT_FILE> <NS> (<INPUT_FILE> <INPUT_SYMBOL>)...\n";
        return EXIT_FAILURE;
    }

    auto out = ofstream{argv[1]};
    auto const ns = string{argv[2]};

    out << "#pragma once\n\n";
    out << "#include <array>\n\n";

    if (!ns.empty())
        out << "namespace " << ns << " {\n\n";

    for (int i = 3; i < argc; i += 2)
    {
        auto const inputFile = string{argv[i]};
        auto const symbolName = string{argv[i + 1]};
        dump(out, inputFile, symbolName);
    }

    if (!ns.empty())
        out << "\n}  // namespace " << ns << "\n";

    return EXIT_SUCCESS;
}
