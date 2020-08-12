/**
 * This file is part of the "libterminal" project
 *   Copyright (c) 2019-2020 Christian Parpart <christian@parpart.family>
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
#include <contour/DebuggerService.h>

#include <contour/Controller.h>
#include <contour/TerminalWindow.h>

#include <terminal/Commands.h>
#include <terminal/Debugger.h>

#include <terminal_view/TerminalView.h>

#include <iostream>
#include <mutex>
#include <string>

#if defined(__linux__) // debugger
#include <csignal>
#include <cstdio>
#include <termios.h>
#include <unistd.h>
#endif

using std::cin;
using std::cout;
using std::getline;
using std::make_unique;
using std::scoped_lock;
using std::string;

namespace contour {

/*
    Debugger commands: (TODO)

    help                         Prints this help
    step                         Single-steps one VT sequence
    inspect screen cursor        Prints screen cursor information
    inspect screen modes         Prints screen modes
    inspect screen buffer        Prints screen including SGRs
    inspect screen text          Prints screen text only
    inspect render cache         Prints render cache
    inspect glyph metrics TEXT   Prints glyph metrics for given TEXT including fonts used for each glyph
    list windows                 Prints all available windows
    use window N                 Uses given window N for debugging
    quit                         Quits the debugger

 - Just hitting enter repeats last command.
 - Commands can be shortened with their string prefix, such as "p s t" for "print screen text"
 - History via readline?
 - Tab-autocompletion?

*/

namespace
{
    [[maybe_unused]] void setRawInputMode(bool _enable)
    {
#if defined(__linux__)
        struct termios raw;
        tcgetattr(STDIN_FILENO, &raw);

        if (_enable)
            raw.c_lflag &= ~(ECHO | ICANON);
        else
            raw.c_lflag |= ECHO | ICANON;

        tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
#endif
    }
}

DebuggerService* DebuggerService::self_ = nullptr;

DebuggerService::DebuggerService(Controller* _controller)
    : controller_{ _controller }
{
    self_ = this;

#if defined(__linux__)
    signal(SIGINT, &DebuggerService::onSigInt);
#endif
}

DebuggerService::~DebuggerService()
{
#if defined(__linux__)
    signal(SIGINT, SIG_DFL);
#endif
}

void DebuggerService::onSigInt(int)
{
#if defined(__linux__)
    signal(SIGINT, SIG_DFL);
    self_->debuggerThread_ = make_unique<std::thread>([]() {
        self_->main();
        signal(SIGINT, &DebuggerService::onSigInt);
    });
    self_->debuggerThread_->detach();
#endif
}

std::string DebuggerService::getInput()
{
    string buf;
    getline(cin, buf);

    if (buf.empty())
        buf = lastCommand_;
    else
        lastCommand_ = buf;

    return buf;
}

void DebuggerService::main()
{
    if (controller_->terminalWindows().empty())
        return;

    cout << "Starting debugger.\n";

    auto window = controller_->terminalWindows().front(); // TODO: command for picking window
    terminal::view::TerminalView* view = window->view();
    terminal::Terminal& terminal = view->terminal();
    terminal::Screen& screen = terminal.screen();

    {
        auto _l = scoped_lock{terminal};
        screen.setDebugging(true);
    }

    terminal::Debugger& debugger = *screen.debugger();

    if (auto cmd = screen.debugger()->nextCommand(); cmd != nullptr)
    {
        auto const text = terminal::to_mnemonic(*cmd, true, true);
        cout << fmt::format("Next instruction: {}\n", text);
    }

    bool flushed = true;

    for (bool quit = false; !quit; )
    {
        auto const input = getInput();
        if (input.size() != 1)
            continue;

        switch (input[0])
        {
            case 'q': // quit
                quit = true;
                break;
            case 'f': // flush
            case 'c': // continue
            {
                auto _l = scoped_lock{terminal};
                while (debugger.nextCommand())
                {
                    auto const text = terminal::to_mnemonic(*debugger.nextCommand(), true, true);
                    cout << fmt::format("{}: Flushing instruction: {}\n", debugger.pointer(), text);
                    debugger.step();
                }
                flushed = true;
                break;
            }
            case 'n': // next
            case 's': // step
            {
                auto _l = scoped_lock{terminal};
                if (flushed)
                {
                    flushed = false;
                    if (auto cmd = debugger.nextCommand(); cmd != nullptr)
                    {
                        auto const text = terminal::to_mnemonic(*cmd, true, true);
                        cout << fmt::format("{}: Current instruction: {}\n", debugger.pointer(), text);
                        break;
                    }
                }
                debugger.step();
                if (auto cmd = debugger.nextCommand(); cmd != nullptr)
                {
                    auto const text = terminal::to_mnemonic(*cmd, true, true);
                    cout << fmt::format("{}: Next instruction: {}\n", debugger.pointer(), text);
                }
                else
                    cout << fmt::format("No next instruction pending.\n");
                break;
            }
            case 'i': // inspect state
            {
                auto _l = scoped_lock{terminal};
                screen.dumpState();
                break;
            }
            case '?': // help
            case 'h': // help
                cout << "Available commands:\n"
                        "  (s)tep to next instruction\n"
                        "  (c)ontinue until next event (<LF>?)\n"
                        "  (i)nspect current state\n"
                        "  (h)elp\n"
                        "  (q)uit\n"
                        "\n";
                break;
            default:
                break;
        }
    }

    cout << "Stopping debugger.\n";
    {
        auto _l = scoped_lock{terminal};
        screen.setDebugging(false);
    }
}

} // end namespace
