// SPDX-License-Identifier: Apache-2.0
//
// The child half of the ConPty tests that run a program (ConPty_test.cpp). A console program that writes
// what it was started with, each as UTF-8 in hex on a line of its own -- hex, so that the pseudoconsole
// finds nothing in them to interpret:
//   "argument: [<hex>]"     each argument it was given;
//   "environment: [<hex>]"  each variable whose name starts with CONTOUR_PROBE_, as NAME=VALUE;
//   "directory: [<hex>]"    the directory it runs in.
// Then OSC 133 marks the way a shell integration does (prompt start and end, command start with its
// command line, command end), followed by a sentinel; and it keeps the pseudoconsole open long enough to
// be read. All of it is read through the wide API, which spells what it was given as it was given.
//
// Given LingerOnClose as an argument, it stays until its pseudoconsole is closed, and then holds that close
// up for a while, as a program saving its work does.

#include <core/Utils.hpp>
#include <core/WideString.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>

#include <Windows.h>
#include <shellapi.h>

namespace
{

/// The argument that makes the probe linger: stay, and hold its console's close up.
constexpr auto LingerOnClose = std::wstring_view { L"--linger-on-close" };

/// Holds a console's close up for two seconds, of the few Windows gives a program before it ends it.
BOOL WINAPI lingerOnClose(DWORD event)
{
    if (event == CTRL_CLOSE_EVENT)
        std::this_thread::sleep_for(std::chrono::seconds { 2 });
    return FALSE;
}

/// Writes @p text to @p output, as it is.
void write(HANDLE output, std::string_view text)
{
    auto written = DWORD {};
    WriteFile(output, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

/// Writes "<label>: [<hex>]" to @p output, the hex that of @p text's UTF-8.
void report(HANDLE output, std::string_view label, std::wstring_view text)
{
    write(output,
          std::format("{}: [{}]\r\n", label, core::toHexString(std::string_view { core::toUtf8(text) })));
}

} // namespace

int main()
{
    auto* const output = GetStdHandle(STD_OUTPUT_HANDLE);

    // A console program sees escape sequences interpreted only once it asks for that, as every
    // shell does; without it the console would print ESC as a glyph and pass nothing through.
    auto mode = DWORD {};
    GetConsoleMode(output, &mode);
    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    // The arguments as CommandLineToArgvW() splits the command line, by the rules the C runtime splits it
    // with -- which is what a test of their quoting reads. Wide, and so not through the ANSI code page.
    auto count = 0;
    auto lingers = false;
    auto* const arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    for (wchar_t const* const argument:
         std::span { arguments, static_cast<std::size_t>(count) } | std::views::drop(1))
    {
        report(output, "argument", argument);
        lingers = lingers || argument == LingerOnClose;
    }
    LocalFree(static_cast<HLOCAL>(arguments));
    if (lingers)
        SetConsoleCtrlHandler(&lingerOnClose, TRUE);

    // In the order of the environment block, which Windows keeps sorted by name.
    auto* const environment = GetEnvironmentStringsW();
    auto const* entry = environment;
    while (*entry != L'\0')
    {
        auto const variable = std::wstring { entry };
        if (variable.starts_with(L"CONTOUR_PROBE_"))
            report(output, "environment", variable);
        entry += variable.size() + 1;
    }
    FreeEnvironmentStringsW(environment);

    auto directory = std::wstring(MAX_PATH * 4, L'\0');
    directory.resize(GetCurrentDirectoryW(static_cast<DWORD>(directory.size()), directory.data()));
    report(output, "directory", directory);

    write(output,
          "\x1b]133;A\x1b\\prompt> \x1b]133;B\x1b\\\r\n"
          "\x1b]133;C;cmdline_url=echo%20hi\x1b\\hi\r\n"
          "\x1b]133;D;0\x1b\\contour-probe-done\r\n");

    // ConPTY renders asynchronously; a child that exits at once can take its last frame with it.
    std::this_thread::sleep_for(lingers ? std::chrono::seconds { 60 } : std::chrono::seconds { 3 });
    return 0;
}
