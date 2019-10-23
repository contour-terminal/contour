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

int main(int argc, char const* argv[])
{
	if (argc != 5)
	{
		cerr << "Usage: " << argv[0] << " <INPUT_FILE> <OUTPUT_FILE> <SYMBOL_NAME>\n";
		return EXIT_FAILURE;
	}

	auto in = ifstream{argv[1]};
	auto out = ofstream{argv[2]};
	auto const name = string{argv[3]};
	auto const ns = string{argv[4]};

	if (!in.good())
	{
		cerr << "Could not open input file.\n";
		return EXIT_FAILURE;
	}

	in.seekg(0, in.end);
	auto length = in.tellg();

	out << "#pragma once\n\n";
	out << "#include <array>\n\n";

	if (!ns.empty())
		out << "namespace " << ns << " {\n\n";

	out << "constexpr std::array<char, " << length << "> " << name << " = {\n\t";

	in.seekg(0, in.beg);
	unsigned column = 0;
	while (length > 0)
	{
		char ch{};
		in.read(&ch, sizeof(ch));
		if (!isprint(ch) || ch == '\'')
			out << "0x" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch) << ',';
		else
			out << '\'' << ch << '\'' << ',' << ' ';
		++column;
		if (column % 16)
			out << ' ';
		else
		{
			column = 0;
			out << "\n\t";
		}
		length -= sizeof(ch);
	}
	out << "\n};\n";

	if (!ns.empty())
		out << "\n}  // namespace " << ns << "\n";

	return EXIT_SUCCESS;
}
