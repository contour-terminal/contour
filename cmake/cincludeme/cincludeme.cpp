// SPDX-License-Identifier: Apache-2.0
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <utility>

#if defined(USING_BOOST_FILESYSTEM) && (USING_BOOST_FILESYSTEM)
    #include <boost/filesystem.hpp>
namespace FileSystem = boost::filesystem;
typedef boost::system::error_code FileSystemError;
#elif (!defined(__has_include) || __has_include(<filesystem>)) && !defined(__APPLE__)
    #include <filesystem>
    #include <system_error>
namespace FileSystem = std::filesystem;
typedef std::error_code FileSystemError;
#elif __has_include(<experimental/filesystem>) && !defined(__APPLE__)
    #include <system_error>

    #include <experimental/filesystem>
namespace FileSystem = std::experimental::filesystem;
typedef std::error_code FileSystemError;
#elif __has_include(<boost/filesystem.hpp>)
    #include <boost/filesystem.hpp>
namespace FileSystem = boost::filesystem;
typedef boost::system::error_code FileSystemError;
#else
    #error No filesystem implementation found.
#endif

using namespace std;

static void dump(ostream& _out, string const& _inputFile, string const& _symbolName)
{
    auto in = ifstream { _inputFile, ios::binary };
    if (!in.good())
    {
        cerr << "Could not open input file.\n";
        exit(EXIT_FAILURE);
    }

    in.seekg(0, in.end);
    auto length = in.tellg();
    _out << "constexpr std::array<uint8_t, " << length << "> " << _symbolName << " = {\n\t";

    in.seekg(0, in.beg);
    unsigned column = 0;
    while (length > 0)
    {
        char ch {};
        in.read(&ch, sizeof(ch));
        if (ch > 0 && isprint(ch) && ch != '\'' && ch != '\\')
            _out << '\'' << ch << '\'' << ',' << ' ';
        else
            _out << "0x" << std::hex << std::setw(2) << std::setfill('0')
                 << (static_cast<unsigned>(ch) & 0xFF) << ',';
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
        cerr << "Usage: " << argv[0] << " <OUTPUT_FILE> <NS> <INPUT_FILE> <INPUT_SYMBOL>\n";
        return EXIT_FAILURE;
    }

    auto const outputFileName = argv[1];
    auto const ns = string { argv[2] };
    auto const inputFileName = argv[3];
    auto const symbolName = argv[4];

    auto out = ofstream { outputFileName };

    out << "#pragma once\n\n";
    out << "#include <array>\n";
    out << "#include <cstdint>\n\n";

    if (!ns.empty())
        out << "namespace " << ns << " {\n\n";

    dump(out, inputFileName, symbolName);

    if (!ns.empty())
        out << "\n}  // namespace " << ns << "\n";

    FileSystem::last_write_time(FileSystem::path(outputFileName), FileSystem::last_write_time(inputFileName));

    return EXIT_SUCCESS;
}
