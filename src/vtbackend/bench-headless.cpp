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

#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/logging.h>

#include <vtpty/MockViewPty.h>

#include <crispy/App.h>
#include <crispy/CLI.h>
#include <crispy/utils.h>

#include <fmt/format.h>

#include <iostream>
#include <optional>
#include <random>
#include <thread>

#include "crispy/BufferObject.h"
#include <libtermbench/termbench.h>

using namespace std;

namespace
{

std::string createText(size_t bytes)
{
    std::string text;
    while (text.size() < bytes)
    {
        text += char('A' + (rand() % 26));
        if ((text.size() % 65) == 0)
            text += '\n';
    }
    return text;
}

} // namespace

struct BenchOptions
{
    unsigned testSizeMB = 64;
    bool manyLines = false;
    bool longLines = false;
    bool sgr = false;
    bool binary = false;
};

template <typename Writer>
int baseBenchmark(Writer&& writer, BenchOptions options, string_view title)
{
    if (!(options.binary || options.longLines || options.manyLines || options.sgr))
    {
        cout << "No test cases specified. Defaulting to: cat, long, sgr.\n";
        options.manyLines = true;
        options.longLines = true;
        options.sgr = true;
    }

    auto const titleText = fmt::format("Running benchmark: {} (test size: {} MB)", title, options.testSizeMB);

    cout << titleText << '\n' << string(titleText.size(), '=') << '\n';

    auto tbp = contour::termbench::Benchmark { std::forward<Writer>(writer),
                                               options.testSizeMB,
                                               80,
                                               24,
                                               [&](contour::termbench::Test const& test) {
                                                   cout << fmt::format("Running test {} ...\n", test.name);
                                               } };

    if (options.manyLines)
        tbp.add(contour::termbench::tests::many_lines());

    if (options.longLines)
        tbp.add(contour::termbench::tests::long_lines());

    if (options.sgr)
    {
        tbp.add(contour::termbench::tests::sgr_fg_lines());
        tbp.add(contour::termbench::tests::sgr_fgbg_lines());
    }

    if (options.binary)
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

class ContourHeadlessBench: public crispy::app
{
  public:
    ContourHeadlessBench():
        app("bench-headless", "Contour Headless Benchmark", CONTOUR_VERSION_STRING, "Apache-2.0")
    {
        using Project = crispy::cli::about::project;
        crispy::cli::about::registerProjects(
#if defined(CONTOUR_BUILD_WITH_MIMALLOC)
            Project { "mimalloc", "", "" },
#endif
            Project { "range-v3", "Boost Software License 1.0", "https://github.com/ericniebler/range-v3" },
            Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
            Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" },
            Project { "fmt", "MIT", "https://github.com/fmtlib/fmt" });
        link("bench-headless.parser", bind(&ContourHeadlessBench::benchParserOnly, this));
        link("bench-headless.grid", bind(&ContourHeadlessBench::benchGrid, this));
        link("bench-headless.pty", bind(&ContourHeadlessBench::benchPTY));
        link("bench-headless.meta", bind(&ContourHeadlessBench::showMetaInfo));

        char const* logFilterString = getenv("LOG");
        if (logFilterString)
        {
            logstore::configure(logFilterString);
            crispy::app::customizeLogStoreOutput();
        }
    }

    [[nodiscard]] crispy::cli::command parameterDefinition() const override
    {
        auto const perfOptions = CLI::option_list {
            CLI::option { "size", CLI::value { 32u }, "Number of megabyte to process per test.", "MB" },
            CLI::option { "cat", CLI::value { false }, "Enable cat-style short-line ASCII stream test." },
            CLI::option { "long", CLI::value { false }, "Enable long-line ASCII stream test." },
            CLI::option { "sgr", CLI::value { false }, "Enable SGR stream test." },
            CLI::option { "binary", CLI::value { false }, "Enable binary stream test." },
        };

        return CLI::command {
            "bench-headless",
            "Contour Terminal Emulator " CONTOUR_VERSION_STRING
            " - https://github.com/contour-terminal/contour/ ;-)",
            CLI::option_list {},
            CLI::command_list {
                CLI::command { "help", "Shows this help and exits." },
                CLI::command { "meta", "Shows some terminal backend meta information and exits." },
                CLI::command { "version", "Shows the version and exits." },
                CLI::command { "license",
                               "Shows the license, and project URL of the used projects and Contour." },
                CLI::command { "grid",
                               "Performs performance tests utilizing the full grid including VT parser.",
                               perfOptions },
                CLI::command {
                    "parser", "Performs performance tests utilizing the VT parser only.", perfOptions },
                CLI::command {
                    "pty",
                    "Performs performance tests utilizing the underlying operating system's PTY only." },
            }
        };
    }

    static int showMetaInfo()
    {
        // Show any interesting meta information.
        fmt::print("SimpleCell  : {} bytes\n", sizeof(terminal::SimpleCell));
        fmt::print("CompactCell : {} bytes\n", sizeof(terminal::CompactCell));
        fmt::print("CellExtra   : {} bytes\n", sizeof(terminal::CellExtra));
        fmt::print("CellFlags   : {} bytes\n", sizeof(terminal::CellFlags));
        fmt::print("Color       : {} bytes\n", sizeof(terminal::Color));
        return EXIT_SUCCESS;
    }

    BenchOptions benchOptionsFor(string_view kind)
    {
        auto const prefix = fmt::format("bench-headless.{}.", kind);
        auto opts = BenchOptions {};
        opts.testSizeMB = parameters().uint(prefix + "size");
        opts.manyLines = parameters().boolean(prefix + "cat");
        opts.longLines = parameters().boolean(prefix + "long");
        opts.sgr = parameters().boolean(prefix + "sgr");
        opts.binary = parameters().boolean(prefix + "binary");
        return opts;
    }

    int benchGrid()
    {
        auto pageSize = terminal::PageSize { terminal::LineCount(25), terminal::ColumnCount(80) };
        size_t const ptyReadBufferSize = 1'000'000;
        auto maxHistoryLineCount = terminal::LineCount(4000);
        auto vt = terminal::MockTerm<terminal::MockViewPty>(pageSize, maxHistoryLineCount, ptyReadBufferSize);
        auto* pty = dynamic_cast<terminal::MockViewPty*>(&vt.terminal.device());
        vt.terminal.setMode(terminal::DECMode::AutoWrap, true);

        auto const rv = baseBenchmark(
            [&](char const* a, size_t b) -> bool {
                if (pty->isClosed())
                    return false;
                // clang-format off
                // vt.writeToScreen(string_view(a, b));
                pty->setReadData({ a, b });
                do vt.terminal.processInputOnce();
                while (!pty->isClosed() && !pty->stdoutBuffer().empty());
                // clang-format on
                return true;
            },
            benchOptionsFor("grid"),
            "terminal with screen buffer");
        if (rv == EXIT_SUCCESS)
            cout << fmt::format("{:>12}: {}\n\n", "history size", *vt.terminal.maxHistoryLineCount());
        return rv;
    }

    static int benchPTY()
    {
        using std::chrono::steady_clock;
        using terminal::ColumnCount;
        using terminal::createPty;
        using terminal::LineCount;
        using terminal::PageSize;
        using terminal::Pty;

        // Benchmark configuration
        // TODO make these values CLI configurable.
        auto constexpr WritesPerLoop = 1;
        auto constexpr PtyWriteSize = 4096;
        auto constexpr PtyReadSize = 4096;
        auto const BenchTime = chrono::seconds(10);

        // Setup benchmark
        std::string const text = createText(PtyWriteSize);
        unique_ptr<Pty> ptyObject = createPty(PageSize { LineCount(25), ColumnCount(80) }, std::nullopt);
        auto& pty = *ptyObject;
        auto& ptySlave = pty.slave();
        (void) ptySlave.configure();

        auto bufferObjectPool = crispy::buffer_object_pool<char>(4llu * 1024 * 1024);
        auto bufferObject = bufferObjectPool.allocateBufferObject();

        auto bytesTransferred = uint64_t { 0 };
        auto loopIterations = uint64_t { 0 };
        auto ptyStdoutReaderThread = std::thread { [&]() {
            while (!pty.isClosed())
            {
                auto const readResult = pty.read(*bufferObject, std::chrono::seconds(2), PtyReadSize);
                if (!readResult)
                    break;
                auto const dataChunk = get<string_view>(readResult.value());
                if (dataChunk.empty())
                    break;
                bytesTransferred += dataChunk.size();
                loopIterations++;
            }
        } };
        auto cleanupReader = crispy::finally { [&]() {
            pty.close();
            ptyStdoutReaderThread.join();
        } };

        // Perform benchmark
        fmt::print("Running PTY benchmark ...\n");
        auto const startTime = steady_clock::now();
        auto stopTime = startTime;
        while (stopTime - startTime < BenchTime)
        {
            for (int i = 0; i < WritesPerLoop; ++i)
                (void) ptySlave.write(text);
            stopTime = steady_clock::now();
        }

        cleanupReader.perform();

        // Create summary
        auto const elapsedTime = stopTime - startTime;
        auto const msecs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTime);
        auto const secs = std::chrono::duration_cast<std::chrono::seconds>(elapsedTime);
        auto const mbPerSecs =
            static_cast<long double>(bytesTransferred) / static_cast<long double>(secs.count());

        fmt::print("\n");
        fmt::print("PTY stdout throughput bandwidth test\n");
        fmt::print("====================================\n\n");
        fmt::print("Writes per loop        : {}\n", WritesPerLoop);
        fmt::print("PTY write size         : {}\n", PtyWriteSize);
        fmt::print("PTY read size          : {}\n", PtyReadSize);
        fmt::print("Test time              : {}.{:03} seconds\n", msecs.count() / 1000, msecs.count() % 1000);
        fmt::print("Data transferred       : {}\n", crispy::humanReadableBytes(bytesTransferred));
        fmt::print("Reader loop iterations : {}\n", loopIterations);
        fmt::print("Average size per read  : {}\n",
                   crispy::humanReadableBytes(static_cast<long double>(bytesTransferred)
                                              / static_cast<long double>(loopIterations)));
        fmt::print("Transfer speed         : {} per second\n", crispy::humanReadableBytes(mbPerSecs));

        return EXIT_SUCCESS;
    }

    int benchParserOnly()
    {
        auto po = terminal::NullParserEvents {};
        auto parser = terminal::parser::Parser<terminal::ParserEvents> { po };
        return baseBenchmark(
            [&](char const* a, size_t b) -> bool {
                parser.parseFragment(string_view(a, b));
                return true;
            },
            benchOptionsFor("parser"),
            "Parser only");
    }
};

int main(int argc, char const* argv[])
{
    srand(static_cast<unsigned int>(time(nullptr))); // initialize rand(). No strong seed required.

    ContourHeadlessBench app;
    return app.run(argc, argv);
}
