// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Logging.hpp>
#include <vtbackend/screen/Terminal.hpp>
#include <vtbackend/testing/MockTerm.hpp>
#include <vtbackend/testing/Workloads.hpp>

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
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <ranges>
#include <span>
#include <stop_token>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <libtermbench/termbench.h>
#include <tracy/Tracy.hpp>

#ifdef _WIN32
    #include <Windows.h>
#endif

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

/// What contour reads per PTY read by default, as Config's `read_buffer_size` does. The throughput and key
/// press benchmarks start from it, so that their numbers are contour's.
constexpr auto ContourPtyReadSize = static_cast<unsigned>(vtbackend::DefaultPtyReadBufferSize);

/// What the application on the other end of a benchmark's PTY does with the file it is given.
enum class ChildMode : uint8_t
{
    Write, ///< Writes the file once, unchanged, and exits: what the pty throughput benchmark measures.
    Idle,  ///< Reads its input and nothing else, like an editor waiting for keys.
    Flood, ///< Also floods the terminal with the file over and over, like `tail -f` or a build.
};

/// An application a PTY benchmark runs: how each launcher plays it, and how the report names it.
struct ChildApplication
{
    ChildMode mode;               ///< What it does, which is also where its row sits in ChildApplications.
    std::string_view name;        ///< How `pty-child mode` names it.
    std::string_view posixScript; ///< What plays it under `/bin/sh -c`, with the file as `$0`.
    std::string_view description; ///< How the report names it.
};

/// Every application the PTY benchmarks run, one row per ChildMode, in its order.
///
/// Each script puts its PTY into raw mode first, so that the line discipline passes every byte through
/// unchanged -- no CRLF translation -- and keys reach it unbuffered and unechoed. Should stty fail, the
/// writer writes nothing, and its byte count reports it. Idle, it reads the keys as an editor would.
/// Flooding, it reads them in the background while writing the file over and over, as fast as the
/// terminal takes it. A non-interactive shell points a background job's stdin at /dev/null before applying
/// the job's own redirections -- so `<&0` would not do, as dash shows -- hence the terminal is kept on
/// descriptor 3 first. Each `cat` writes several copies, so that starting the next one rarely pauses the
/// flood.
constexpr auto ChildApplications = std::array {
    ChildApplication { .mode = ChildMode::Write,
                       .name = "write",
                       .posixScript = "stty raw -echo && exec cat \"$0\"",
                       .description = "writes the file once" },
    ChildApplication { .mode = ChildMode::Idle,
                       .name = "idle",
                       .posixScript = "stty raw -echo; exec cat >/dev/null",
                       .description = "idle" },
    ChildApplication { .mode = ChildMode::Flood,
                       .name = "flood",
                       .posixScript =
                           "stty raw -echo; exec 3<&0; cat <&3 >/dev/null & "
                           "while :; do cat \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\" \"$0\"; done",
                       .description = "floods log output" },
};

static_assert(std::ranges::all_of(std::views::iota(size_t { 0 }, ChildApplications.size()),
                                  [](size_t index) {
                                      return ChildApplications[index].mode == static_cast<ChildMode>(index);
                                  }),
              "ChildApplications holds the rows in ChildMode's order, so that a mode's row is at its value");

/// @return The row of ChildApplications that plays @p mode. An enumerator added without a row throws here,
///         as the programmer error it is, rather than reading past the table.
[[nodiscard]] constexpr ChildApplication const& childApplication(ChildMode mode)
{
    return ChildApplications.at(static_cast<size_t>(mode));
}

/// The platforms whose PTY the benchmarks know.
enum class Host : uint8_t
{
    Posix,   ///< A kernel PTY with a line discipline.
    Windows, ///< ConPTY, a pseudo console that conhost hosts.
};

/// The platform this program is built for.
#ifdef _WIN32
constexpr auto ThisHost = Host::Windows;
#else
constexpr auto ThisHost = Host::Posix;
#endif

/// How a platform starts the application on the other end of a benchmark's PTY.
enum class ChildLauncher : uint8_t
{
    PosixShell,  ///< `/bin/sh -c <script> <file>`, the script playing the application.
    ThisProgram, ///< This program's own `pty-child mode <mode> file <file>`.
};

/// What a platform's PTY delivers to the terminal, next to what the application wrote.
enum class PtyOutput : uint8_t
{
    /// The very bytes: a raw-mode line discipline passes them through, so as many arrive as were written,
    /// and any other count is a failure.
    Unchanged,
    /// The output of a console host in between, which interprets what the application wrote and writes
    /// sequences of its own: only nothing arriving, or the writer failing, is a failure, and the report says
    /// how much arrived.
    Reencoded,
};

/// Whether this process can write output into a platform's PTY, as an application on its other end would.
enum class PtySlaveEnd : uint8_t
{
    Absent,   ///< There is no end to write output into, only the application's input.
    Writable, ///< The slave end, which the application's standard output is.
};

/// Whether something between the application and the terminal asks the terminal questions of its own.
enum class HostQueries : uint8_t
{
    None, ///< Nothing does: whatever the terminal reads, the application wrote.
    /// The console host asks for the terminal's attributes as it starts, and holds the application's output
    /// until they are answered, or for 3 s. Contour's GUI thread sends such a reply once the screen has
    /// changed. The throughput benchmark sends it after every read; the key press benchmark only while it
    /// lets the application settle, because its reading thread sending replies during the measurement would
    /// send key presses' bytes too, which the key presses must send themselves: @see answerHostQueries() and
    /// settle().
    AwaitedAtStart,
};

/// How a key press reaches the application through a platform's PTY.
enum class KeyEncoding : uint8_t
{
    Characters, ///< As the characters it types: `a` is the byte `a`.
    /// As a win32-input-mode record, which conhost asks the terminal for as it starts: `a` is some 18 bytes
    /// that name its virtual key code and its character.
    Win32InputRecords,
};

/// Whether a pty throughput run's time keeps the writer's start-up and exit, which are not the terminal's
/// work.
enum class WriterStartup : uint8_t
{
    /// Kept: the clock starts at the first byte read, which the writer wrote once it ran, and stops at the
    /// read that reports it gone, so that only its exit is in it, a sliver of the run.
    Included,
    /// Subtracted: the console host writes before the writer has even started up, so that the first byte read
    /// starts the clock early, and the writer's start-up, the host's queries and the writer's exit come to
    /// some 3% of a 16 MiB ascii run. Each run is taken less the fastest of several untimed runs of an empty
    /// file: @see measureWriterStartup().
    Subtracted,
};

/// What a real PTY is on a platform, and how the PTY benchmarks run their application through it.
struct PtyPlatform
{
    Host host;                   ///< Which platform this row describes.
    std::string_view device;     ///< How a throughput report describes a real PTY: @see describe().
    ChildLauncher launcher;      ///< How a benchmark starts the application on the PTY's other end.
    PtyOutput output;            ///< What reaches the terminal of what the application writes.
    PtySlaveEnd slaveEnd;        ///< Whether `bench-headless pty` can write into the PTY itself.
    HostQueries hostQueries;     ///< Whether a console host awaits the terminal's answers.
    WriterStartup writerStartup; ///< Whether a pty throughput run keeps the writer's start-up and exit.
    KeyEncoding keyEncoding;     ///< How a key press reaches the application.
};

/// Every platform's PTY.
constexpr auto PtyPlatforms = std::array {
    PtyPlatform { .host = Host::Posix,
                  .device = "written through a real PTY",
                  .launcher = ChildLauncher::PosixShell,
                  .output = PtyOutput::Unchanged,
                  .slaveEnd = PtySlaveEnd::Writable,
                  .hostQueries = HostQueries::None,
                  .writerStartup = WriterStartup::Included,
                  .keyEncoding = KeyEncoding::Characters },
    PtyPlatform { .host = Host::Windows,
                  .device = "written through ConPTY, with conhost in between",
                  .launcher = ChildLauncher::ThisProgram,
                  .output = PtyOutput::Reencoded,
                  .slaveEnd = PtySlaveEnd::Absent,
                  .hostQueries = HostQueries::AwaitedAtStart,
                  .writerStartup = WriterStartup::Subtracted,
                  .keyEncoding = KeyEncoding::Win32InputRecords },
};

/// The PTY of the platform this program is built for.
constexpr auto ThisPty = *std::ranges::find(PtyPlatforms, ThisHost, &PtyPlatform::host);

/// @return The key the key press benchmark presses, `a`, with the virtual key code that @p encoding sends.
[[nodiscard]] constexpr vtbackend::KeyIdentity pressedKey(KeyEncoding encoding)
{
    switch (encoding)
    {
        // A character carries no virtual key code; this one is as it always was.
        case KeyEncoding::Characters: return { .unshiftedKey = U'a', .nativeVirtualKey = 'a' };
        case KeyEncoding::Win32InputRecords:
            return { .unshiftedKey = U'a', .nativeVirtualKey = 0x41 }; // VK_A
    }
    std::unreachable();
}

/// @return What the key press report says about how a key press reaches the application, if it says anything:
///         where it is not the character it types.
[[nodiscard]] constexpr std::string_view describe(KeyEncoding encoding) noexcept
{
    switch (encoding)
    {
        case KeyEncoding::Characters: return {};
        case KeyEncoding::Win32InputRecords:
            return "  key encoding  : win32-input-mode records, as conhost asks the terminal for\n";
    }
    std::unreachable();
}

#ifdef _WIN32

/// Sleeps until @p deadline, on a high-resolution waitable timer: std::this_thread::sleep_until() sleeps in
/// steps of the system timer here -- 15.6 ms by default, or whatever another process last asked for -- which
/// would make a key press schedule or a refresh rate whatever the machine's other processes make it.
void sleepUntil(std::chrono::steady_clock::time_point deadline)
{
    // One per thread, made by its first sleep and closed when the thread ends.
    thread_local auto const timer = std::unique_ptr<void, decltype(&CloseHandle)> {
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS),
        &CloseHandle
    };
    auto const remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero())
        return;
    auto dueTime = LARGE_INTEGER {};
    // Negative for a time relative to now, in units of 100 ns.
    dueTime.QuadPart =
        -std::chrono::duration_cast<std::chrono::duration<LONGLONG, std::ratio<1, 10'000'000>>>(remaining)
             .count();
    if (!timer || !SetWaitableTimer(timer.get(), &dueTime, 0, nullptr, nullptr, FALSE))
    {
        std::this_thread::sleep_until(deadline); // a Windows older than 10 1803 has no such timer
        return;
    }
    WaitForSingleObject(timer.get(), INFINITE);
}

/// @return Why the last Windows API call on this thread failed.
[[nodiscard]] std::string lastErrorText()
{
    return std::system_category().message(static_cast<int>(GetLastError()));
}

/// @return Where @p module was loaded from -- this program's executable for nullptr -- or why it cannot
///         be named.
[[nodiscard]] std::expected<std::filesystem::path, std::string> modulePath(HMODULE module)
{
    auto path = std::wstring(32'768, L'\0'); // the longest path Windows has, so that one call does
    auto const length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return std::unexpected(lastErrorText());
    path.resize(length);
    return std::filesystem::path { std::move(path) };
}

/// @return This program's executable, or why it cannot be named.
[[nodiscard]] std::expected<std::filesystem::path, std::string> thisExecutable()
{
    return modulePath(nullptr).transform_error(
        [](std::string const& why) { return std::format("Cannot name this program's executable: {}", why); });
}

/// @return @p path in UTF-8, for a message to show.
[[nodiscard]] std::string toUtf8(std::filesystem::path const& path)
{
    auto const utf8 = path.u8string();
    return { utf8.begin(), utf8.end() };
}

/// @return @p path in the ANSI code page, which the application's command line is built in (Process_win32
///         starts it with CreateProcessA); or why the path cannot go there: that code page lacks a character
///         of it.
[[nodiscard]] std::expected<std::string, std::string> narrowPath(std::filesystem::path const& path)
{
    auto const& wide = path.native();
    if (wide.empty())
        return std::string {};
    // A UTF-8 code page has every character, and takes neither the flag nor the question.
    auto const utf8CodePage = GetACP() == CP_UTF8;
    auto const flags = utf8CodePage ? DWORD { 0 } : DWORD { WC_NO_BEST_FIT_CHARS };
    auto lacksCharacter = BOOL { FALSE };
    auto const wideSize = static_cast<int>(wide.size());
    auto const size = WideCharToMultiByte(
        CP_ACP, flags, wide.data(), wideSize, nullptr, 0, nullptr, utf8CodePage ? nullptr : &lacksCharacter);
    if (size == 0 || lacksCharacter)
        return std::unexpected(
            std::format("Cannot pass '{}' to the application: its command line is built in "
                        "the ANSI code page, which lacks a character of that path.",
                        toUtf8(path)));
    auto narrow = std::string(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_ACP, flags, wide.data(), wideSize, narrow.data(), size, nullptr, nullptr);
    return narrow;
}

/// @return Which ConPTY a PTY made now runs on, as the throughput report names it: a conpty.dll, and where it
///         was found -- ConPty takes the one beside this program first, then one on PATH -- or Windows'
///         built-in ConPTY. They differ much: the built-in one re-renders what the application writes.
[[nodiscard]] std::string describePtyImplementation()
{
    // ConPty loads its implementation as it is constructed, and keeps it loaded.
    [[maybe_unused]] auto const pty = vtpty::createPty(
        vtpty::PageSize { .lines = vtpty::LineCount(24), .columns = vtpty::ColumnCount(80) }, std::nullopt);
    auto const module = GetModuleHandleW(L"conpty.dll");
    if (module == nullptr)
        return ", Windows' built-in ConPTY"s;
    return modulePath(module)
        .transform([](std::filesystem::path const& library) {
            return std::format(", conpty.dll from {}", toUtf8(library.parent_path()));
        })
        .value_or(", a conpty.dll from an unknown directory"s);
}

/// How much the application writes per write: what coreutils' cat writes, which the POSIX writer runs.
constexpr auto ChildWriteSize = size_t { 128 } * 1024;

/// A console device the application opens as one of its standard handles, and the mode it puts it in.
struct ConsoleMode
{
    DWORD standardHandle;  ///< As GetStdHandle() names it.
    wchar_t const* device; ///< The console device it is to be.
    DWORD mode;            ///< What SetConsoleMode() puts it in.
    std::string_view what; ///< How an error names it.
};

/// The console modes that are the raw, VT-processing equivalent of `stty raw -echo`.
constexpr auto RawConsoleModes = std::array {
    // Escape sequences interpreted, and a line feed only a line feed, as through a raw-mode line discipline.
    ConsoleMode { .standardHandle = STD_OUTPUT_HANDLE,
                  .device = L"CONOUT$",
                  .mode = ENABLE_PROCESSED_OUTPUT | ENABLE_WRAP_AT_EOL_OUTPUT
                          | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN,
                  .what = "output" },
    // No line input, no echo and no processed input: each key as it comes, as a VT sequence.
    ConsoleMode { .standardHandle = STD_INPUT_HANDLE,
                  .device = L"CONIN$",
                  .mode = ENABLE_VIRTUAL_TERMINAL_INPUT,
                  .what = "input" },
};

/// Makes the console's own input and output this process's standard input and output, in raw mode, with
/// UTF-8 output: @see RawConsoleModes. They are opened afresh, because the standard handles a process starts
/// with need not be its console's: a parent's redirected ones reach it even through a pseudo console.
/// Standard error stays where it was. Where the parent's is redirected, as under ctest, an error goes there
/// rather than into what the terminal reads; where it is a console, the error most likely reaches the pseudo
/// console, and the exit code is what tells the benchmark.
/// @return Nothing, or why it could not.
[[nodiscard]] std::expected<void, std::string> enterRawConsoleMode()
{
    if (!SetConsoleOutputCP(CP_UTF8))
        return std::unexpected(std::format("Cannot set the console's output to UTF-8: {}", lastErrorText()));
    for (auto const& console: RawConsoleModes)
    {
        auto const handle = CreateFileW(console.device,
                                        GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        0,
                                        nullptr);
        if (handle == INVALID_HANDLE_VALUE || !SetStdHandle(console.standardHandle, handle)
            || !SetConsoleMode(handle, console.mode))
            return std::unexpected(
                std::format("Cannot open the console's {} in raw mode: {}", console.what, lastErrorText()));
    }
    return {};
}

/// Writes all of @p bytes to @p output, as they are.
/// @return Nothing, or why it could not.
[[nodiscard]] std::expected<void, std::string> writeAll(HANDLE output, std::span<char const> bytes)
{
    while (!bytes.empty())
    {
        auto written = DWORD {};
        if (!WriteFile(output, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)
            || written == 0)
            return std::unexpected(std::format("Cannot write to the console: {}", lastErrorText()));
        bytes = bytes.subspan(written);
    }
    return {};
}

/// Copies @p file to standard output unchanged, as `cat` does: a chunk read, a chunk written.
/// @return Nothing, or why it could not.
[[nodiscard]] std::expected<void, std::string> copyToStandardOutput(std::filesystem::path const& file)
{
    auto input = std::ifstream(file, std::ios::binary);
    if (!input)
        return std::unexpected(std::format("Cannot read '{}'.", file.string()));
    auto const output = GetStdHandle(STD_OUTPUT_HANDLE);
    auto chunk = std::vector<char>(ChildWriteSize);
    while (input)
    {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        auto const bytesRead = static_cast<size_t>(input.gcount());
        if (auto const written = writeAll(output, std::span<char const>(chunk).first(bytesRead)); !written)
            return written;
    }
    if (input.bad())
        return std::unexpected(std::format("Cannot read all of '{}'.", file.string()));
    return {};
}

/// Reads standard input and discards it, until it ends: the `cat >/dev/null` of the POSIX scripts.
void discardStandardInput()
{
    auto const input = GetStdHandle(STD_INPUT_HANDLE);
    auto buffer = std::array<char, 4096> {};
    auto bytesRead = DWORD {};
    while (ReadFile(input, buffer.data(), static_cast<DWORD>(buffer.size()), &bytesRead, nullptr)
           && bytesRead != 0)
    {
    }
}

/// Plays the application that @p mode names on this program's console: @see ChildApplications.
/// @param mode What the application does.
/// @param file What it writes, where it writes.
/// @return The exit code.
[[nodiscard]] int runPtyChild(ChildMode mode, std::filesystem::path const& file)
{
    auto const played = enterRawConsoleMode().and_then([&]() -> std::expected<void, std::string> {
        switch (mode)
        {
            case ChildMode::Write: return copyToStandardOutput(file);
            case ChildMode::Idle: discardStandardInput(); return {};
            case ChildMode::Flood: {
                // Never joined: the flood ends when the application is killed, or when the console goes
                // and a write fails, and either way a reader blocked on the console must not hold it up.
                std::thread { discardStandardInput }.detach();
                auto flooded = std::expected<void, std::string> {};
                while (flooded)
                    flooded = copyToStandardOutput(file);
                return flooded;
            }
        }
        std::unreachable();
    });
    if (!played)
    {
        cerr << played.error() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

#else

/// Sleeps until @p deadline.
void sleepUntil(std::chrono::steady_clock::time_point deadline)
{
    std::this_thread::sleep_until(deadline);
}

/// @return Why this program does not name its own executable here: only ChildLauncher::ThisProgram asks.
[[nodiscard]] std::expected<std::filesystem::path, std::string> thisExecutable()
{
    return std::unexpected("Only a Windows build names its own executable."s);
}

/// @return @p path as a command line takes it.
[[nodiscard]] std::expected<std::string, std::string> narrowPath(std::filesystem::path const& path)
{
    return path.string();
}

/// @return Nothing to add to the report: a kernel PTY has no implementation to choose between.
[[nodiscard]] std::string describePtyImplementation()
{
    return {};
}

/// Refuses: here a POSIX shell plays the PTY benchmarks' applications, @see ChildApplications.
/// @return EXIT_FAILURE.
[[nodiscard]] int runPtyChild(ChildMode, std::filesystem::path const&)
{
    cerr << "pty-child plays the PTY benchmarks' application on Windows; here, a script run by /bin/sh "
            "does.\n";
    return EXIT_FAILURE;
}

#endif

/// Sends @p terminal's replies to what the console host asked, where one asks: @see HostQueries.
void answerHostQueries(vtbackend::Terminal& terminal)
{
    switch (ThisPty.hostQueries)
    {
        case HostQueries::None: return;
        case HostQueries::AwaitedAtStart: terminal.flushInput(); return;
    }
    std::unreachable();
}

/// Waits @p duration for the application on @p terminal's PTY to settle into its steady state, answering what
/// the console host asks meanwhile, where one asks: @see HostQueries. A key press would carry the answers
/// otherwise, and the application could not settle before the first one. There, it also answers the flood's
/// own queries (`query-every`) asked meanwhile, which elsewhere the first key press carries.
void settle(vtbackend::Terminal& terminal, std::chrono::milliseconds duration)
{
    auto const settled = std::chrono::steady_clock::now() + duration;
    switch (ThisPty.hostQueries)
    {
        case HostQueries::None: break;
        case HostQueries::AwaitedAtStart:
            while (std::chrono::steady_clock::now() < settled)
            {
                answerHostQueries(terminal);
                sleepUntil(
                    std::min(settled, std::chrono::steady_clock::now() + std::chrono::milliseconds(1)));
            }
            break;
    }
    sleepUntil(settled);
}

/// @return How this platform starts the application that @p mode names, with @p file as what it writes, in
///         the temporary directory; or why it cannot.
[[nodiscard]] std::expected<vtpty::Process::ExecInfo, std::string> launchFor(
    ChildMode mode, std::filesystem::path const& file)
{
    auto const& application = childApplication(mode);
    switch (ThisPty.launcher)
    {
        case ChildLauncher::PosixShell:
            return vtpty::Process::ExecInfo {
                .program = "/bin/sh",
                .arguments = { "-c", std::string(application.posixScript), file.string() },
                .workingDirectory = std::filesystem::temp_directory_path(),
                .env = {},
                .removedEnvironment = {},
            };
        case ChildLauncher::ThisProgram: {
            auto const program = thisExecutable().and_then(narrowPath);
            if (!program)
                return std::unexpected(program.error());
            // The file is in the temporary directory, so that the working directory below can be spelled
            // wherever the file can.
            auto const narrowFile = narrowPath(file);
            if (!narrowFile)
                return std::unexpected(narrowFile.error());
            return vtpty::Process::ExecInfo {
                // Quoted: the command line takes the program as it is given, and a path with a space in it
                // would name another program.
                .program = std::format("\"{}\"", *program),
                .arguments = { "pty-child", "mode", std::string(application.name), "file", *narrowFile },
                .workingDirectory = std::filesystem::temp_directory_path(),
                .env = {},
                // Its log would go to the console, and from there into what the terminal reads.
                .removedEnvironment = { "LOG" },
            };
        }
    }
    std::unreachable();
}

/// How the key press benchmark runs. Its defaults are the command line's.
struct KeyPressBenchOptions
{
    ChildMode load = ChildMode::Flood; ///< What the application does: idle, or flood.
    std::chrono::seconds duration { 10 };
    unsigned keyPressesPerSecond = 1000;
    unsigned ptyReadSize = ContourPtyReadSize; ///< What contour reads per PTY read by default.
    unsigned queryEvery = 0;                   ///< Lines between DA1 queries in the flood; 0 sends none.
};

/// The application's process, keeping count of what the terminal read from its PTY -- the output it parsed:
/// the bytes, the reads that delivered them, and when the first one arrived. Any thread may ask, while
/// another one reads.
class ByteCountingProcess final: public vtpty::Process
{
  public:
    using vtpty::Process::Process;

    /// @return How many bytes have been read so far.
    [[nodiscard]] uint64_t bytesRead() const noexcept { return _bytesRead.load(std::memory_order_relaxed); }

    /// @return How many reads have delivered bytes so far.
    [[nodiscard]] uint64_t reads() const noexcept { return _reads.load(std::memory_order_relaxed); }

    /// @return When the first byte was read, or nullopt while none has been.
    [[nodiscard]] std::optional<std::chrono::steady_clock::time_point> firstReadAt() const noexcept
    {
        if (!_hasFirstReadAt.load(std::memory_order_acquire))
            return std::nullopt;
        return _firstReadAt;
    }

    [[nodiscard]] std::optional<ReadResult> read(crispy::BufferObject<char>& storage,
                                                 std::optional<std::chrono::milliseconds> timeout,
                                                 size_t size) override
    {
        auto result = Process::read(storage, timeout, size);
        if (result && !result->data.empty())
        {
            if (!_hasFirstReadAt.load(std::memory_order_relaxed))
            {
                _firstReadAt = std::chrono::steady_clock::now();
                _hasFirstReadAt.store(true, std::memory_order_release); // publishes _firstReadAt
            }
            _bytesRead.fetch_add(result->data.size(), std::memory_order_relaxed);
            _reads.fetch_add(1, std::memory_order_relaxed);
        }
        return result;
    }

  private:
    std::atomic<uint64_t> _bytesRead = 0;
    std::atomic<uint64_t> _reads = 0;
    /// When the first byte was read. Written once, by the reading thread, before _hasFirstReadAt is set.
    std::chrono::steady_clock::time_point _firstReadAt {};
    std::atomic<bool> _hasFirstReadAt = false;
};

/// A file in the temporary directory, for an application to read; removed again when this goes.
class ScratchFile
{
  public:
    /// Writes @p content to a new file.
    /// @param purpose What the file is for, which its name says.
    /// @return The file, or why it could not be written.
    [[nodiscard]] static std::expected<ScratchFile, std::string> create(std::string_view purpose,
                                                                        std::string_view content)
    {
        auto file = ScratchFile { std::filesystem::temp_directory_path()
                                  / std::format("contour-bench-{}-{}", purpose, std::random_device {}()) };
        if (!(std::ofstream(file._path, std::ios::binary) << content))
            return std::unexpected(std::format("Cannot write '{}'.", file._path.string()));
        return file;
    }

    ScratchFile(ScratchFile&& other) noexcept: _path { std::exchange(other._path, {}) } {}
    ScratchFile(ScratchFile const&) = delete;
    ScratchFile& operator=(ScratchFile const&) = delete;
    ScratchFile& operator=(ScratchFile&&) = delete;

    ~ScratchFile()
    {
        if (_path.empty())
            return;
        auto ignored = std::error_code {};
        std::filesystem::remove(_path, ignored);
    }

    /// @return Where the file is.
    [[nodiscard]] std::filesystem::path const& path() const noexcept { return _path; }

  private:
    explicit ScratchFile(std::filesystem::path path): _path { std::move(path) } {}

    std::filesystem::path _path; ///< Empty once moved from.
};

/// A terminal on a real PTY with an application on its other end, as the key press and pty throughput
/// benchmarks run one. Constructing it builds both; start() runs the application.
class ChildSession
{
  public:
    /// @param launch   How to start the application: @see launchFor().
    /// @param settings The terminal's settings; its page size is the PTY's too.
    /// @param env      The environment the terminal reads.
    ChildSession(vtpty::Process::ExecInfo const& launch,
                 vtbackend::Settings const& settings,
                 core::Environment const& env):
        _terminal { _events,
                    env,
                    std::make_unique<ByteCountingProcess>(
                        launch,
                        vtpty::createPty(settings.pageSize, std::nullopt),
                        /*escapeSandbox=*/false,
                        // a benchmark measures the terminal, not systemd: no scope, and no placement latency
                        std::make_shared<vtpty::NoPlacement>()),
                    settings,
                    std::nullopt,
                    std::chrono::steady_clock::now() }
    {
    }

    /// Starts the application.
    /// @return Nothing, or why it could not be started.
    [[nodiscard]] std::expected<void, std::string> start()
    {
        auto const started = _terminal.device().start();
        if (!started)
            return std::unexpected(std::format("Cannot start the application: {}", started.error()));
        // Started otherwise than asked -- the login shell in its place, say -- it would not be what is
        // measured, and would not end by itself.
        if (!started->diagnostic.empty())
        {
            process().terminate(vtpty::Process::TerminationHint::Normal);
            return std::unexpected(
                std::format("Cannot start the application as asked: {}", started->diagnostic));
        }
        return {};
    }

    /// @return The terminal the application writes to.
    [[nodiscard]] vtbackend::Terminal& terminal() noexcept { return _terminal; }

    /// @return The application's process, and what has been read from it.
    [[nodiscard]] ByteCountingProcess& process() noexcept
    {
        return static_cast<ByteCountingProcess&>(_terminal.device());
    }

  private:
    vtbackend::Terminal::NullEvents _events;
    vtbackend::Terminal _terminal;
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
    auto const keyIdentity = pressedKey(ThisPty.keyEncoding);
    auto latencies = std::vector<std::chrono::nanoseconds> {};
    latencies.reserve(static_cast<size_t>(options.duration.count()) * options.keyPressesPerSecond);

    auto const end = steady_clock::now() + options.duration;
    auto next = steady_clock::now() + interval;
    while (next < end)
    {
        sleepUntil(next);
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
void printKeyPressReport(KeyPressBenchOptions const& options,
                         ChildApplication const& application,
                         KeyPressResult result)
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
    cout << describe(ThisPty.keyEncoding);
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
    auto constexpr LogSize = size_t { 4 } * 1024 * 1024; // what the application writes over and over
    auto const pageSize = vtbackend::PageSize { vtbackend::LineCount(40), vtbackend::ColumnCount(120) };

    auto const logFile = ScratchFile::create("keypress", createLogText(LogSize, options.queryEvery));
    if (!logFile)
    {
        cerr << logFile.error() << '\n';
        return EXIT_FAILURE;
    }
    auto const launch = launchFor(options.load, logFile->path());
    if (!launch)
    {
        cerr << launch.error() << '\n';
        return EXIT_FAILURE;
    }

    auto settings = vtbackend::Settings {};
    settings.pageSize = pageSize;
    settings.ptyReadBufferSize = options.ptyReadSize;
    auto session = ChildSession { *launch, settings, env };
    if (auto const started = session.start(); !started)
    {
        cerr << started.error() << '\n';
        return EXIT_FAILURE;
    }
    auto& terminal = session.terminal();
    auto& process = session.process();

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
    settle(terminal, std::chrono::milliseconds(500));

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

    printKeyPressReport(options, childApplication(options.load), std::move(result));
    if (stillPending)
        cout << "  note          : input was still pending when the measurement ended\n";

    return EXIT_SUCCESS;
}

namespace
{

/// The page every throughput and snapshot run uses: as tall as the TUI workload's frames, and wider than any
/// workload's lines.
constexpr auto BenchPageSize =
    vtbackend::PageSize { .lines = vtbackend::LineCount(vtbackend::test::WorkloadPageLines),
                          .columns = vtbackend::ColumnCount(vtbackend::test::WorkloadPageColumns) };

/// Scrollback for the runs: small next to the workloads, so that one untimed pass of a workload fills the
/// ring, and the timed pass after it recycles lines all the way, as a long session's scrolling does. Timing
/// the pass that fills it would mix in a cold path, and more of it the smaller the run.
constexpr auto BenchHistory = vtbackend::LineCount(4000);

/// A screen the throughput and snapshot benchmarks write to.
struct BenchScreen
{
    std::string_view name;          ///< How the command line names it.
    std::string_view entrySequence; ///< What switches a fresh terminal to it.
};

/// Every screen the benchmarks know, in the order they report them.
constexpr auto BenchScreens = std::array {
    BenchScreen { .name = "primary", .entrySequence = "" },
    BenchScreen { .name = "alternate", .entrySequence = "\033[?1049h" },
};

/// @return The length of the longest name in @p table: the width of a report column that names its rows.
template <typename Row, size_t N>
[[nodiscard]] constexpr size_t longestName(std::array<Row, N> const& table)
{
    return std::ranges::max(table | std::views::transform([](Row const& row) { return row.name.size(); }));
}

/// The widths of the workload and screen columns the throughput and snapshot reports align on.
constexpr auto WorkloadColumnWidth = longestName(vtbackend::test::Workloads);
constexpr auto ScreenColumnWidth = longestName(BenchScreens);

/// Finds the row of @p table that @p name names.
/// @param table The rows to search; each has a @c name.
/// @param name  The name to find.
/// @param what  What a row is called, for the error message.
/// @return The row, or a message naming the unknown name and every known one.
template <typename Row>
[[nodiscard]] std::expected<Row, std::string> findByName(std::span<Row const> table,
                                                         std::string_view name,
                                                         std::string_view what)
{
    auto const row = std::ranges::find(table, name, &Row::name);
    if (row == table.end())
        return std::unexpected(
            std::format("Unknown {} '{}'. Known: {}.",
                        what,
                        name,
                        core::joinHumanReadable(table | std::views::transform(&Row::name))));
    return *row;
}

/// Picks the rows of @p table that the comma-separated @p names name, in the order named.
/// @param table The rows to pick from; each has a @c name.
/// @param names The names to pick, comma-separated; all rows when empty.
/// @param what  What a row is called, for the error message.
/// @return The rows, or a message naming the unknown name and every known one.
template <typename Row>
[[nodiscard]] std::expected<std::vector<Row>, std::string> pickByName(std::span<Row const> table,
                                                                      std::string_view names,
                                                                      std::string_view what)
{
    if (names.empty())
        return std::vector<Row>(table.begin(), table.end());

    auto picked = std::vector<Row> {};
    for (auto const nameRange: std::views::split(names, ','))
    {
        auto row = findByName(table, std::string_view(nameRange.begin(), nameRange.end()), what);
        if (!row)
            return std::unexpected(std::move(row.error()));
        picked.push_back(*std::move(row));
    }
    return picked;
}

/// The workloads and screens one throughput or snapshot run covers, in the order it reports them.
struct BenchSelection
{
    std::vector<vtbackend::test::Workload> workloads; ///< What is written.
    std::vector<BenchScreen> screens;                 ///< Where each workload is written.
};

/// Runs @p measure @p runs times, and keeps the fastest run.
/// @param runs    How many runs; at least one.
/// @param measure One run. It returns what the run measured, the fastest run ordering least, or a
///                std::expected of that where a run can fail.
/// @return The least of what @p measure returned. A failed run ends the series, and its failure is returned.
template <typename Measure>
[[nodiscard]] auto fastestOf(unsigned runs, Measure const& measure) -> std::invoke_result_t<Measure const&>
{
    auto fastest = measure();
    for ([[maybe_unused]] auto const run: std::views::iota(1u, runs))
    {
        if constexpr (requires(decltype(fastest) const& result) { result.error(); })
        {
            if (!fastest)
                break;
            fastest = measure().transform([&](auto const& next) { return std::min(*fastest, next); });
        }
        else
        {
            fastest = std::min(fastest, measure());
        }
    }
    return fastest;
}

/// Writes @p text into @p vt's terminal through its mock PTY, outside any measurement.
void feed(vtbackend::MockTerm<vtpty::MockViewPty>& vt, std::string_view text)
{
    vt.mockPty().setReadData(text);
    while (!vt.mockPty().stdoutBuffer().empty())
        vt.terminal.processInputOnce();
}

/// Lets @p terminal take in everything @p pty holds, the way contour's PTY thread does: read, lock, parse.
///
/// Out of line so that callgrind can count exactly this, with `--toggle-collect='*runWorkloadOnce*'`.
/// @return How long it took.
[[gnu::noinline, nodiscard]] std::chrono::nanoseconds runWorkloadOnce(vtbackend::Terminal& terminal,
                                                                      vtpty::MockViewPty& pty)
{
    auto const start = std::chrono::steady_clock::now();
    while (!pty.stdoutBuffer().empty())
        terminal.processInputOnce();
    return std::chrono::steady_clock::now() - start;
}

/// Where a throughput run's bytes come from.
enum class ThroughputDevice : uint8_t
{
    MockPty, ///< Fed from memory: the terminal's own cost, with no system calls.
    RealPty, ///< Written by a child process through a real PTY, system calls and all.
};

/// @return How the report describes @p device.
[[nodiscard]] constexpr std::string_view describe(ThroughputDevice device) noexcept
{
    switch (device)
    {
        case ThroughputDevice::MockPty: return "fed from memory";
        case ThroughputDevice::RealPty: return ThisPty.device;
    }
    std::unreachable();
}

/// How the throughput benchmark runs. Its defaults are the command line's.
struct ThroughputOptions
{
    size_t bytesPerWorkload = size_t { 16 } * 1024 * 1024; ///< How much of each workload to generate.
    unsigned runs = 5;                         ///< Runs per workload and screen; the fastest counts.
    unsigned ptyReadSize = ContourPtyReadSize; ///< Bytes per PTY read, as the parser takes them.
    ThroughputDevice device = ThroughputDevice::MockPty; ///< Where each run's bytes come from.
    unsigned refreshHz = 0; ///< Render-buffer refreshes per second during a pty run; 0 for none.
};

/// What one throughput run measured. Runs order by how long they took first, so the fastest of several is the
/// least.
struct ThroughputRun
{
    std::chrono::nanoseconds elapsed {};     ///< How long the terminal took to take the workload in.
    std::optional<uint64_t> bytesPerRead {}; ///< What a PTY read delivered; counted through a real PTY only.
    /// How much of what was written arrived, as a share of it; counted only through a PTY that does not pass
    /// the bytes through unchanged, @see PtyOutput.
    std::optional<double> arrived {};

    auto operator<=>(ThroughputRun const&) const = default;
};

/// @return How long a terminal took to take in @p input on @p screen, fed from memory @p ptyReadSize
///         bytes per read, once an untimed pass of the same input had brought it to its steady state.
[[nodiscard]] ThroughputRun measureFromMemory(std::string_view input,
                                              BenchScreen const& screen,
                                              size_t ptyReadSize)
{
    auto vt = vtbackend::MockTerm<vtpty::MockViewPty>(BenchPageSize, BenchHistory, ptyReadSize);
    vt.terminal.setMode(vtbackend::DECMode::AutoWrap, true);
    feed(vt, screen.entrySequence);
    feed(vt, input); // fills the scrollback, @see BenchHistory
    vt.mockPty().setReadData(input);
    return ThroughputRun { .elapsed = runWorkloadOnce(vt.terminal, vt.mockPty()),
                           .bytesPerRead = std::nullopt,
                           .arrived = std::nullopt };
}

/// Refreshes @p terminal's render buffer @p hz times a second until stopped, the way the render thread
/// contends for the terminal's state while output floods in.
/// @return The refreshing thread, or no thread at all when @p hz is 0.
[[nodiscard]] std::jthread startRefresher(vtbackend::Terminal& terminal, unsigned hz)
{
    if (hz == 0)
        return {};
    return std::jthread { [&terminal, hz](std::stop_token const& stop) {
        auto const interval = std::chrono::microseconds { 1'000'000 / hz };
        auto next = std::chrono::steady_clock::now();
        while (!stop.stop_requested())
        {
            terminal.refreshRenderBuffer();
            next += interval;
            sleepUntil(next);
        }
    } };
}

/// Checks what came of a run through a PTY: what arrived, against what was written into it, and where the PTY
/// re-encodes, how the writer ended. There the byte count cannot tell a failed writer from a working one,
/// because the console host writes sequences of its own whatever the writer does.
/// @param output       What the PTY delivers.
/// @param bytesRead    What the terminal read.
/// @param bytesWritten What the writer wrote.
/// @param writerExit   How the writer ended, if it has.
/// @return How much arrived, as a share of what was written, where the report shows it; or why the run
///         failed.
[[nodiscard]] std::expected<std::optional<double>, std::string> checkRun(
    PtyOutput output,
    uint64_t bytesRead,
    uint64_t bytesWritten,
    std::optional<vtpty::Process::ExitStatus> const& writerExit)
{
    switch (output)
    {
        case PtyOutput::Unchanged:
            if (bytesRead != bytesWritten)
                return std::unexpected(
                    std::format("{} bytes arrived where {} were written.", bytesRead, bytesWritten));
            return std::optional<double> {};
        case PtyOutput::Reencoded: {
            if (!writerExit)
                return std::unexpected("The writer was still running when its PTY closed."s);
            auto const* const normalExit = std::get_if<vtpty::Process::NormalExit>(&*writerExit);
            if (normalExit == nullptr || normalExit->exitCode != 0)
                return std::unexpected(std::format("The writer exited with {}.", *writerExit));
            if (bytesRead == 0)
                return std::unexpected(
                    std::format("Nothing arrived where {} bytes were written.", bytesWritten));
            if (bytesWritten == 0)
                return std::optional<double> {};
            return std::optional { static_cast<double>(bytesRead) / static_cast<double>(bytesWritten) };
        }
    }
    std::unreachable();
}

/// @return How long a terminal took to take in @p input on @p screen when @p writer wrote it through a real
///         PTY, once an untimed pass of the same input had brought the terminal to its steady state: from the
///         first byte read to the read that reported the writer gone, including the writer's exit. Or why the
///         run could not happen.
[[nodiscard]] std::expected<ThroughputRun, std::string> measureThroughPty(
    std::string_view input,
    vtpty::Process::ExecInfo const& writer,
    BenchScreen const& screen,
    ThroughputOptions const& options,
    core::Environment const& env)
{
    auto settings = vtbackend::Settings {};
    settings.pageSize = BenchPageSize;
    settings.historyLimits = vtbackend::HistoryLimits::plain(BenchHistory);
    settings.ptyReadBufferSize = options.ptyReadSize;
    auto session = ChildSession { writer, settings, env };
    auto& terminal = session.terminal();
    terminal.setMode(vtbackend::DECMode::AutoWrap, true);
    // Straight to the screen, before the writer exists: fills the scrollback, @see BenchHistory.
    terminal.writeToScreen(screen.entrySequence);
    terminal.writeToScreen(input);

    if (auto const started = session.start(); !started)
        return std::unexpected(started.error());

    // Stopped and joined when it goes out of scope, before the terminal does.
    auto const refresher = startRefresher(terminal, options.refreshHz);

    // Runs until the PTY reports the writer gone, as contour's own read loop does.
    while (terminal.processInputOnce())
        answerHostQueries(terminal);
    auto const finished = std::chrono::steady_clock::now();

    // Where checkRun() asks how the writer ended (ConPTY), the answer is in: Process_win32's exit watcher
    // records the writer's exit before it closes the PTY, which is what ended the loop.
    auto const& process = session.process();
    auto const firstRead = process.firstReadAt();
    return checkRun(ThisPty.output, process.bytesRead(), input.size(), process.checkStatus())
        .and_then([&](std::optional<double> arrived) -> std::expected<ThroughputRun, std::string> {
            if (!firstRead) // an empty input through a PTY that adds nothing
                return std::unexpected("Nothing arrived."s);
            return ThroughputRun { .elapsed = finished - *firstRead,
                                   .bytesPerRead = process.bytesRead() / process.reads(),
                                   .arrived = arrived };
        });
}

/// The fewest empty runs whose fastest is subtracted, where a platform subtracts the writer's start-up and
/// exit: one alone is noisy, and as the process's first launch of the writer, often slow.
constexpr auto MinEmptyRuns = 3u;

/// What each pty throughput run is taken less, where the platform subtracts the writer's start-up and exit.
struct WriterStartupCost
{
    std::chrono::nanoseconds elapsed {}; ///< What the fastest empty run took.
    unsigned emptyRuns = 0;              ///< How many empty runs it was the fastest of.
};

/// @return What each pty throughput run is taken less, where the platform subtracts the writer's start-up and
///         exit: the fastest of at least MinEmptyRuns untimed runs of an empty file, @see WriterStartup. The
///         same estimate as the fastest of the measured runs, so that the difference is the terminal's work.
///         Nothing where the platform keeps them in, or where the runs are fed from memory. Or why an empty
///         run could not happen.
[[nodiscard]] std::expected<std::optional<WriterStartupCost>, std::string> measureWriterStartup(
    ThroughputOptions const& options, core::Environment const& env)
{
    if (options.device == ThroughputDevice::MockPty)
        return std::nullopt;
    switch (ThisPty.writerStartup)
    {
        case WriterStartup::Included: return std::nullopt;
        case WriterStartup::Subtracted: {
            auto const emptyRuns = std::max(options.runs, MinEmptyRuns);
            return ScratchFile::create("empty", "").and_then([&](ScratchFile const& file) {
                return launchFor(ChildMode::Write, file.path())
                    .and_then([&](vtpty::Process::ExecInfo const& writer) {
                        return fastestOf(emptyRuns, [&] {
                            return measureThroughPty("", writer, BenchScreens.front(), options, env);
                        });
                    })
                    .transform([&](ThroughputRun const& fastest) {
                        return std::optional { WriterStartupCost { .elapsed = fastest.elapsed,
                                                                   .emptyRuns = emptyRuns } };
                    });
            });
        }
    }
    std::unreachable();
}

/// @return @p run less @p writerStartup, if any; or why that leaves too little to measure.
[[nodiscard]] std::expected<ThroughputRun, std::string> lessWriterStartup(
    ThroughputRun run, std::optional<WriterStartupCost> const& writerStartup)
{
    if (!writerStartup)
        return run;
    if (run.elapsed <= writerStartup->elapsed)
        return std::unexpected(
            std::format("A run took {:.1f} ms, no longer than an empty one: too little to measure.",
                        std::chrono::duration<double, std::milli>(run.elapsed).count()));
    run.elapsed -= writerStartup->elapsed;
    return run;
}

/// @return The fastest of @p options.runs runs of @p input on @p screen, each less @p writerStartup, if
///         any; or why one could not run.
[[nodiscard]] std::expected<ThroughputRun, std::string> measureFastestThroughput(
    std::string_view input,
    BenchScreen const& screen,
    ThroughputOptions const& options,
    std::optional<WriterStartupCost> const& writerStartup,
    core::Environment const& env)
{
    switch (options.device)
    {
        case ThroughputDevice::MockPty:
            return fastestOf(options.runs,
                             [&] { return measureFromMemory(input, screen, options.ptyReadSize); });
        case ThroughputDevice::RealPty:
            // The writer reads the workload from a file, which every run reuses.
            return ScratchFile::create("throughput", input).and_then([&](ScratchFile const& file) {
                return launchFor(ChildMode::Write, file.path())
                    .and_then([&](vtpty::Process::ExecInfo const& writer) {
                        return fastestOf(options.runs, [&] {
                            return measureThroughPty(input, writer, screen, options, env)
                                .and_then([&](ThroughputRun const& run) {
                                    return lessWriterStartup(run, writerStartup);
                                });
                        });
                    });
            });
    }
    std::unreachable();
}

/// Runs every picked workload on every picked screen, and prints the fastest run of each.
int runThroughputBenchmark(std::span<vtbackend::test::Workload const> workloads,
                           std::span<BenchScreen const> screens,
                           ThroughputOptions const& options,
                           core::Environment const& env)
{
    auto const writerStartup = measureWriterStartup(options, env);
    if (!writerStartup)
    {
        cerr << writerStartup.error() << '\n';
        return EXIT_FAILURE;
    }
    cout << std::format(
        "Throughput: {} MiB per workload, fastest of {} runs, PTY reads of up to {} bytes, {}{}{}\n",
        options.bytesPerWorkload / (size_t { 1024 } * 1024),
        options.runs,
        core::nextPowerOfTwo(static_cast<size_t>(options.ptyReadSize)), // as Terminal rounds it
        describe(options.device),
        options.device == ThroughputDevice::RealPty ? describePtyImplementation() : std::string {},
        writerStartup
            ->transform([](WriterStartupCost const& startup) {
                return std::format(", each run less {:.1f} ms, the fastest of {} empty runs: the writer's "
                                   "start-up and exit",
                                   std::chrono::duration<double, std::milli>(startup.elapsed).count(),
                                   startup.emptyRuns);
            })
            .value_or(""));
    for (auto const& workload: workloads)
    {
        auto const input = vtbackend::test::makeWorkload(workload, options.bytesPerWorkload);
        auto const mebibytes = static_cast<double>(input.size()) / (1024.0 * 1024.0);
        for (auto const& screen: screens)
        {
            auto const fastest = measureFastestThroughput(input, screen, options, *writerStartup, env);
            if (!fastest)
            {
                cerr << fastest.error() << '\n';
                return EXIT_FAILURE;
            }
            auto row = std::format("{:<{}} {:<{}} {:9.1f} MiB/s",
                                   workload.name,
                                   WorkloadColumnWidth,
                                   screen.name,
                                   ScreenColumnWidth,
                                   mebibytes / std::chrono::duration<double>(fastest->elapsed).count());
            if (fastest->bytesPerRead)
                row += std::format(" {:9} bytes/read", *fastest->bytesPerRead);
            if (fastest->arrived)
                row += std::format(" {:6.1f}% arrived", 100.0 * *fastest->arrived);
            cout << row << '\n';
        }
    }
    return EXIT_SUCCESS;
}

/// A status line the snapshot benchmark renders with.
struct SnapshotStatusLine
{
    vtbackend::StatusDisplayType type; ///< What the terminal shows.
    std::string_view label;            ///< How the report names it.
    /// The rows it takes. The terminal gets them on top of BenchPageSize, so that every column renders the
    /// same main page, and the difference between two columns is the status line's own cost.
    vtbackend::LineCount height;
};

/// Every status line a snapshot run measures, in the order the report shows them.
constexpr auto SnapshotStatusLines = std::array {
    SnapshotStatusLine { .type = vtbackend::StatusDisplayType::None,
                         .label = "no status line",
                         .height = vtbackend::LineCount(0) },
    SnapshotStatusLine { .type = vtbackend::StatusDisplayType::Indicator,
                         .label = "indicator",
                         .height = vtbackend::LineCount(1) },
};

/// How much of a workload fills the terminal before frames are timed: the page and the scrollback, many
/// times over.
constexpr auto SnapshotFillBytes = size_t { 1 } * 1024 * 1024;

/// How the snapshot benchmark runs. Its defaults are the command line's.
struct SnapshotOptions
{
    unsigned frames = 2000; ///< Frames per run; the report shows their mean.
    unsigned runs = 3;      ///< Runs per workload, screen and status line; the fastest counts.
};

/// Renders @p frames frames the way the render thread does each one: refreshRenderBuffer(), which builds the
/// render buffer while it holds the terminal's state lock.
///
/// Out of line so that callgrind can count exactly this, with `--toggle-collect='*runSnapshotFrames*'`.
/// @return How long all the frames took.
[[gnu::noinline, nodiscard]] std::chrono::nanoseconds runSnapshotFrames(vtbackend::Terminal& terminal,
                                                                        unsigned frames)
{
    auto const start = std::chrono::steady_clock::now();
    for ([[maybe_unused]] auto const frame: std::views::iota(0u, frames))
        std::ignore = terminal.refreshRenderBuffer();
    return std::chrono::steady_clock::now() - start;
}

/// @return The mean time of one frame, once @p fill has been written on @p screen with @p statusLine shown.
[[nodiscard]] std::chrono::duration<double, std::micro> measureSnapshot(std::string_view fill,
                                                                        BenchScreen const& screen,
                                                                        SnapshotStatusLine const& statusLine,
                                                                        unsigned frames)
{
    auto const totalPageSize = vtbackend::PageSize { .lines = BenchPageSize.lines + statusLine.height,
                                                     .columns = BenchPageSize.columns };
    auto vt = vtbackend::MockTerm<vtpty::MockViewPty>(totalPageSize, BenchHistory, ContourPtyReadSize);
    vt.terminal.setMode(vtbackend::DECMode::AutoWrap, true);
    vt.terminal.setStatusDisplay(statusLine.type);
    assert(vt.terminal.pageSize() == BenchPageSize && "a status line's height is not the terminal's");
    feed(vt, screen.entrySequence);
    feed(vt, fill);
    return std::chrono::duration<double, std::micro>(runSnapshotFrames(vt.terminal, frames)) / frames;
}

/// Measures every picked workload on every picked screen with every status line, and prints the fastest run
/// of each.
int runSnapshotBenchmark(std::span<vtbackend::test::Workload const> workloads,
                         std::span<BenchScreen const> screens,
                         SnapshotOptions const& options)
{
    cout << std::format(
        "Render snapshot: refreshRenderBuffer() per frame, mean of {} frames, fastest of {} runs\n",
        options.frames,
        options.runs);
    for (auto const& workload: workloads)
    {
        auto const fill = vtbackend::test::makeWorkload(workload, SnapshotFillBytes);
        for (auto const& screen: screens)
        {
            auto line = std::format(
                "{:<{}} {:<{}}", workload.name, WorkloadColumnWidth, screen.name, ScreenColumnWidth);
            for (auto const& statusLine: SnapshotStatusLines)
            {
                auto const fastest = fastestOf(
                    options.runs, [&] { return measureSnapshot(fill, screen, statusLine, options.frames); });
                line += std::format(" {:9.1f} us/frame ({})", fastest.count(), statusLine.label);
            }
            cout << line << '\n';
        }
    }
    return EXIT_SUCCESS;
}

} // namespace

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
        link("bench-headless.pty", bind(&ContourHeadlessBench::benchPTY, this));
        link("bench-headless.keypress", bind(&ContourHeadlessBench::benchKeyPress, this));
        link("bench-headless.throughput", bind(&ContourHeadlessBench::benchThroughput, this));
        link("bench-headless.snapshot", bind(&ContourHeadlessBench::benchSnapshot, this));
        link("bench-headless.pty-child", bind(&ContourHeadlessBench::playPtyChild, this));
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
        auto constexpr ThroughputDefaults = ThroughputOptions {};
        auto constexpr SnapshotDefaults = SnapshotOptions {};

        // What the throughput and snapshot benchmarks cover, which both pick alike: @see selectionFor().
        auto const kindsOption = CLI::Option { .name = "kinds",
                                               .v = CLI::Value { ""s },
                                               .helpText = "Comma-separated workloads, all when empty; an "
                                                           "unknown name lists the known ones.",
                                               .placeholder = "LIST" };
        auto const screensOption =
            CLI::Option { .name = "screens",
                          .v = CLI::Value { ""s },
                          .helpText = "Comma-separated screens (primary, alternate), both when empty.",
                          .placeholder = "LIST" };

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

        auto const throughputOptions = CLI::OptionList {
            CLI::Option { .name = "size",
                          .v = CLI::Value { static_cast<unsigned>(ThroughputDefaults.bytesPerWorkload
                                                                  / (size_t { 1024 } * 1024)) },
                          .helpText = "Mebibytes per workload.",
                          .placeholder = "MIB" },
            CLI::Option { .name = "runs",
                          .v = CLI::Value { ThroughputDefaults.runs },
                          .helpText = "Runs per workload and screen; the fastest counts." },
            kindsOption,
            screensOption,
            CLI::Option { .name = "read-size",
                          .v = CLI::Value { ThroughputDefaults.ptyReadSize },
                          .helpText = "Bytes per PTY read, which is what the parser takes per lock; the "
                                      "default is contour's.",
                          .placeholder = "BYTES" },
            CLI::Option { .name = "pty",
                          .v = CLI::Value { ThroughputDefaults.device == ThroughputDevice::RealPty },
                          .helpText = "Write each workload from a child process through a real PTY -- ConPTY "
                                      "on Windows -- instead of feeding it from memory." },
            CLI::Option { .name = "refresh-hz",
                          .v = CLI::Value { ThroughputDefaults.refreshHz },
                          .helpText =
                              "With pty: refresh the render buffer this many times a second while the "
                              "workload floods in, as the render thread does; 0 for never.",
                          .placeholder = "HZ" },
        };

        auto const snapshotOptions = CLI::OptionList {
            CLI::Option { .name = "frames",
                          .v = CLI::Value { SnapshotDefaults.frames },
                          .helpText = "Frames per run; the report shows their mean." },
            CLI::Option { .name = "runs",
                          .v = CLI::Value { SnapshotDefaults.runs },
                          .helpText = "Runs per workload, screen and status line; the fastest counts." },
            kindsOption,
            screensOption,
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
                                               "operating system's PTY only, writing into its slave end "
                                               "from this process. POSIX only: ConPTY has no slave end to "
                                               "write output into; `throughput pty` measures ConPTY instead.",
                                   .options =
                                       CLI::OptionList {
                                           CLI::Option { .name = "seconds",
                                                         .v = CLI::Value { 10u },
                                                         .helpText = "How long to write for." },
                                       } },
                    CLI::Command {
                        .name = "throughput",
                        .helpText = "Measures how fast the terminal takes in output -- PTY read, state lock, "
                                    "parse and grid, as contour's PTY thread runs them -- for each workload "
                                    "on each screen.",
                        .options = throughputOptions },
                    CLI::Command {
                        .name = "snapshot",
                        .helpText =
                            "Measures what the render thread does every frame while it holds the "
                            "terminal's state lock -- refreshRenderBuffer() -- once each workload has "
                            "filled the page, with no status line and with the indicator.",
                        .options = snapshotOptions },
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
                                CLI::Option { .name = "idle",
                                              .v = CLI::Value { KeyPressDefaults.load == ChildMode::Idle },
                                              .helpText = "The application only reads the keys and writes "
                                                          "no output." },
                            } },
                    CLI::Command {
                        .name = "pty-child",
                        .helpText = "Windows only, and refused elsewhere: plays the application on the other "
                                    "end of the PTY that throughput's pty and keypress run, where no POSIX "
                                    "shell plays it. Puts the console into raw mode, then writes the file "
                                    "once, only reads its input, or floods: reads its input while writing "
                                    "the file over and over until killed.",
                        .options =
                            CLI::OptionList {
                                CLI::Option { .name = "mode",
                                              .v = CLI::Value { ""s },
                                              .helpText = "write, idle or flood; an unknown name lists the "
                                                          "known ones.",
                                              .placeholder = "MODE",
                                              .presence = CLI::Presence::Required },
                                CLI::Option { .name = "file",
                                              .v = CLI::Value { ""s },
                                              .helpText = "What write and flood write.",
                                              .placeholder = "PATH" },
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
            .load = parameters().boolean("bench-headless.keypress.idle") ? ChildMode::Idle : ChildMode::Flood,
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

    /// Picks the workloads and screens that @p command's `kinds` and `screens` options name.
    /// @param command The subcommand, as the command line names it.
    /// @return Both, or a message naming the unknown name and every known one.
    [[nodiscard]] std::expected<BenchSelection, std::string> selectionFor(std::string_view command) const
    {
        auto const prefix = std::format("bench-headless.{}.", command);
        return pickByName<vtbackend::test::Workload>(
                   vtbackend::test::Workloads, parameters().str(prefix + "kinds"), "workload")
            .and_then([&](std::vector<vtbackend::test::Workload> workloads) {
                return pickByName<BenchScreen>(BenchScreens, parameters().str(prefix + "screens"), "screen")
                    .transform([&](std::vector<BenchScreen> screens) {
                        return BenchSelection { .workloads = std::move(workloads),
                                                .screens = std::move(screens) };
                    });
            });
    }

    /// Runs `bench-headless throughput`: how fast the terminal takes in each picked workload on each picked
    /// screen.
    /// @return EXIT_SUCCESS, or EXIT_FAILURE when an option is invalid or a run fails.
    int benchThroughput()
    {
        auto const selection = selectionFor("throughput");
        if (!selection)
        {
            cerr << selection.error() << '\n';
            return EXIT_FAILURE;
        }
        auto const options = ThroughputOptions {
            .bytesPerWorkload = size_t { parameters().uint("bench-headless.throughput.size") } * 1024 * 1024,
            .runs = parameters().uint("bench-headless.throughput.runs"),
            .ptyReadSize = parameters().uint("bench-headless.throughput.read-size"),
            .device = parameters().boolean("bench-headless.throughput.pty") ? ThroughputDevice::RealPty
                                                                            : ThroughputDevice::MockPty,
            .refreshHz = parameters().uint("bench-headless.throughput.refresh-hz"),
        };
        if (options.bytesPerWorkload == 0 || options.runs == 0 || options.ptyReadSize == 0)
        {
            cerr << "size, runs and read-size must each be at least 1.\n";
            return EXIT_FAILURE;
        }
        return runThroughputBenchmark(
            selection->workloads, selection->screens, options, processEnvironment());
    }

    /// Runs `bench-headless snapshot`: what the render thread does per frame, once each picked workload has
    /// filled each picked screen.
    /// @return EXIT_SUCCESS, or EXIT_FAILURE when an option is invalid.
    int benchSnapshot()
    {
        auto const selection = selectionFor("snapshot");
        if (!selection)
        {
            cerr << selection.error() << '\n';
            return EXIT_FAILURE;
        }
        auto const options = SnapshotOptions {
            .frames = parameters().uint("bench-headless.snapshot.frames"),
            .runs = parameters().uint("bench-headless.snapshot.runs"),
        };
        if (options.frames == 0 || options.runs == 0)
        {
            cerr << "frames and runs must each be at least 1.\n";
            return EXIT_FAILURE;
        }
        return runSnapshotBenchmark(selection->workloads, selection->screens, options);
    }

    /// Runs `bench-headless pty-child`: the application on the other end of a PTY benchmark's PTY.
    /// @return Its exit code, or EXIT_FAILURE when the mode is unknown.
    int playPtyChild()
    {
        auto const application = findByName<ChildApplication>(
            ChildApplications, parameters().str("bench-headless.pty-child.mode"), "mode");
        if (!application)
        {
            cerr << application.error() << '\n';
            return EXIT_FAILURE;
        }
        return runPtyChild(application->mode, parameters().str("bench-headless.pty-child.file"));
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

    int benchPTY()
    {
        using std::chrono::steady_clock;
        using vtpty::ColumnCount;
        using vtpty::createPty;
        using vtpty::LineCount;
        using vtpty::PageSize;
        using vtpty::Pty;

        if (ThisPty.slaveEnd == PtySlaveEnd::Absent)
        {
            cerr << "The pty benchmark writes into the PTY's slave end from this process, as an application "
                    "would, and ConPTY has no slave end to write output into: only the application's input. "
                    "Use `bench-headless throughput pty`, whose child process writes through ConPTY.\n";
            return EXIT_FAILURE;
        }

        auto constexpr WritesPerLoop = 1u;
        auto constexpr PtyWriteSize = 4096;
        auto constexpr PtyReadSize = 4096;
        auto const benchTime = chrono::seconds(parameters().uint("bench-headless.pty.seconds"));
        if (benchTime.count() < 1)
        {
            cerr << "seconds must be at least 1.\n";
            return EXIT_FAILURE;
        }

        // Setup benchmark
        std::string const text = createText(PtyWriteSize);
        unique_ptr<Pty> ptyObject =
            createPty(PageSize { .lines = LineCount(25), .columns = ColumnCount(80) }, std::nullopt);
        auto& pty = *ptyObject;
        // createPty() only describes the PTY: start() opens it, and only then is there a slave to configure.
        if (auto const started = pty.start(); !started)
        {
            cerr << std::format("Cannot open a PTY: {}\n", started.error());
            return EXIT_FAILURE;
        }
        auto& ptySlave = pty.slave();
        (void) ptySlave.configure();

        auto bufferObjectPool = crispy::BufferObjectPool<char>(4llu * 1024 * 1024);
        auto bufferObject = bufferObjectPool.allocateBufferObject();

        auto bytesTransferred = uint64_t { 0 };
        auto loopIterations = uint64_t { 0 };
        // Set before the PTY is closed. The reader cannot ask the PTY instead: isClosed() takes no lock.
        auto stopping = std::atomic<bool> { false };
        auto ptyStdoutReaderThread = std::thread { [&]() {
            while (!stopping.load())
            {
                auto const readResult = pty.read(*bufferObject, std::chrono::seconds(2), PtyReadSize);
                if (!readResult)
                {
                    // Nothing to read yet is no reason to stop: once the PTY is full, the writer blocks
                    // until this loop drains it, and nothing else would.
                    if (errno == EAGAIN || errno == EINTR)
                        continue;
                    break;
                }
                auto const dataChunk = readResult.value().data;
                if (dataChunk.empty())
                    break;
                bytesTransferred += dataChunk.size();
                loopIterations++;
            }
        } };
        auto cleanupReader = core::Finally { [&]() {
            stopping.store(true);
            pty.close();
            ptyStdoutReaderThread.join();
        } };

        // Perform benchmark
        std::cout << std::format("Running PTY benchmark ...\n");
        auto const startTime = steady_clock::now();
        auto stopTime = startTime;
        while (stopTime - startTime < benchTime)
        {
            for ([[maybe_unused]] auto const write: core::times(WritesPerLoop))
                (void) ptySlave.write(text);
            stopTime = steady_clock::now();
        }

        cleanupReader.run();

        // Create summary
        auto const elapsedTime = stopTime - startTime;
        auto const msecs = std::chrono::duration_cast<std::chrono::milliseconds>(elapsedTime);
        auto const seconds = std::chrono::duration<double>(elapsedTime).count();
        auto const bytesPerSecond = static_cast<uint64_t>(static_cast<double>(bytesTransferred) / seconds);
        auto const averageRead = loopIterations != 0 ? bytesTransferred / loopIterations : uint64_t { 0 };

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
        std::cout << std::format("Average size per read  : {}\n", core::humanReadableBytes(averageRead));
        std::cout << std::format("Transfer speed         : {} per second\n",
                                 core::humanReadableBytes(bytesPerSecond));

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
