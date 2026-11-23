// SPDX-License-Identifier: Apache-2.0
//
// The child of the process runner's Windows tests (ProcessRunner_win32_test.cpp). A console program that runs
// its arguments as a sequence of verbs, each followed by as many arguments as it takes; `args` takes all that
// follow, `spawn` and `spawn-quiet` all up to a `--` (or the end), which the probe then goes on after. At the
// end of the verbs it exits 0.
//
//   out <text> / err <text>     writes <text> and "\n" to stdout / stderr
//   zeros <n> / zeros-err <n>   writes n zero bytes to stdout / stderr, 64 KiB a write
//   exit <n>                    ExitProcess(n), n decimal, may be negative
//   sleep <ms>                  Sleep(ms)
//   pid <file>                  writes its pid (decimal, no newline) to <file>, through <file>.tmp
//   touch <file>                creates <file>
//   env                         writes each entry of its environment block, one a line
//   cwd                         writes its working directory and "\n"
//   self                        writes the path of the file that runs and "\n"
//   stdin                       reads stdin to its end, writes the byte count and "\n"
//   close-out / close-err       closes its stdout / stderr
//   args ...                    writes the remaining arguments, '|' between each, and "\n"
//   spawn <file> ... [--]       starts itself with the verbs up to `--` on its own standard handles, writes
//                               the grandchild's pid to <file>, does not wait; runs the verbs after `--`
//   spawn-quiet <file> ... [--] the same, the grandchild's standard handles NUL and nothing else inherited
//   crash                       raises an exception nothing handles, of code UnhandledExceptionCode
//
// Everything it writes is UTF-8, through WriteFile(): no C runtime buffer stands between a verb and its pipe.
// An unknown verb, or one short of its arguments, is reported on stderr and exits 64.

#include <core/WideString.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <iterator>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <Windows.h>
#include <fcntl.h>
#include <io.h>
#include <shellapi.h>

namespace
{

/// The arguments a verb is given.
using Arguments = std::span<std::wstring_view const>;

/// How many zero bytes one write of `zeros` carries at most.
constexpr auto ZeroChunk = std::size_t { 64 } * 1024;

/// The character that ends each entry of an environment block, and the block.
constexpr auto EntryEnd = L'\0';

/// The exit code of a command line it cannot run.
constexpr auto UsageError = UINT { 64 };

/// The code of the exception `crash` raises; ProcessRunner_win32_test.cpp expects it as the exit code.
constexpr auto UnhandledExceptionCode = DWORD { 0xE0000042 };

/// Writes @p text to @p output, as it is.
void write(HANDLE output, std::string_view text)
{
    auto written = DWORD {};
    WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

/// Reports @p message on stderr and exits as a usage error.
[[noreturn]] void fail(std::string_view message)
{
    write(GetStdHandle(STD_ERROR_HANDLE), std::format("process_runner_probe: {}\n", message));
    ExitProcess(UsageError);
}

/// @return @p text as a decimal number of type @p T; a usage error if it is not one.
template <typename T>
[[nodiscard]] T number(std::wstring_view text)
{
    auto const utf8 = core::toUtf8(text);
    auto value = T {};
    auto const [end, error] = std::from_chars(utf8.data(), utf8.data() + utf8.size(), value);
    if (error != std::errc {} || end != utf8.data() + utf8.size())
        fail(std::format("not a number: {}", utf8));
    return value;
}

/// Writes @p text to @p path whole or not at all: to `<path>.tmp` first, then moved over @p path.
void writeWhole(std::wstring_view path, std::string_view text)
{
    auto const temporary = std::wstring { path } + L".tmp";
    {
        auto file = std::ofstream { std::filesystem::path { temporary }, std::ios::binary };
        file << text;
        if (!file.flush())
            fail("cannot write the temporary file");
    }
    if (MoveFileExW(temporary.c_str(), std::wstring { path }.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE)
        fail("cannot move the temporary file into place");
}

/// @return @p argument quoted as CommandLineToArgvW() and the C runtime split it back.
[[nodiscard]] std::wstring quotedArgument(std::wstring_view argument)
{
    if (!argument.empty() && argument.find_first_of(L" \t\n\v\"") == std::wstring_view::npos)
        return std::wstring { argument };
    auto result = std::wstring { L"\"" };
    auto backslashes = std::size_t { 0 };
    for (auto const character: argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        // Backslashes double only before a quote, which itself is escaped.
        auto const count = character == L'"' ? (backslashes * 2) + 1 : backslashes;
        result.append(count, L'\\');
        result += character;
        backslashes = 0;
    }
    result.append(backslashes * 2, L'\\');
    result += L'"';
    return result;
}

/// @return The path of the file this process runs.
[[nodiscard]] std::wstring selfPath()
{
    auto path = std::wstring(32768, L'\0');
    path.resize(GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size())));
    return path;
}

/// What a grandchild's standard handles are.
enum class GrandchildStreams : uint8_t
{
    Ours = 0, ///< This process's own, inherited with every other inheritable handle.
    Null,     ///< NUL, and nothing else inherited.
};

/// Starts this program with @p verbs, and writes the grandchild's pid to @p pidFile; does not wait.
void spawnSelf(std::wstring_view pidFile, Arguments verbs, GrandchildStreams streams)
{
    auto const self = selfPath();
    auto commandLine = quotedArgument(self);
    for (auto const verb: verbs)
        commandLine += L' ' + quotedArgument(verb);

    auto startup = STARTUPINFOEXW {};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;

    auto attributeStorage = std::vector<std::byte> {};
    auto* nullInput = INVALID_HANDLE_VALUE;
    auto* nullOutput = INVALID_HANDLE_VALUE;
    auto inherited = std::array<HANDLE, 2> {};
    auto flags = DWORD { 0 };
    switch (streams)
    {
        case GrandchildStreams::Ours: {
            auto const handles = std::array { STD_INPUT_HANDLE, STD_OUTPUT_HANDLE, STD_ERROR_HANDLE };
            // Inherited only where inheritable; a standard handle this process was given may not be.
            for (auto const which: handles)
                SetHandleInformation(GetStdHandle(which), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
            startup.StartupInfo.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
            startup.StartupInfo.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
            startup.StartupInfo.hStdError = GetStdHandle(STD_ERROR_HANDLE);
            break;
        }
        case GrandchildStreams::Null: {
            auto security = SECURITY_ATTRIBUTES { .nLength = sizeof(SECURITY_ATTRIBUTES),
                                                  .lpSecurityDescriptor = nullptr,
                                                  .bInheritHandle = TRUE };
            nullInput = CreateFileW(L"NUL",
                                    GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &security,
                                    OPEN_EXISTING,
                                    0,
                                    nullptr);
            nullOutput = CreateFileW(L"NUL",
                                     GENERIC_WRITE,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &security,
                                     OPEN_EXISTING,
                                     0,
                                     nullptr);
            if (nullInput == INVALID_HANDLE_VALUE || nullOutput == INVALID_HANDLE_VALUE)
                fail("cannot open NUL");
            startup.StartupInfo.hStdInput = nullInput;
            startup.StartupInfo.hStdOutput = nullOutput;
            startup.StartupInfo.hStdError = nullOutput;

            // A handle list: NUL alone crosses, whatever else this process holds that is inheritable.
            inherited = { nullInput, nullOutput };
            auto size = SIZE_T { 0 };
            InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
            attributeStorage.resize(size);
            startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
            if (InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size) == FALSE
                || UpdateProcThreadAttribute(startup.lpAttributeList,
                                             0,
                                             PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                             static_cast<void*>(inherited.data()),
                                             inherited.size() * sizeof(HANDLE),
                                             nullptr,
                                             nullptr)
                       == FALSE)
                fail("cannot build the handle list");
            flags = EXTENDED_STARTUPINFO_PRESENT;
            break;
        }
    }

    auto process = PROCESS_INFORMATION {};
    if (CreateProcessW(self.c_str(),
                       commandLine.data(),
                       nullptr,
                       nullptr,
                       TRUE,
                       flags,
                       nullptr,
                       nullptr,
                       &startup.StartupInfo,
                       &process)
        == FALSE)
        fail(std::format("cannot start the grandchild: {}", core::systemErrorMessage(GetLastError())));
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    if (startup.lpAttributeList != nullptr)
        DeleteProcThreadAttributeList(startup.lpAttributeList);
    if (nullInput != INVALID_HANDLE_VALUE)
        CloseHandle(nullInput);
    if (nullOutput != INVALID_HANDLE_VALUE)
        CloseHandle(nullOutput);
    writeWhole(pidFile, std::to_string(process.dwProcessId));
}

/// Writes as many zero bytes as @p arguments' first says to @p output, ZeroChunk a write.
void writeZeros(HANDLE output, Arguments arguments)
{
    static auto const chunk = std::string(ZeroChunk, '\0');
    auto const count = number<std::size_t>(arguments.front());
    auto const writes = (count + ZeroChunk - 1) / ZeroChunk;
    for (auto const index: std::views::iota(std::size_t { 0 }, writes))
        write(output, std::string_view { chunk }.substr(0, std::min(ZeroChunk, count - (index * ZeroChunk))));
}

/// Creates the file @p path, empty.
void touch(std::wstring_view path)
{
    auto const file = std::ofstream { std::filesystem::path { path } };
    if (!file)
        fail("cannot create the file");
}

/// Writes the path of the file this process runs, and a newline.
void writeSelfPath()
{
    write(GetStdHandle(STD_OUTPUT_HANDLE), core::toUtf8(selfPath()) + '\n');
}

/// Writes each entry of this process's environment block, one a line.
void writeEnvironment()
{
    auto* const block = GetEnvironmentStringsW();
    if (block == nullptr)
        fail("no environment block");
    // The block ends at the first empty entry: two NULs in a row, or one at its very start.
    auto const* const begin = static_cast<wchar_t const*>(block);
    auto const all = std::ranges::subrange(begin, std::unreachable_sentinel);
    auto const* const end =
        *begin == EntryEnd ? begin : std::ranges::search(all, std::array { EntryEnd, EntryEnd }).begin();
    auto text = std::string {};
    for (auto const entry: std::wstring_view { begin, end } | std::views::split(EntryEnd))
        text += core::toUtf8(std::wstring_view { entry.begin(), entry.end() }) + '\n';
    FreeEnvironmentStringsW(block);
    write(GetStdHandle(STD_OUTPUT_HANDLE), text);
}

/// Writes the working directory and "\n".
void writeWorkingDirectory()
{
    auto directory = std::wstring(GetCurrentDirectoryW(0, nullptr), L'\0');
    directory.resize(GetCurrentDirectoryW(static_cast<DWORD>(directory.size()), directory.data()));
    write(GetStdHandle(STD_OUTPUT_HANDLE), core::toUtf8(directory) + '\n');
}

/// Reads stdin to its end, and writes how many bytes it held and "\n".
void countStandardInput()
{
    _setmode(_fileno(stdin), _O_BINARY);
    auto const count =
        std::distance(std::istreambuf_iterator<char> { std::cin }, std::istreambuf_iterator<char> {});
    write(GetStdHandle(STD_OUTPUT_HANDLE), std::format("{}\n", count));
}

/// Writes @p arguments, '|' between each, and "\n".
void writeArguments(Arguments arguments)
{
    auto line = std::string {};
    for (auto const argument: arguments)
    {
        if (!line.empty())
            line += '|';
        line += core::toUtf8(argument);
    }
    write(GetStdHandle(STD_OUTPUT_HANDLE), line + '\n');
}

/// Whether a verb takes arguments that follow its own.
enum class Trailing : uint8_t
{
    None = 0,       ///< It takes its own arguments alone.
    Rest,           ///< It takes all that follow, too.
    UntilSeparator, ///< It takes all up to the Separator, too; the verbs after it run next.
};

/// What ends the verbs a `spawn` hands its grandchild.
constexpr auto Separator = std::wstring_view { L"--" };

/// How many arguments a verb takes.
struct Arity
{
    std::size_t fixed;                  ///< How many it needs, at least.
    Trailing trailing = Trailing::None; ///< Whether it takes the rest, too.
};

/// One verb: its name, how many arguments follow it, and what it does with them.
struct Verb
{
    std::wstring_view name;
    Arity arity;
    void (*action)(Arguments arguments);
};

/// Writes @p arguments' first, and a newline, to @p output.
void writeLine(HANDLE output, Arguments arguments)
{
    write(output, core::toUtf8(arguments.front()) + '\n');
}

/// Every verb the probe knows.
constexpr auto Verbs = std::array {
    Verb { .name = L"out",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { writeLine(GetStdHandle(STD_OUTPUT_HANDLE), arguments); } },
    Verb { .name = L"err",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { writeLine(GetStdHandle(STD_ERROR_HANDLE), arguments); } },
    Verb { .name = L"zeros",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { writeZeros(GetStdHandle(STD_OUTPUT_HANDLE), arguments); } },
    Verb { .name = L"zeros-err",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { writeZeros(GetStdHandle(STD_ERROR_HANDLE), arguments); } },
    Verb { .name = L"exit",
           .arity = { .fixed = 1 },
           .action =
               [](Arguments arguments) {
                   ExitProcess(static_cast<UINT>(number<std::int32_t>(arguments.front())));
               } },
    Verb { .name = L"sleep",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { Sleep(number<DWORD>(arguments.front())); } },
    Verb { .name = L"pid",
           .arity = { .fixed = 1 },
           .action =
               [](Arguments arguments) {
                   writeWhole(arguments.front(), std::to_string(GetCurrentProcessId()));
               } },
    Verb { .name = L"touch",
           .arity = { .fixed = 1 },
           .action = [](Arguments arguments) { touch(arguments.front()); } },
    Verb { .name = L"env", .arity = { .fixed = 0 }, .action = [](Arguments) { writeEnvironment(); } },
    Verb { .name = L"cwd", .arity = { .fixed = 0 }, .action = [](Arguments) { writeWorkingDirectory(); } },
    Verb { .name = L"self", .arity = { .fixed = 0 }, .action = [](Arguments) { writeSelfPath(); } },
    Verb { .name = L"stdin", .arity = { .fixed = 0 }, .action = [](Arguments) { countStandardInput(); } },
    Verb { .name = L"close-out",
           .arity = { .fixed = 0 },
           .action = [](Arguments) { CloseHandle(GetStdHandle(STD_OUTPUT_HANDLE)); } },
    Verb { .name = L"close-err",
           .arity = { .fixed = 0 },
           .action = [](Arguments) { CloseHandle(GetStdHandle(STD_ERROR_HANDLE)); } },
    Verb { .name = L"args",
           .arity = { .fixed = 0, .trailing = Trailing::Rest },
           .action = [](Arguments arguments) { writeArguments(arguments); } },
    Verb { .name = L"spawn",
           .arity = { .fixed = 1, .trailing = Trailing::UntilSeparator },
           .action =
               [](Arguments arguments) {
                   spawnSelf(arguments.front(), arguments.subspan(1), GrandchildStreams::Ours);
               } },
    Verb { .name = L"spawn-quiet",
           .arity = { .fixed = 1, .trailing = Trailing::UntilSeparator },
           .action =
               [](Arguments arguments) {
                   spawnSelf(arguments.front(), arguments.subspan(1), GrandchildStreams::Null);
               } },
    Verb { .name = L"crash",
           .arity = { .fixed = 0 },
           .action = [](Arguments) { RaiseException(UnhandledExceptionCode, 0, 0, nullptr); } },
};

/// Runs the verbs @p arguments spell, the first of them and then the rest.
void run(Arguments arguments)
{
    if (arguments.empty())
        return;
    auto const name = arguments.front();
    auto const verb = std::ranges::find(Verbs, name, &Verb::name);
    if (verb == Verbs.end())
        fail(std::format("unknown verb: {}", core::toUtf8(name)));
    auto const rest = arguments.subspan(1);
    if (rest.size() < verb->arity.fixed)
        fail(std::format("{} needs {} argument(s)", core::toUtf8(name), verb->arity.fixed));
    auto taken = verb->arity.fixed;
    auto skipped = std::size_t { 0 };
    switch (verb->arity.trailing)
    {
        case Trailing::None: break;
        case Trailing::Rest: taken = rest.size(); break;
        case Trailing::UntilSeparator: {
            auto const trailing = rest.subspan(verb->arity.fixed);
            taken += static_cast<std::size_t>(
                std::distance(trailing.begin(), std::ranges::find(trailing, Separator)));
            skipped = taken < rest.size() ? 1 : 0;
            break;
        }
    }
    verb->action(rest.first(taken));
    run(rest.subspan(taken + skipped));
}

} // namespace

int main()
{
    // Wide, and so not through the ANSI code page: what the runner's UTF-16 command line carried.
    auto count = 0;
    auto* const argv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (argv == nullptr)
        return static_cast<int>(UsageError);
    auto const arguments =
        std::span { argv, static_cast<std::size_t>(count) } | std::views::drop(1)
        | std::views::transform([](wchar_t const* argument) { return std::wstring_view { argument }; })
        | std::ranges::to<std::vector>();
    run(arguments);
    LocalFree(static_cast<HLOCAL>(argv));
    return 0;
}
