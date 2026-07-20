// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/MockTerm.h>
#include <vtbackend/Terminal.h>
#include <vtbackend/logging.h>

#include <vtparser/ParserEvents.h>

#include <vtpty/MockViewPty.h>

#include <crispy/App.h>
#include <crispy/BufferObject.h>
#include <crispy/CLI.h>
#include <crispy/utils.h>

#include <chrono>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <thread>

#include <libtermbench/termbench.h>

using namespace std;
using namespace std::string_literals;

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

struct BenchOptions
{
    unsigned testSizeMB = 64;
    bool manyLines = false;
    bool longLines = false;
    bool sgr = false;
    bool binary = false;
};

} // namespace

template <typename Writer>
static int baseBenchmark(Writer&& writer, BenchOptions options, string_view title)
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

/// Reads a whole file into memory.
/// @param path File to read.
/// @return Its bytes, or nullopt when it cannot be read.
static std::optional<std::string> readWholeFile(std::string const& path)
{
    auto file = std::ifstream(path, std::ios::binary);
    if (!file)
        return std::nullopt;
    return std::string { std::istreambuf_iterator<char> { file }, std::istreambuf_iterator<char> {} };
}

/// Feeds one sixel frame through a headless terminal repeatedly, reporting decode throughput.
///
/// This is where a terminal spends its time while an application streams sixel at it: VT parse,
/// sixel decode, and placement into the grid. Rendering is deliberately excluded, which makes the
/// number stable across runs and the binary small enough to profile under callgrind.
///
/// @param sixelData    One complete sixel sequence (DCS ... ST).
/// @param iterations   How many times to feed it.
/// @param pageSize     The grid to decode into.
/// @param cellSize     Pixel size of one grid cell.
/// @param maxImageSize The image canvas ceiling, as a display would set it.
/// @return EXIT_SUCCESS.
static int benchSixelStream(std::string const& sixelData,
                            unsigned iterations,
                            vtbackend::PageSize pageSize,
                            vtbackend::ImageSize cellSize,
                            vtbackend::ImageSize maxImageSize)
{
    auto vt = vtbackend::MockTerm<>(pageSize, vtbackend::LineCount(0), 1'000'000);
    vt.terminal.setCellPixelSize(cellSize);
    vt.terminal.setImageCanvasCeiling(maxImageSize);

    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto const iteration: crispy::times(iterations))
    {
        vt.writeToScreen("\033[H");
        vt.writeToScreen(sixelData);
    }
    auto const elapsed = std::chrono::steady_clock::now() - start;

    auto const seconds = std::chrono::duration<double>(elapsed).count();
    auto const totalBytes = static_cast<double>(sixelData.size()) * iterations;
    auto const msPerFrame = (seconds * 1000.0) / iterations;

    cout << std::format("Sixel decode throughput\n"
                        "-----------------------\n"
                        "  frame size    : {} bytes\n"
                        "  grid          : {} x {} cells of {} x {} px\n"
                        "  iterations    : {}\n"
                        "  elapsed       : {:.3f} s\n"
                        "  per frame     : {:.3f} ms ({:.1f} fps equivalent)\n"
                        "  throughput    : {:.2f} MiB/s\n",
                        sixelData.size(),
                        pageSize.columns,
                        pageSize.lines,
                        cellSize.width,
                        cellSize.height,
                        iterations,
                        seconds,
                        msPerFrame,
                        1000.0 / msPerFrame,
                        totalBytes / seconds / (1024.0 * 1024.0));
    return EXIT_SUCCESS;
}

namespace CLI = crispy::cli;

namespace
{
class ContourHeadlessBench: public crispy::app
{
  public:
    ContourHeadlessBench():
        app("bench-headless", "Contour Headless Benchmark", CONTOUR_VERSION_STRING, "Apache-2.0")
    {
        using Project = crispy::cli::about::project;
        crispy::cli::about::registerProjects(
#ifdef CONTOUR_BUILD_WITH_MIMALLOC
            Project { "mimalloc", "", "" },
#endif
            Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
            Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" },
            Project { "fmt", "MIT", "https://github.com/fmtlib/fmt" });
        link("bench-headless.parser", bind(&ContourHeadlessBench::benchParserOnly, this));
        link("bench-headless.grid", bind(&ContourHeadlessBench::benchGrid, this));
        link("bench-headless.sixel", bind(&ContourHeadlessBench::benchSixel, this));
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
                    CLI::command {
                        .name = "sixel",
                        .helpText = "Measures sixel decode throughput: VT parse, sixel decode and "
                                    "placement into the grid, with no rendering.",
                        .options =
                            CLI::option_list {
                                CLI::option { .name = "file",
                                              .v = CLI::value { ""s },
                                              .helpText = "File holding one complete sixel sequence.",
                                              .placeholder = "PATH" },
                                CLI::option { .name = "iterations",
                                              .v = CLI::value { 100u },
                                              .helpText = "How many times to feed the frame." },
                                CLI::option { .name = "columns",
                                              .v = CLI::value { 240u },
                                              .helpText = "Grid width in cells." },
                                CLI::option { .name = "lines",
                                              .v = CLI::value { 63u },
                                              .helpText = "Grid height in cells." },
                                CLI::option { .name = "cell-width",
                                              .v = CLI::value { 8u },
                                              .helpText = "Cell width in pixels." },
                                CLI::option { .name = "cell-height",
                                              .v = CLI::value { 17u },
                                              .helpText = "Cell height in pixels." },
                            } },
                }
        };
    }

    static int showMetaInfo()
    {
        // Show any interesting meta information.
        std::cout << std::format("CellProxy   : {} bytes\n", sizeof(vtbackend::CellProxy));
        std::cout << std::format("LineSoA     : {} bytes\n", sizeof(vtbackend::LineSoA));
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

    int benchSixel()
    {
        auto const path = parameters().get<std::string>("bench-headless.sixel.file");
        if (path.empty())
        {
            cerr << "No sixel file given. Use: bench-headless sixel file PATH\n"
                    "Produce one with termbench-pro, e.g.\n"
                    "  image-bench --protocol sixel --width 800 --height 600 --duration 0.05 > frame.six\n";
            return EXIT_FAILURE;
        }

        auto const data = readWholeFile(path);
        if (!data)
        {
            cerr << std::format("Cannot read '{}'.\n", path);
            return EXIT_FAILURE;
        }

        // A capture may hold several frames; one is enough and keeps the measurement per-frame.
        auto const begin = data->find("\033P");
        auto const end = data->find("\033\\", begin);
        if (begin == std::string::npos || end == std::string::npos)
        {
            cerr << std::format("'{}' holds no complete sixel sequence (DCS ... ST).\n", path);
            return EXIT_FAILURE;
        }
        auto const frame = data->substr(begin, (end + 2) - begin);

        auto const columns = parameters().get<unsigned>("bench-headless.sixel.columns");
        auto const lines = parameters().get<unsigned>("bench-headless.sixel.lines");
        auto const cellWidth = parameters().get<unsigned>("bench-headless.sixel.cell-width");
        auto const cellHeight = parameters().get<unsigned>("bench-headless.sixel.cell-height");
        auto const iterations = parameters().get<unsigned>("bench-headless.sixel.iterations");
        auto const cellSize =
            vtbackend::ImageSize { vtbackend::Width(cellWidth), vtbackend::Height(cellHeight) };
        auto const pageSize = vtbackend::PageSize { vtbackend::LineCount::cast_from(lines),
                                                    vtbackend::ColumnCount::cast_from(columns) };
        // What a display would set it to: the monitor the window sits on.
        auto const maxImageSize = vtbackend::ImageSize { vtbackend::Width(columns * cellWidth),
                                                         vtbackend::Height(lines * cellHeight) };

        return benchSixelStream(frame, iterations, pageSize, cellSize, maxImageSize);
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
} // namespace

int main(int argc, char const* argv[])
{
    srand(static_cast<unsigned int>(time(nullptr))); // initialize rand(). No strong seed required.

    ContourHeadlessBench app;
    return app.run(argc, argv);
}
