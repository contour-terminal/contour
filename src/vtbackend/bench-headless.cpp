// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/cell/CellConfig.h>
#include <vtbackend/logging.h>

#include <vtparser/ParserEvents.h>

#include <vtpty/MockViewPty.h>

#include <crispy/App.h>
#include <crispy/BufferObject.h>
#include <crispy/CLI.h>
#include <crispy/utils.h>

#include <format>
#include <iostream>
#include <optional>
#include <thread>

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

    auto const titleText = std::format("Running benchmark: {} (test size: {} MB)", title, options.testSizeMB);

    cout << titleText << '\n' << string(titleText.size(), '=') << '\n';

    auto tbp = termbench::Benchmark { std::forward<Writer>(writer),
                                      options.testSizeMB,
                                      termbench::TerminalSize { .columns = 80, .lines = 24 },
                                      [&](termbench::Test const& test) {
                                          cout << std::format("Running test {} ...\n", test.name);
                                      } };

    if (options.manyLines)
        tbp.add(termbench::tests::many_lines());

    if (options.longLines)
        tbp.add(termbench::tests::long_lines());

    if (options.sgr)
    {
        tbp.add(termbench::tests::sgr_fg_lines());
        tbp.add(termbench::tests::sgr_fgbg_lines());
    }

    if (options.binary)
        tbp.add(termbench::tests::binary());

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
            CLI::option { .name = "size",
                          .v = CLI::value { 32u },
                          .helpText = "Number of megabyte to process per test.",
                          .placeholder = "MB" },
            CLI::option { .name = "cat",
                          .v = CLI::value { false },
                          .helpText = "Enable cat-style short-line ASCII stream test." },
            CLI::option { .name = "long",
                          .v = CLI::value { false },
                          .helpText = "Enable long-line ASCII stream test." },
            CLI::option { .name = "sgr", .v = CLI::value { false }, .helpText = "Enable SGR stream test." },
            CLI::option {
                .name = "binary", .v = CLI::value { false }, .helpText = "Enable binary stream test." },
        };

        return CLI::command {
            .name = "bench-headless",
            .helpText = "Contour Terminal Emulator " CONTOUR_VERSION_STRING
                        " - https://github.com/contour-terminal/contour/ ;-)",
            .options = CLI::option_list {},
            .children =
                CLI::command_list {
                    CLI::command { .name = "help", .helpText = "Shows this help and exits." },
                    CLI::command { .name = "meta",
                                   .helpText = "Shows some terminal backend meta information and exits." },
                    CLI::command { .name = "version", .helpText = "Shows the version and exits." },
                    CLI::command {
                        .name = "license",
                        .helpText = "Shows the license, and project URL of the used projects and Contour." },
                    CLI::command {
                        .name = "grid",
                        .helpText = "Performs performance tests utilizing the full grid including VT parser.",
                        .options = perfOptions },
                    CLI::command { .name = "parser",
                                   .helpText = "Performs performance tests utilizing the VT parser only.",
                                   .options = perfOptions },
                    CLI::command { .name = "pty",
                                   .helpText = "Performs performance tests utilizing the underlying "
                                               "operating system's PTY only." },
                }
        };
    }

    static int showMetaInfo()
    {
        // Show any interesting meta information.
        std::cout << std::format("SimpleCell  : {} bytes\n", sizeof(vtbackend::SimpleCell));
        std::cout << std::format("CompactCell : {} bytes\n", sizeof(vtbackend::CompactCell));
        std::cout << std::format("CellExtra   : {} bytes\n", sizeof(vtbackend::CellExtra));
        std::cout << std::format("CellFlags   : {} bytes\n", sizeof(vtbackend::CellFlags));
        std::cout << std::format("Color       : {} bytes\n", sizeof(vtbackend::Color));
        return EXIT_SUCCESS;
    }

    BenchOptions benchOptionsFor(string_view kind)
    {
        auto const prefix = std::format("bench-headless.{}.", kind);
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
        auto pageSize = vtbackend::PageSize { vtbackend::LineCount(25), vtbackend::ColumnCount(80) };
        size_t const ptyReadBufferSize = 1'000'000;
        auto maxHistoryLineCount = vtbackend::LineCount(4000);
        auto vt = vtbackend::MockTerm<vtpty::MockViewPty>(pageSize, maxHistoryLineCount, ptyReadBufferSize);
        auto* pty = dynamic_cast<vtpty::MockViewPty*>(&vt.terminal.device());
        vt.terminal.setMode(vtbackend::DECMode::AutoWrap, true);

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
            cout << std::format("{:>12}: {}\n\n", "history size", *vt.terminal.maxHistoryLineCount());
        return rv;
    }

    static int benchPTY()
    {
        using std::chrono::steady_clock;
        using vtpty::ColumnCount;
        using vtpty::createPty;
        using vtpty::LineCount;
        using vtpty::PageSize;
        using vtpty::Pty;

        // Benchmark configuration
        // TODO make these values CLI configurable.
        auto constexpr WritesPerLoop = 1;
        auto constexpr PtyWriteSize = 4096;
        auto constexpr PtyReadSize = 4096;
        auto const benchTime = chrono::seconds(10);

        // Setup benchmark
        std::string const text = createText(PtyWriteSize);
        unique_ptr<Pty> ptyObject =
            createPty(PageSize { .lines = LineCount(25), .columns = ColumnCount(80) }, std::nullopt);
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
                auto const dataChunk = readResult.value().data;
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
        std::cout << std::format("Running PTY benchmark ...\n");
        auto const startTime = steady_clock::now();
        auto stopTime = startTime;
        while (stopTime - startTime < benchTime)
        {
            for (int i = 0; i < WritesPerLoop; ++i)
                (void) ptySlave.write(text);
            stopTime = steady_clock::now();
        }

        cleanupReader.run();

        // Create summary
        auto const elapsedTime = stopTime - startTime;
        auto const msecs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTime);
        auto const secs = std::chrono::duration_cast<std::chrono::seconds>(elapsedTime);
        auto const mbPerSecs =
            static_cast<long double>(bytesTransferred) / static_cast<long double>(secs.count());

        std::cout << std::format("\n");
        std::cout << std::format("PTY stdout throughput bandwidth test\n");
        std::cout << std::format("====================================\n\n");
        std::cout << std::format("Writes per loop        : {}\n", WritesPerLoop);
        std::cout << std::format("PTY write size         : {}\n", PtyWriteSize);
        std::cout << std::format("PTY read size          : {}\n", PtyReadSize);
        std::cout << std::format(
            "Test time              : {}.{:03} seconds\n", msecs.count() / 1000, msecs.count() % 1000);
        std::cout << std::format("Data transferred       : {}\n",
                                 crispy::humanReadableBytes(bytesTransferred));
        std::cout << std::format("Reader loop iterations : {}\n", loopIterations);
        std::cout << std::format(
            "Average size per read  : {}\n",
            crispy::humanReadableBytes(static_cast<uint64_t>(static_cast<long double>(bytesTransferred)
                                                             / static_cast<long double>(loopIterations))));
        std::cout << std::format("Transfer speed         : {} per second\n",
                                 crispy::humanReadableBytes(static_cast<uint64_t>(mbPerSecs)));

        return EXIT_SUCCESS;
    }

    int benchParserOnly()
    {
        auto po = vtparser::NullParserEvents {};
        auto parser = vtparser::Parser<vtparser::ParserEvents> { po };
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
