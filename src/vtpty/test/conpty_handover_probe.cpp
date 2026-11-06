// SPDX-License-Identifier: Apache-2.0

/// @file
/// The two processes of a pseudoconsole handed from one process to another, for ConPty_test:
///
/// - `program` is what runs on the pseudoconsole: it says `ready`, then answers each line it reads -- `size`
///   with the console's size as `size=COLUMNSxLINES`, `quit` by exiting with QuitExitCode.
/// - `adopter` is the process the pseudoconsole is handed to: it adopts the pseudoconsole and the program on
/// it
///   from the handles it inherited (ConPty::adopt(), Process::adopt()), resizes it, has the program tell the
///   size it sees, has it quit, closes the pseudoconsole and waits for its console host to end. It exits 0
///   when all of that held, and says on stderr what did not otherwise.

#include <vtpty/ConPty.hpp>
#include <vtpty/CrossingHandle.hpp>
#include <vtpty/InheritedPty.hpp>
#include <vtpty/PageSize.hpp>
#include <vtpty/Process.hpp>
#include <vtpty/Win32Handle.hpp>
#include <vtpty/test/PtyReading.hpp>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>

namespace
{

/// What the program exits with when told to quit.
constexpr auto QuitExitCode = 42;

/// The size the adopter resizes the pseudoconsole to.
constexpr auto AdoptedSize =
    vtpty::PageSize { .lines = vtpty::LineCount(40), .columns = vtpty::ColumnCount(100) };

/// The adopter's failures, each its own exit code.
enum class AdopterFailure : uint8_t
{
    Usage = 1,
    PseudoconsoleNotAdopted,
    ProgramNotAdopted,
    ResizeNotSeen,
    WrongExitCode,
    ConsoleHostStayed,
};

int fail(AdopterFailure failure, std::string_view why)
{
    std::fprintf(stderr, "conpty_handover_probe adopter: %.*s\n", static_cast<int>(why.size()), why.data());
    return static_cast<int>(failure);
}

/// @return @p text as a number, or nullopt.
std::optional<uint64_t> numberOf(std::string_view text)
{
    auto value = uint64_t {};
    auto const [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc {} || end != text.data() + text.size())
        return std::nullopt;
    return value;
}

/// The hosted program: answers `size` and `quit`, one per line.
int program()
{
    auto* const console = GetStdHandle(STD_OUTPUT_HANDLE);
    std::printf("ready\n");
    std::fflush(stdout);
    auto line = std::array<char, 256> {};
    while (std::fgets(line.data(), static_cast<int>(line.size()), stdin) != nullptr)
    {
        auto const command = std::string_view { line.data() };
        if (command.starts_with("quit"))
            return QuitExitCode;
        if (command.starts_with("size"))
        {
            auto info = CONSOLE_SCREEN_BUFFER_INFO {};
            GetConsoleScreenBufferInfo(console, &info);
            std::printf("size=%dx%d\n",
                        info.srWindow.Right - info.srWindow.Left + 1,
                        info.srWindow.Bottom - info.srWindow.Top + 1);
            std::fflush(stdout);
        }
    }
    return 0;
}

/// Reads @p pty until @p needle, answering the pseudoconsole's queries along the way.
/// @return Whether it came.
bool awaitOutput(vtpty::Pty& pty, std::string_view needle)
{
    return vtpty::testing::readUntilAnswering(
               pty, [needle](std::string_view text) { return text.contains(needle); })
        .contains(needle);
}

/// The process the pseudoconsole is handed to. @p arguments: the five handles of InheritedConPty, in their
/// order, then the program's process id and process handle, then the size at hand-off, columns and lines.
int adopter(std::span<char const* const> arguments)
{
    auto values = std::vector<uint64_t> {};
    for (auto const* argument: arguments)
    {
        auto const value = numberOf(argument);
        if (!value)
            return fail(AdopterFailure::Usage, std::format("'{}' is no number", argument));
        values.push_back(*value);
    }
    if (values.size() != 9)
        return fail(AdopterFailure::Usage, "expected 9 numbers");

    auto const crossing = [&values](std::size_t index) {
        return vtpty::CrossingHandle { static_cast<std::int64_t>(values.at(index)) };
    };
    auto const handles = vtpty::InheritedConPty {
        .outputReader = crossing(0),
        .inputWriter = crossing(1),
        .signal = crossing(2),
        .reference = crossing(3),
        .consoleHost = crossing(4),
    };
    auto const handedOverSize =
        vtpty::PageSize { .lines = vtpty::LineCount(static_cast<int>(values[8])),
                          .columns = vtpty::ColumnCount(static_cast<int>(values[7])) };
    // Kept to wait on once the pseudoconsole is closed, which closes its own.
    auto consoleHost = HANDLE {};
    DuplicateHandle(GetCurrentProcess(),
                    handles.consoleHost.native(),
                    GetCurrentProcess(),
                    &consoleHost,
                    SYNCHRONIZE,
                    FALSE,
                    0);

    auto pty = vtpty::ConPty::adopt(vtpty::InheritedPty { .pageSize = handedOverSize, .conpty = handles });
    if (!pty)
        return fail(AdopterFailure::PseudoconsoleNotAdopted,
                    std::format("ConPty::adopt() refused with {}", std::to_underlying(pty.error())));
    auto process = vtpty::Process::adopt(
        vtpty::AdoptedChild { .pid = static_cast<int64_t>(values[5]), .processHandle = crossing(6) },
        *std::move(pty));
    if (!process)
        return fail(AdopterFailure::ProgramNotAdopted,
                    std::format("Process::adopt() refused with {}", std::to_underlying(process.error())));
    auto& adopted = **process;

    adopted.resizeScreen(AdoptedSize);
    std::ignore = adopted.write("size\r\n");
    auto const seen = std::format("size={}x{}", AdoptedSize.columns.value, AdoptedSize.lines.value);
    if (!awaitOutput(adopted, seen))
        return fail(AdopterFailure::ResizeNotSeen, std::format("the program never said '{}'", seen));

    std::ignore = adopted.write("quit\r\n");
    auto const status = adopted.wait();
    auto const* const exit = status ? std::get_if<vtpty::Process::NormalExit>(&*status) : nullptr;
    if (exit == nullptr || exit->exitCode != QuitExitCode)
        return fail(AdopterFailure::WrongExitCode, "the program did not exit as told");

    adopted.close();
    if (WaitForSingleObject(consoleHost, 10'000) != WAIT_OBJECT_0)
        return fail(AdopterFailure::ConsoleHostStayed, "the console host still runs after the close");
    CloseHandle(consoleHost);
    return 0;
}

} // namespace

int main(int argc, char const* argv[])
{
    auto const arguments = std::span { argv, static_cast<std::size_t>(argc) };
    if (arguments.size() >= 2 && std::string_view { arguments[1] } == "program")
        return program();
    if (arguments.size() >= 2 && std::string_view { arguments[1] } == "adopter")
        return adopter(arguments.subspan(2));
    std::fprintf(stderr,
                 "usage: conpty_handover_probe program | adopter HANDLES... PID PROCESS COLUMNS LINES\n");
    return 64;
}
