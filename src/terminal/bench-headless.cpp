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

#include <crispy/App.h>
#include <crispy/CLI.h>

#include <libtermbench/termbench.h>

#include <iostream>
#include <optional>
#include <random>

#include <fmt/format.h>

using namespace std;

class NullParserEvents
{
public:
    void error(std::string_view const& _errorString) {}
    void print(char _text) {}
    void print(std::string_view _chars) {}
    void execute(char _controlCode) {}
    void clear() {}
    void collect(char _char) {}
    void collectLeader(char _leader) {}
    void param(char _char) {}
    void dispatchESC(char _function) {}
    void dispatchCSI(char _function) {}
    void startOSC() {}
    void putOSC(char _char) {}
    void dispatchOSC() {}
    void hook(char _function) {}
    void put(char _char) {}
    void unhook() {}
    void startAPC() {}
    void putAPC(char) {}
    void dispatchAPC() {}
    void startPM() {}
    void putPM(char) {}
    void dispatchPM() {}
};

struct BenchOptions
{
    unsigned testSizeMB = 64;
    bool manyLines = false;
    bool longLines = false;
    bool sgr = false;
    bool binary = false;
};

template <typename Writer>
int baseBenchmark(Writer&& _writer, BenchOptions _options, string_view _title)
{
    if (!(_options.binary || _options.longLines || _options.manyLines || _options.sgr))
    {
        cout << "No test cases specified. Defaulting to: cat, long, sgr.\n";
        _options.manyLines = true;
        _options.longLines = true;
        _options.sgr = true;
    }

    auto const titleText = fmt::format("Running benchmark: {} (test size: {} MB)",
                                       _title, _options.testSizeMB);

    cout << titleText << '\n'
         << string(titleText.size(), '=') << '\n';

    auto tbp = contour::termbench::Benchmark{
        std::forward<Writer>(_writer),
        _options.testSizeMB,
        80,
        24,
        [&](contour::termbench::Test const& _test)
        {
            cout << fmt::format("Running test {} ...\n", _test.name);
        }
    };

    if (_options.manyLines)
        tbp.add(contour::termbench::tests::many_lines());

    if (_options.longLines)
        tbp.add(contour::termbench::tests::long_lines());

    if (_options.sgr)
    {
        tbp.add(contour::termbench::tests::sgr_fg_lines());
        tbp.add(contour::termbench::tests::sgr_fgbg_lines());
    }

    if (_options.binary)
        tbp.add(contour::termbench::tests::binary());

    tbp.runAll();

    cout << '\n';
    cout << "Results\n";
    cout << "-------\n";
    tbp.summarize(cout);
    cout << '\n';

    return EXIT_SUCCESS;
}

namespace CLI = crispy::cli;

class ContourHeadlessBench: public crispy::App
{
public:
    ContourHeadlessBench():
        App("bench-headless", "Contour Headless Benchmark", CONTOUR_VERSION_STRING, "Apache-2.0")
    {
        using Project = crispy::cli::about::Project;
        crispy::cli::about::registerProjects(
            #if defined(CONTOUR_BUILD_WITH_MIMALLOC)
            Project{"mimalloc", "", ""},
            #endif
            Project{"range-v3", "Boost Software License 1.0", "https://github.com/ericniebler/range-v3"},
            Project{"yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp"},
            Project{"termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro"},
            Project{"fmt", "MIT", "https://github.com/fmtlib/fmt"}
        );
        link("bench-headless.parser", bind(&ContourHeadlessBench::benchParserOnly, this));
        link("bench-headless.grid", bind(&ContourHeadlessBench::benchGrid, this));
        link("bench-headless.meta", bind(&ContourHeadlessBench::showMetaInfo, this));
    }

    crispy::cli::Command parameterDefinition() const override
    {
        auto const perfOptions =
            CLI::OptionList{
                CLI::Option{"size", CLI::Value{32u}, "Number of megabyte to process per test.", "MB"},
                CLI::Option{"cat", CLI::Value{false}, "Enable cat-style short-line ASCII stream test."},
                CLI::Option{"long", CLI::Value{false}, "Enable long-line ASCII stream test."},
                CLI::Option{"sgr", CLI::Value{false}, "Enable SGR stream test."},
                CLI::Option{"binary", CLI::Value{false}, "Enable binary stream test."},
            };

        return CLI::Command{
            "bench-headless",
            "Contour Terminal Emulator " CONTOUR_VERSION_STRING " - https://github.com/contour-terminal/contour/ ;-)",
            CLI::OptionList{},
            CLI::CommandList{
                CLI::Command{"help", "Shows this help and exits."},
                CLI::Command{"meta", "Shows some terminal backend meta information and exits."},
                CLI::Command{"version", "Shows the version and exits."},
                CLI::Command{"license", "Shows the license, and project URL of the used projects and Contour."},
                CLI::Command{"grid", "Shows the license, and project URL of the used projects and Contour.", perfOptions},
                CLI::Command{"parser", "Shows the license, and project URL of the used projects and Contour.", perfOptions},
            }
        };
    }

    int showMetaInfo()
    {
        // Show any interesting meta information.
        fmt::print("Cell      : {} bytes\n", sizeof(terminal::Cell));
        fmt::print("CellExtra : {} bytes\n", sizeof(terminal::CellExtra));
        fmt::print("CellFlags : {} bytes\n", sizeof(terminal::CellFlags));
        fmt::print("Color     : {} bytes\n", sizeof(terminal::Color));
        return EXIT_SUCCESS;
    }

    BenchOptions benchOptionsFor(string_view _kind)
    {
        auto const prefix = fmt::format("bench-headless.{}.", _kind);
        auto opts = BenchOptions{};
        opts.testSizeMB = parameters().uint(prefix + "size");
        opts.manyLines = parameters().boolean(prefix + "cat");
        opts.longLines = parameters().boolean(prefix + "long");
        opts.sgr = parameters().boolean(prefix + "sgr");
        opts.binary = parameters().boolean(prefix + "binary");
        return opts;
    }

    int benchGrid()
    {
        auto pageSize = terminal::PageSize{terminal::LineCount(25), terminal::ColumnCount(80)};
        auto const ptyReadBufferSize = 10000;
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

        auto const rv = baseBenchmark(
            [&](char const* a, size_t b)
            {
                pty->setReadData({a, b});
                do vt.processInputOnce();
                while (!pty->stdoutBuffer().empty());
            },
            benchOptionsFor("grid"),
            "terminal with screen buffer"
        );
        if (rv == EXIT_SUCCESS)
            cout << fmt::format("{:>12}: {}\n\n",
                                "history size",
                                *vt.screen().maxHistoryLineCount());
        return rv;
    }

    int benchParserOnly()
    {
        auto po = NullParserEvents{};
        auto parser = terminal::parser::Parser{po};
        return baseBenchmark(
            [&](char const* a, size_t b)
            {
                parser.parseFragment(string_view(a, b));
            },
            benchOptionsFor("parser"),
            "Parser only"
        );
    }
};

int main(int argc, char const* argv[])
{
    ContourHeadlessBench app;
    return app.run(argc, argv);
}
