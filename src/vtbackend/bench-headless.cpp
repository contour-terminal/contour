// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Logging.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>

#include <vtparser/ParserEvents.hpp>

#include <vtpty/MockViewPty.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Pty.hpp>

#include <crispy/BufferObject.hpp>

#include <core/Environment.hpp>
#include <core/Utils.hpp>
#include <core/cli/App.hpp>
#include <core/cli/CLI.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <libtermbench/termbench.h>
#include <tracy/Tracy.hpp>

using namespace std;
using namespace std::string_literals;

namespace
{

std::string createText(size_t bytes)
{
    auto entropy = std::random_device {};
    auto engine = std::mt19937 { entropy() };
    auto letters = std::uniform_int_distribution<int> { 0, 25 };

    std::string text;
    while (text.size() < bytes)
    {
        text += static_cast<char>('A' + letters(engine));
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
    for ([[maybe_unused]] auto const iteration: core::times(iterations))
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

namespace
{

/// What the application on the other end of the PTY does while key presses are measured.
enum class ApplicationLoad : uint8_t
{
    Idle,  ///< Reads its input and nothing else, like an editor waiting for keys.
    Flood, ///< Also floods the terminal with log-like output, like `tail -f` or a build.
};

/// An application a load stands for: the script that plays it, and how the report names it.
struct Application
{
    std::string_view script {}; ///< Run by `/bin/sh -c`, with the log file as `$0`.
    std::string_view description {};
};

/// @return The application that puts @p load on the terminal.
///
/// Either one puts its PTY into raw mode, so that keys reach it unbuffered and unechoed, and reads them
/// as an editor would. Flooding, it reads them in the background while writing the log over and over,
/// as fast as the terminal takes it. A non-interactive shell points a background job's stdin at
/// /dev/null before applying the job's own redirections -- so `<&0` would not do, as dash shows -- hence
/// the terminal is kept on descriptor 3 first. Each `cat` writes several copies, so that starting the
/// next one rarely pauses the flood.
constexpr Application applicationFor(ApplicationLoad load)
{
    switch (load)
    {
        case ApplicationLoad::Idle:
            return { .script = "stty raw -echo; exec cat >/dev/null", .description = "idle" };
        case ApplicationLoad::Flood:
            return { .script =
                         "stty raw -echo; exec 3<&0; cat <&3 >/dev/null & "
                         "while :; do cat \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\"; done",
                     .description = "floods log output" };
    }
    std::unreachable();
}

/// How the key press benchmark runs. Its defaults are the command line's.
struct KeyPressBenchOptions
{
    ApplicationLoad load = ApplicationLoad::Flood;
    std::chrono::seconds duration { 10 };
    unsigned keyPressesPerSecond = 1000;
    unsigned ptyReadSize = 16384; ///< What contour reads per PTY read by default.
    unsigned queryEvery = 0;      ///< Lines between DA1 queries in the flood; 0 sends none.
};

/// The application's process, counting the bytes read from its PTY: the output the terminal parsed.
class ByteCountingProcess final: public vtpty::Process
{
  public:
    using vtpty::Process::Process;

    /// @return How many bytes have been read so far.
    [[nodiscard]] uint64_t bytesRead() const noexcept { return _bytesRead.load(std::memory_order_relaxed); }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto result = Process::read(storage, timeout, size);
        if (result)
            _bytesRead.fetch_add(result->data.size(), std::memory_order_relaxed);
        return result;
    }

  private:
    std::atomic<uint64_t> _bytesRead = 0;
};

/// Creates log-like output: a dimmed timestamp, a coloured level and a message on every line.
/// @param bytes      How much to create, at least.
/// @param queryEvery Lines between DA1 queries, each of which makes the terminal reply; 0 adds none.
/// @return The text, ending its lines in CRLF as an application on a raw-mode PTY has to.
std::string createLogText(size_t bytes, unsigned queryEvery)
{
    static constexpr auto Levels = std::array {
        "\033[32mINFO \033[m"sv, "\033[33mWARN \033[m"sv, "\033[1;31mERROR\033[m"sv, "\033[36mDEBUG\033[m"sv
    };

    auto text = std::string {};
    auto line = 0u;
    while (text.size() < bytes)
    {
        if (queryEvery != 0 && line % queryEvery == 0)
            text += "\033[c";
        text += std::format("\033[2m12:{:02}:{:02}.{:03}\033[m {} vtbackend: request {} served in {} us, "
                            "queue depth {}, cache hit ratio 0.{:03}\r\n",
                            (line / 60) % 60,
                            line % 60,
                            line % 1000,
                            Levels.at(line % Levels.size()),
                            line,
                            (line * 7919) % 5000,
                            line % 17,
                            (line * 31) % 1000);
        ++line;
    }
    return text;
}

/// @return The @p p quantile of @p sorted (ascending, not empty), by the nearest-rank method.
std::chrono::nanoseconds quantile(std::span<std::chrono::nanoseconds const> sorted, double p)
{
    auto const rank = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::clamp<size_t>(rank, 1, sorted.size()) - 1];
}

/// What a key press run measured.
struct KeyPressResult
{
    std::vector<std::chrono::nanoseconds> latencies {}; ///< One per key press, in the order pressed.
    std::chrono::duration<double> elapsed {};
    uint64_t bytesParsed = 0;
};

/// Presses a key at a steady rate, timing each press from sendCharEvent() until its bytes reached the PTY.
///
/// The time is the press's own, as the GUI thread spends it, including whatever the press writes along
/// with its key -- a reply the parser queued since the last write, say. A press that falls behind is not
/// made up for: the schedule resumes from it, so that a stall shows as one long press rather than as a
/// burst of short ones after it, and the load stays what was asked for.
/// @param terminal The terminal to press keys into, with its read loop running on another thread.
/// @param options  How many keys to press, and how fast.
/// @return One latency per key press.
std::vector<std::chrono::nanoseconds> pressKeys(vtbackend::Terminal& terminal,
                                                KeyPressBenchOptions const& options)
{
    using std::chrono::steady_clock;

    auto const interval = std::chrono::duration_cast<steady_clock::duration>(std::chrono::seconds(1))
                          / options.keyPressesPerSecond;
    auto const keyIdentity = vtbackend::KeyIdentity { .unshiftedKey = U'a', .nativeVirtualKey = 'a' };
    auto latencies = std::vector<std::chrono::nanoseconds> {};
    latencies.reserve(static_cast<size_t>(options.duration.count()) * options.keyPressesPerSecond);

    auto const end = steady_clock::now() + options.duration;
    auto next = steady_clock::now() + interval;
    while (next < end)
    {
        std::this_thread::sleep_until(next);
        auto const pressed = steady_clock::now();
        {
            ZoneScopedN("bench.keyPress");
            std::ignore = terminal.sendCharEvent(U'a',
                                                 keyIdentity,
                                                 vtbackend::KeyboardModifiers {},
                                                 vtbackend::KeyboardEventType::Press,
                                                 pressed);
        }
        auto const written = steady_clock::now();
        latencies.push_back(written - pressed);
        next = std::max(next + interval, written);
    }
    return latencies;
}

/// Prints the latency distribution of a key press run, and the output parsed meanwhile.
/// @param options     How the run was configured.
/// @param application What the application did.
/// @param result      What the run measured; its latencies are sorted in place.
void printKeyPressReport(KeyPressBenchOptions const& options, Application application, KeyPressResult result)
{
    auto& latencies = result.latencies;
    std::ranges::sort(latencies);
    auto const micros = [](std::chrono::nanoseconds t) {
        return std::chrono::duration<double, std::micro>(t).count();
    };
    auto const total = std::accumulate(latencies.begin(), latencies.end(), std::chrono::nanoseconds {});
    auto const mebibytes = static_cast<double>(result.bytesParsed) / (1024.0 * 1024.0);

    cout << std::format(
        "Key press latency\n"
        "-----------------\n"
        "  application   : {}\n"
        "  key presses   : {} at {} Hz over {:.1f} s\n"
        "  PTY read size : {} bytes\n"
        "  output parsed : {:.1f} MiB ({:.1f} MiB/s)\n"
        "  latency (us)  : mean {:.1f}, p50 {:.1f}, p90 {:.1f}, p99 {:.1f}, p99.9 {:.1f}, "
        "max {:.1f}\n",
        application.description,
        latencies.size(),
        options.keyPressesPerSecond,
        result.elapsed.count(),
        core::nextPowerOfTwo(static_cast<size_t>(options.ptyReadSize)), // as Terminal rounds it
        mebibytes,
        mebibytes / result.elapsed.count(),
        micros(total / latencies.size()),
        micros(quantile(latencies, 0.50)),
        micros(quantile(latencies, 0.90)),
        micros(quantile(latencies, 0.99)),
        micros(quantile(latencies, 0.999)),
        micros(latencies.back()));
}

} // namespace

/// Measures how long a key press takes to reach the PTY -- the GUI thread's part of typing -- while an
/// application on a real PTY reads the keys and, unless idle, floods the terminal with output.
///
/// The terminal is driven the way contour drives it: one thread runs the PTY read loop, parsing each
/// read under the terminal's state lock, while another presses keys. What a key press waits for on its
/// way to the PTY -- a lock the parser holds, the write itself -- is what this measures. With Tracy
/// built in, each key press is the zone `bench.keyPress`.
///
/// @param options What the application does, and how the keys are pressed.
/// @param env     The environment the terminal reads.
/// @return EXIT_SUCCESS, or EXIT_FAILURE when the application cannot be started or does not last.
static int benchKeyPressLatency(KeyPressBenchOptions const& options, core::Environment const& env)
{
#ifdef _WIN32
    (void) options;
    (void) env;
    cerr << "The keypress benchmark runs its application in a POSIX shell, which Windows lacks.\n";
    return EXIT_FAILURE;
#else
    auto constexpr LogSize = size_t { 4 } * 1024 * 1024; // what the application writes over and over
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(40), vtbackend::ColumnCount(120) };
    auto const application = applicationFor(options.load);

    auto const logFile = std::filesystem::temp_directory_path()
                         / std::format("contour-bench-keypress-{}.log", std::random_device {}());
    auto removeLogFile = core::Finally { [&]() {
        auto ignored = std::error_code {};
        std::filesystem::remove(logFile, ignored);
    } };
    if (!(std::ofstream(logFile, std::ios::binary) << createLogText(LogSize, options.queryEvery)))
    {
        cerr << std::format("Cannot write the application's output to '{}'.\n", logFile.string());
        return EXIT_FAILURE;
    }

    auto ownedProcess = std::make_unique<ByteCountingProcess>(
        vtpty::Process::ExecInfo { .program = "/bin/sh",
                                   .arguments = { "-c", std::string(application.script), logFile.string() },
                                   .workingDirectory = std::filesystem::temp_directory_path(),
                                   .env = {},
                                   .removedEnvironment = {} },
        vtpty::createPty(pageSize, std::nullopt),
        /*escapeSandbox=*/false,
        // a benchmark measures the terminal, not systemd: no scope, and no placement latency
        std::make_shared<vtpty::NoPlacement>());
    auto& process = *ownedProcess;

    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    settings.ptyReadBufferSize = options.ptyReadSize;
    auto events = vtbackend::Terminal::NullEvents {};
    auto terminal = vtbackend::Terminal {
        events, env, std::move(ownedProcess), settings, std::chrono::steady_clock::now()
    };

    if (auto const started = terminal.device().start(); !started)
    {
        cerr << std::format("Cannot start the application: {}\n", started.error().detail);
        return EXIT_FAILURE;
    }

    // Runs until the PTY reports the application gone, as contour's own read loop does.
    auto parser = std::thread { [&]() {
        while (terminal.processInputOnce())
        {
        }
    } };

    // Hang up, then keep reading until the application has gone: a process cannot finish exiting while
    // output it wrote to the terminal is still waiting to be read, so stopping the reader first would
    // leave it, and ~Process() waiting on it, stuck for good.
    auto stopApplication = core::Finally { [&]() {
        process.terminate(vtpty::Process::TerminationHint::Hangup);
        parser.join();
    } };

    // Let the application settle into its steady state before measuring.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    auto const bytesBefore = process.bytesRead();
    auto const start = std::chrono::steady_clock::now();
    auto latencies = pressKeys(terminal, options);
    auto result = KeyPressResult { .latencies = std::move(latencies),
                                   .elapsed = std::chrono::steady_clock::now() - start,
                                   .bytesParsed = process.bytesRead() - bytesBefore };

    // A key press that could not be written stays queued, and one written to an application that has
    // gone is dropped; either would leave fast latencies standing for keys nobody read. Replies may
    // still be queued too, so flush once more before asking.
    terminal.flushInput();
    auto const stillPending = terminal.hasInput();
    auto const applicationLasted = process.alive();

    stopApplication.run();

    if (!applicationLasted)
    {
        cerr << "The application exited during the measurement, so its key presses went nowhere.\n";
        return EXIT_FAILURE;
    }

    printKeyPressReport(options, application, std::move(result));
    if (stillPending)
        cout << "  note          : input was still pending when the measurement ended\n";

    return EXIT_SUCCESS;
#endif
}

namespace CLI = core::cli;

namespace
{
class ContourHeadlessBench: public core::cli::App
{
  public:
    /// @param env The process environment every part of the benchmark reads through.
    explicit ContourHeadlessBench(core::Environment const& env):
        App(env, "bench-headless", "Contour Headless Benchmark", CONTOUR_VERSION_STRING, "Apache-2.0")
    {
        using Project = core::cli::about::Project;
        core::cli::about::registerProjects(
#ifdef CONTOUR_BUILD_WITH_MIMALLOC
            Project { "mimalloc", "", "" },
#endif
            Project { "yaml-cpp", "MIT", "https://github.com/jbeder/yaml-cpp" },
            Project { "termbench-pro", "Apache-2.0", "https://github.com/contour-terminal/termbench-pro" });
        link("bench-headless.parser", bind(&ContourHeadlessBench::benchParserOnly, this));
        link("bench-headless.grid", bind(&ContourHeadlessBench::benchGrid, this));
        link("bench-headless.sixel", bind(&ContourHeadlessBench::benchSixel, this));
        link("bench-headless.pty", bind(&ContourHeadlessBench::benchPTY));
        link("bench-headless.keypress", bind(&ContourHeadlessBench::benchKeyPress, this));
        link("bench-headless.meta", bind(&ContourHeadlessBench::showMetaInfo));

        if (auto const logFilterString = env.get("LOG"))
        {
            core::log::configure(*logFilterString);
            core::cli::App::customizeLogStoreOutput();
        }
    }

    [[nodiscard]] core::cli::Command parameterDefinition() const override
    {
        auto constexpr KeyPressDefaults = KeyPressBenchOptions {};

        auto const perfOptions = CLI::OptionList {
            CLI::Option { .name = "size",
                          .v = CLI::Value { 32u },
                          .helpText = "Number of megabyte to process per test.",
                          .placeholder = "MB" },
            CLI::Option { .name = "cat",
                          .v = CLI::Value { false },
                          .helpText = "Enable cat-style short-line ASCII stream test." },
            CLI::Option { .name = "long",
                          .v = CLI::Value { false },
                          .helpText = "Enable long-line ASCII stream test." },
            CLI::Option { .name = "sgr", .v = CLI::Value { false }, .helpText = "Enable SGR stream test." },
            CLI::Option {
                .name = "binary", .v = CLI::Value { false }, .helpText = "Enable binary stream test." },
        };

        return CLI::Command {
            .name = "bench-headless",
            .helpText = "Contour Terminal Emulator " CONTOUR_VERSION_STRING
                        " - https://github.com/contour-terminal/contour/ ;-)",
            .options = CLI::OptionList {},
            .children =
                CLI::CommandList {
                    CLI::Command { .name = "help", .helpText = "Shows this help and exits." },
                    CLI::Command { .name = "meta",
                                   .helpText = "Shows some terminal backend meta information and exits." },
                    CLI::Command { .name = "version", .helpText = "Shows the version and exits." },
                    CLI::Command {
                        .name = "license",
                        .helpText = "Shows the license, and project URL of the used projects and Contour." },
                    CLI::Command {
                        .name = "grid",
                        .helpText = "Performs performance tests utilizing the full grid including VT parser.",
                        .options = perfOptions },
                    CLI::Command { .name = "parser",
                                   .helpText = "Performs performance tests utilizing the VT parser only.",
                                   .options = perfOptions },
                    CLI::Command { .name = "pty",
                                   .helpText = "Performs performance tests utilizing the underlying "
                                               "operating system's PTY only." },
                    CLI::Command {
                        .name = "keypress",
                        .helpText = "Measures how long a key press takes to reach the PTY while an "
                                    "application on a real PTY reads the keys and floods the terminal with "
                                    "output.",
                        .options =
                            CLI::OptionList {
                                CLI::Option { .name = "seconds",
                                              .v = CLI::Value { static_cast<unsigned>(
                                                  KeyPressDefaults.duration.count()) },
                                              .helpText = "How long to press keys for." },
                                CLI::Option { .name = "rate",
                                              .v = CLI::Value { KeyPressDefaults.keyPressesPerSecond },
                                              .helpText = "Key presses per second." },
                                CLI::Option { .name = "read-size",
                                              .v = CLI::Value { KeyPressDefaults.ptyReadSize },
                                              .helpText = "Bytes per PTY read, which is what the parser "
                                                          "takes per lock; the default is contour's.",
                                              .placeholder = "BYTES" },
                                CLI::Option { .name = "query-every",
                                              .v = CLI::Value { KeyPressDefaults.queryEvery },
                                              .helpText = "Lines between DA1 queries in the flood, each of "
                                                          "which makes the terminal reply; 0 sends none.",
                                              .placeholder = "LINES" },
                                CLI::Option {
                                    .name = "idle",
                                    .v = CLI::Value { KeyPressDefaults.load == ApplicationLoad::Idle },
                                    .helpText = "The application only reads the keys and writes "
                                                "no output." },
                            } },
                    CLI::Command {
                        .name = "sixel",
                        .helpText = "Measures sixel decode throughput: VT parse, sixel decode and "
                                    "placement into the grid, with no rendering.",
                        .options =
                            CLI::OptionList {
                                CLI::Option { .name = "file",
                                              .v = CLI::Value { ""s },
                                              .helpText = "File holding one complete sixel sequence.",
                                              .placeholder = "PATH" },
                                CLI::Option { .name = "iterations",
                                              .v = CLI::Value { 100u },
                                              .helpText = "How many times to feed the frame." },
                                CLI::Option { .name = "columns",
                                              .v = CLI::Value { 240u },
                                              .helpText = "Grid width in cells." },
                                CLI::Option { .name = "lines",
                                              .v = CLI::Value { 63u },
                                              .helpText = "Grid height in cells." },
                                CLI::Option { .name = "cell-width",
                                              .v = CLI::Value { 8u },
                                              .helpText = "Cell width in pixels." },
                                CLI::Option { .name = "cell-height",
                                              .v = CLI::Value { 17u },
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

    int benchKeyPress()
    {
        auto const options = KeyPressBenchOptions {
            .load = parameters().boolean("bench-headless.keypress.idle") ? ApplicationLoad::Idle
                                                                         : ApplicationLoad::Flood,
            .duration = std::chrono::seconds(parameters().uint("bench-headless.keypress.seconds")),
            .keyPressesPerSecond = parameters().uint("bench-headless.keypress.rate"),
            .ptyReadSize = parameters().uint("bench-headless.keypress.read-size"),
            .queryEvery = parameters().uint("bench-headless.keypress.query-every"),
        };
        // Bounded so that the interval between presses stays well above a key press's own cost, and a
        // whole run's samples fit in memory (at most 60 million, 480 MB).
        if (options.duration.count() < 1 || options.duration.count() > 600 || options.keyPressesPerSecond < 1
            || options.keyPressesPerSecond > 100'000 || options.ptyReadSize < 1)
        {
            cerr << "seconds must be within 1..600, rate within 1..100000, and read-size at least 1.\n";
            return EXIT_FAILURE;
        }
        return benchKeyPressLatency(options, processEnvironment());
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

        auto bufferObjectPool = crispy::BufferObjectPool<char>(4llu * 1024 * 1024);
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
        auto cleanupReader = core::Finally { [&]() {
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
        std::cout << std::format("Data transferred       : {}\n", core::humanReadableBytes(bytesTransferred));
        std::cout << std::format("Reader loop iterations : {}\n", loopIterations);
        std::cout << std::format(
            "Average size per read  : {}\n",
            core::humanReadableBytes(static_cast<uint64_t>(static_cast<long double>(bytesTransferred)
                                                           / static_cast<long double>(loopIterations))));
        std::cout << std::format("Transfer speed         : {} per second\n",
                                 core::humanReadableBytes(static_cast<uint64_t>(mbPerSecs)));

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
    ContourHeadlessBench app { core::defaultEnvironment() };
    return app.run(argc, argv);
}
