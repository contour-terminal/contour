// SPDX-License-Identifier: Apache-2.0
//
// The program the ProgramCommandCapture tests run (CommandCapture_test.cpp). A GUI-subsystem program that, as
// contour.exe does at start, attaches to its parent's console, then writes the arguments it was given to its
// standard output, '|' between each, on one line.
//
// Attaching replaces each standard handle the program was not started with as such (STARTF_USESTDHANDLES):
// cmd.exe hands its children its own by inheritance alone, so under cmd.exe the line goes to cmd.exe's
// console instead of the pipe it was started with. Only a capture that starts the program itself reads it.

#include <core/WideString.hpp>

#include <cstddef>
#include <span>
#include <string>

#include <Windows.h>
#include <shellapi.h>

int WINAPI wWinMain(HINSTANCE /*instance*/, HINSTANCE /*previous*/, PWSTR /*commandLine*/, int /*show*/)
{
    AttachConsole(ATTACH_PARENT_PROCESS);

    auto count = 0;
    auto* const arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr)
        return 1;
    auto line = std::string {};
    for (auto const* const argument: std::span { arguments, static_cast<std::size_t>(count) }.subspan(1))
    {
        if (!line.empty())
            line += '|';
        line += core::toUtf8(argument);
    }
    LocalFree(static_cast<HLOCAL>(arguments));
    line += '\n';

    // Through the handle as it is now, after the attach, as contour.exe's own reopening of its streams does.
    auto written = DWORD { 0 };
    return WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),
                     line.data(),
                     static_cast<DWORD>(line.size()),
                     &written,
                     nullptr)
               ? 0
               : 1;
}
