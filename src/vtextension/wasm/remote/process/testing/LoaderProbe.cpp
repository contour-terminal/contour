// SPDX-License-Identifier: Apache-2.0
//
// The loader probe (Windows only, test-only, never installed): what a process can load, said on its stderr by
// one that imports nothing but kernel32 -- its C runtime linked in -- so that it starts wherever a process
// can start at all, contained or not.
//
//   extension_host_loader_probe [LIBRARY]...
//
// It writes one line each, to the stderr it was started with: `started`; `console=ok`, or
// `console=error:<code>` with the Win32 error, for whether it may create a console of its own
// (AllocConsole(), freed again at once -- on a desktop a console window may flash); `library=<LIBRARY> ok` or
// `library=<LIBRARY> error:<code>` for each LIBRARY in the order given (none may hold a double quote), loaded
// with LoadLibraryExW() and kept loaded, so that a library is tried after those it imports when they come
// first -- and for a LIBRARY named without a directory, an API set among them, `resolved=<LIBRARY> <file>`,
// the file the loader mapped it to; then `end`. It exits 0.
#define WIN32_LEAN_AND_MEAN
#include <format>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <Windows.h>

namespace
{

/// Writes @p line, UTF-16, to @p report as one UTF-8 line.
void say(HANDLE report, std::wstring_view line)
{
    auto text = std::string {};
    if (!line.empty())
    {
        auto const size = ::WideCharToMultiByte(
            CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        text.resize(static_cast<std::size_t>(size));
        ::WideCharToMultiByte(
            CP_UTF8, 0, line.data(), static_cast<int>(line.size()), text.data(), size, nullptr, nullptr);
    }
    text += '\n';
    auto written = DWORD { 0 };
    ::WriteFile(report, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

/// @return `ok` for ERROR_SUCCESS, else `error:<code>` with @p error, a Win32 error.
[[nodiscard]] std::wstring outcomeOf(DWORD error)
{
    if (error == ERROR_SUCCESS)
        return L"ok";
    return std::format(L"error:{}", error);
}

/// @return The Win32 error of a call that returned @p result: ERROR_SUCCESS if it is not zero, the thread's
///         last error if it is.
template <typename Result>
[[nodiscard]] DWORD errorOf(Result result)
{
    return result ? DWORD { ERROR_SUCCESS } : ::GetLastError();
}

/// @return The arguments of @p commandLine, the program's name first: split at blanks outside double
///         quotes, the quotes dropped -- enough for paths, which hold no quote. Parsed here, from the wide
///         command line, because the entry point is main(), whose argv is in the ANSI code page; and
///         CommandLineToArgvW() lives in shell32, which this process must not import.
[[nodiscard]] std::vector<std::wstring> argumentsOf(std::wstring_view commandLine)
{
    auto arguments = std::vector<std::wstring> {};
    auto argument = std::optional<std::wstring> {};
    auto quoted = false;
    for (auto const c: commandLine)
    {
        if (c == L'"')
        {
            quoted = !quoted;
            if (!argument)
                argument.emplace();
        }
        else if ((c == L' ' || c == L'\t') && !quoted)
        {
            if (argument)
                arguments.push_back(*std::exchange(argument, std::nullopt));
        }
        else
        {
            if (!argument)
                argument.emplace();
            *argument += c;
        }
    }
    if (argument)
        arguments.push_back(*std::move(argument));
    return arguments;
}

/// @return The file @p module was mapped from; empty if the system does not say.
[[nodiscard]] std::wstring fileOf(HMODULE module)
{
    auto file = std::wstring(MAX_PATH, L'\0');
    auto const length = ::GetModuleFileNameW(module, file.data(), static_cast<DWORD>(file.size()));
    file.resize(length < file.size() ? length : 0);
    return file;
}

} // namespace

int main()
{
    // Read once, before AllocConsole() points the standard handles at a console of its own.
    auto* const report = ::GetStdHandle(STD_ERROR_HANDLE);
    say(report, L"started");
    auto const consoleError = errorOf(::AllocConsole());
    say(report, std::format(L"console={}", outcomeOf(consoleError)));
    if (consoleError == ERROR_SUCCESS)
        ::FreeConsole();
    for (auto const& library: argumentsOf(::GetCommandLineW()) | std::views::drop(1))
    {
        auto* const module = ::LoadLibraryExW(library.c_str(), nullptr, 0);
        say(report, std::format(L"library={} {}", library, outcomeOf(errorOf(module))));
        if (module != nullptr && library.find_first_of(L"\\/") == std::wstring::npos)
            say(report, std::format(L"resolved={} {}", library, fileOf(module)));
    }
    say(report, L"end");
    return 0;
}
