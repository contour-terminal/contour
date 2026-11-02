// SPDX-License-Identifier: Apache-2.0
// Regenerates the terminal-state golden of the format this build writes:
//   vtbackend_make_golden <directory>
// writes <directory>/terminal-state-v<N>.bin and <directory>/terminal-state-v<N>.txt. A golden of an
// older format is never regenerated: it is what that format looked like when it shipped.
#include <vtbackend/state/TerminalStateCodec.hpp>
#include <vtbackend/state/golden/GoldenSession.hpp>

#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <span>

int main(int argc, char** argv)
{
    using namespace vtbackend;
    if (argc != 2)
    {
        std::cerr << "usage: vtbackend_make_golden <directory>\n";
        return EXIT_FAILURE;
    }
    auto const directory = std::filesystem::path { std::span<char*> { argv, static_cast<size_t>(argc) }[1] };
    auto const stem = directory / std::format("terminal-state-v{}", TerminalStateFormatVersion);

    auto const mock = test::goldenSetup().makeTerminal();
    mock->writeToScreen(test::goldenSession());
    mock->terminal.flushInput();
    auto const bytes = encodeTerminalState(mock->terminal.captureState());
    auto const text = test::describeTerminal(*mock);

    auto bin = std::ofstream { std::filesystem::path { stem }.concat(".bin"), std::ios::binary };
    bin.write(reinterpret_cast<char const*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    auto txt = std::ofstream { std::filesystem::path { stem }.concat(".txt"), std::ios::binary };
    txt << text;
    if (!bin || !txt)
    {
        std::cerr << std::format("cannot write the golden files under {}\n", directory.string());
        return EXIT_FAILURE;
    }
    std::cout << std::format("wrote {} bytes and {} bytes of text for format v{}\n",
                             bytes.size(),
                             text.size(),
                             TerminalStateFormatVersion);
    return EXIT_SUCCESS;
}
