// SPDX-License-Identifier: Apache-2.0
//
// The child half of the ConPty tests that run a program (ConPty_test.cpp): a console program that
// writes OSC 133 marks the way a shell integration does -- prompt start and end, command start with
// its command line, command end -- followed by a sentinel, and keeps the pseudoconsole open long
// enough to be read.

#include <chrono>
#include <string_view>
#include <thread>

#include <Windows.h>

int main()
{
    auto* const output = GetStdHandle(STD_OUTPUT_HANDLE);

    // A console program sees escape sequences interpreted only once it asks for that, as every
    // shell does; without it the console would print ESC as a glyph and pass nothing through.
    auto mode = DWORD {};
    GetConsoleMode(output, &mode);
    SetConsoleMode(output, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    constexpr auto Probe = std::string_view { "\x1b]133;A\x1b\\prompt> \x1b]133;B\x1b\\\r\n"
                                              "\x1b]133;C;cmdline_url=echo%20hi\x1b\\hi\r\n"
                                              "\x1b]133;D;0\x1b\\contour-probe-done\r\n" };
    auto written = DWORD {};
    WriteFile(output, Probe.data(), static_cast<DWORD>(Probe.size()), &written, nullptr);

    // ConPTY renders asynchronously; a child that exits at once can take its last frame with it.
    std::this_thread::sleep_for(std::chrono::seconds { 3 });
    return 0;
}
