/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2021 Christian Parpart <christian@parpart.family>
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

#include <terminal/Terminal.h>
#include <terminal/logging.h>
#include <terminal/pty/MockViewPty.h>

#include <libtermbench/termbench.h>

#include <iostream>
#include <optional>
#include <random>

#include <fmt/format.h>

using namespace std;

class NullParserEvents: public terminal::ParserEvents
{
public:
    void error(std::string_view const& _errorString) override {}
    void print(char32_t _text) override {}
    void print(std::string_view _chars) override {}
    void execute(char _controlCode) override {}
    void clear() override {}
    void collect(char _char) override {}
    void collectLeader(char _leader) override {}
    void param(char _char) override {}
    void dispatchESC(char _function) override {}
    void dispatchCSI(char _function) override {}
    void startOSC() override {}
    void putOSC(char32_t _char) override {}
    void dispatchOSC() override {}
    void hook(char _function) override {}
    void put(char32_t _char) override {}
    void unhook() override {}
    void startAPC() override {}
    void putAPC(char32_t) override {}
    void dispatchAPC() override {}
};

template <typename Writer>
void baseBenchmark(Writer&& _writer, size_t _testSizeMB, string_view _title)
{
    auto const titleText = fmt::format("Running benchmark: {}", _title);
    std::cout << titleText << '\n'
              << std::string(titleText.size(), '=') << '\n';

    auto tbp = contour::termbench::Benchmark{
        std::forward<Writer>(_writer),
        _testSizeMB,
        80,
        24,
        [&](contour::termbench::Test const& _test)
        {
            cout << fmt::format("Running test {} ...\n", _test.name);
        }
    };

    tbp.add(contour::termbench::tests::many_lines());
    tbp.add(contour::termbench::tests::long_lines());
    tbp.add(contour::termbench::tests::sgr_fg_lines());
    tbp.add(contour::termbench::tests::sgr_fgbg_lines());
    // tbp.add(contour::termbench::tests::binary());

    tbp.runAll();

    cout << '\n';
    cout << "Results\n";
    cout << "-------\n";
    tbp.summarize(cout);
    cout << '\n';
}

void benchmarkParserOnly()
{
    auto po = NullParserEvents{};
    auto parser = terminal::parser::Parser{po};
    baseBenchmark(
        [&](char const* a, size_t b)
        {
            parser.parseFragment(string_view(a, b));
        },
        1024, // MB
        "Parser only"sv
    );
}

void benchmarkTerminal()
{
    auto const testSizeMB = 32;
    auto pageSize = terminal::PageSize{terminal::LineCount(25), terminal::ColumnCount(80)};
    auto const ptyReadBufferSize = 8192;
    auto maxHistoryLineCount = terminal::LineCount(4096);
    auto eh = terminal::Terminal::Events{};
    auto pty = std::make_unique<terminal::MockViewPty>(pageSize);
    auto vt = terminal::Terminal{
        *pty,
        ptyReadBufferSize,
        eh,
        maxHistoryLineCount
    };
    vt.screen().setMode(terminal::DECMode::AutoWrap, true);

    baseBenchmark(
        [&](char const* a, size_t b)
        {
            pty->setReadData({a, b});
            do vt.processInputOnce();
            while (!pty->stdoutBuffer().empty());
        },
        testSizeMB,
        "terminal with screen buffer"
    );

    cout << fmt::format("{:>12}: {}\n\n",
                        "history size",
                        vt.screen().maxHistoryLineCount().value_or(terminal::LineCount(0)));
}

int main(int argc, char const* argv[])
{
    benchmarkTerminal();
    benchmarkParserOnly();

    return EXIT_SUCCESS;
}
