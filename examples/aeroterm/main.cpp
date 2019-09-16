/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019 Christian Parpart <christian@parpart.family>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "AeroTerminal.h"
#include "GLLogger.h"
#include "Flags.h"

#include <terminal/InputGenerator.h>
#include <terminal/OutputGenerator.h>
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <terminal/UTF8.h>
#include <terminal/Util.h>

#include <iostream>

// TODOs:
// - [x] proper termination (window close as well as process exit)
// - [x] input: rename Numpad_Dot to Numpad_Decimal, and others (Div -> Divide, etc)
// - [x] Fix window-resize: call Screen::resize(), PseudoTerminal::updateWindowSize()
// - [x] logging: runtime-configurable logging (to file or stdout, differ between error/warn/debug/trace logging)
// - [x] Hi-DPI support (hm, because I need it)
// - [x] font (fontconfig) loading on Linux
// - [ ] Fix font size on non-Hi-DPI screens (such as my Linux monitor)
// - [ ] show cursor (in correct shapes, with blinking)
//   - [x] CursorShape: Block
//   - [x] Screen: HideCursor / ShowCursor
//   - [ ] CursorShape: Beam
//   - [ ] CursorShape: Underline
//   - [ ] Blinking Mode
// - [ ] other SGRs (bold, italic, etc)
// - [ ] basic runtime-reloadable config file (yaml?)
// - [ ] Windowed fullscreen support (ALT+ENTER, or similar)
// - [ ] input: F13..F25
// - [ ] input: GLFW_KEY_PRINT_SCREEN
// - [ ] input: GLFW_KEY_PAUSE

#if defined(__unix__)
#include <unistd.h>
#endif

using namespace std;

CursorShape makeCursorShape(string const& _name)
{
    if (_name == "block")
        return CursorShape::Block;

    if (_name == "underscore")
        return CursorShape::Underscore;

    if (_name == "beam")
        return CursorShape::Beam;

    throw runtime_error("Invalid cursor shape. Use one of block, underscore, beam.");
}

int main(int argc, char const* argv[])
{
    try
    {
        util::Flags flags;
        flags.defineBool("help", 'h', "Shows this help and quits.");
        flags.defineBool("log-parser-error", 0, "Enables logging of parser errorrs.");
        flags.defineBool("log-raw-input", 0, "Enables logging of raw input.");
        flags.defineBool("log-raw-output", 0, "Enables logging of raw output.");
        flags.defineBool("log-invalid-output", 0, "Enables logging of invalid output sequences.");
        flags.defineBool("log-unsupported-output", 0, "Enables logging of unsupported output sequences.");
        flags.defineBool("log-trace-output", 0, "Enables logging of output trace.");
        flags.defineNumber("font-size", 'S', "PIXELS", "Defines character font-size.", 12);
        flags.defineNumber("columns", 'C', "COUNT", "Defines number of text columns.", 130);
        flags.defineNumber("lines", 'L', "COUNT", "Defines number of text lines.", 25);
        flags.defineString("font", 'F', "PATTERN", "Defines font family.", "Fira Code, Ubuntu Mono, Consolas, monospace");
        flags.defineString("cursor-shape", 'P', "SHAPE", "Defines cursor shape.", "block");
        flags.defineString("shell", 's', "SHELL", "Defines shell to invoke.", terminal::Process::loginShell());
        flags.defineFloat("background-red", 'r', "PCT", "Background red color", 0.0f);
        flags.defineFloat("background-green", 'g', "PCT", "Background red color", 0.0f);
        flags.defineFloat("background-blue", 'b', "PCT", "Background red color", 0.0f);
        flags.defineFloat("background-transparency", 'T', "PCT", "Defines background transparency.", 1.0f);
        flags.defineBool("background-blur", 'A', "Enable background blur.");

        flags.parse(argc, argv);

        LogMask const logMask = [&]() {
            LogMask mask{};
            if (flags.getBool("log-parser-error"))
                mask |= LogMask::ParserError;

            if (flags.getBool("log-invalid-output"))
                mask |= LogMask::InvalidOutput;

            if (flags.getBool("log-unsupported-output"))
                mask |= LogMask::UnsupportedOutput;

            if (flags.getBool("log-raw-input"))
                mask |= LogMask::RawInput;

            if (flags.getBool("log-raw-output"))
                mask |= LogMask::RawOutput;

            if (flags.getBool("log-trace-output"))
                mask |= LogMask::TraceOutput;

            return mask;
        }();

        if (flags.getBool("help"))
        {
            cout << "Aero Terminal Emulator.\n"
                 << "\n"
                 << "Usage:\n"
                 << "  aeroterm [OPTIONS ...]\n"
                 << "\n"
                 << flags.helpText() << endl;
            return EXIT_SUCCESS;
        }

        auto const cursorColor = glm::vec3{ 0.6, 0.6, 0.6 };

        auto const backgroundColor = glm::vec4{
            flags.getFloat("background-red"),
            flags.getFloat("background-green"),
            flags.getFloat("background-blue"),
            flags.getFloat("background-transparency")
        };

        auto myterm = AeroTerminal{
            terminal::WindowSize{
                static_cast<unsigned short>(flags.getNumber("columns")),
                static_cast<unsigned short>(flags.getNumber("lines"))
            },
            static_cast<unsigned short>(flags.getNumber("font-size")),
            flags.getString("font"),
            makeCursorShape(flags.getString("cursor-shape")),
            cursorColor,
            backgroundColor,
            flags.getBool("background-blur"),
            flags.getString("shell"),
            logMask
        };
        return myterm.main();
    }
    catch (exception const& e)
    {
        cerr << "Unhandled error caught. " << e.what() << endl;
        return EXIT_FAILURE;
    }
}
