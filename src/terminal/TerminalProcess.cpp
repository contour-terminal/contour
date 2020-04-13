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
#include <terminal/TerminalProcess.h>

using namespace terminal;
using namespace std;

TerminalProcess::TerminalProcess(const string& _path,
                                 vector<string> const& _args,
                                 Environment const& _env,
                                 WindowSize _winSize,
                                 optional<size_t> _maxHistoryLineCount,
                                 chrono::milliseconds _cursorBlinkInterval,
                                 function<void()> _changeWindowTitleCallback,
                                 function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                                 function<RGBColor(DynamicColorName)> _requestDynamicColor,
                                 function<void(DynamicColorName)> _resetDynamicColor,
                                 function<void(DynamicColorName, RGBColor const&)> _setDynamicColor,
                                 chrono::steady_clock::time_point _now,
                                 string const& _wordDelimiters,
                                 function<void()> _onSelectionComplete,
                                 Screen::OnBufferChanged _onScreenBufferChanged,
                                 function<void()> _bell,
                                 CursorDisplay _cursorDisplay,
                                 CursorShape _cursorShape,
                                 Hook _onScreenCommands,
                                 function<void()> _onTerminalClosed,
                                 Logger _logger) :
    Terminal(
        _winSize,
        _maxHistoryLineCount,
        _cursorBlinkInterval,
        move(_changeWindowTitleCallback),
        move(_resizeWindow),
        _now,
        move(_logger),
        move(_onScreenCommands),
        move(_onTerminalClosed),
        _wordDelimiters,
        move(_onSelectionComplete),
        move(_onScreenBufferChanged),
        move(_bell),
        move(_requestDynamicColor),
        move(_resetDynamicColor),
        move(_setDynamicColor)
    ),
    Process{_path, _args, _env, terminal().device()}
{
    terminal().setCursorDisplay(_cursorDisplay);
    terminal().setCursorShape(_cursorShape);
}

TerminalProcess::~TerminalProcess()
{
    // Closing the terminal I/O.
    // Maybe the process is still alive, but we need to disconnect from the PTY,
    // so that the Process will be notified via SIGHUP.
    // NB: We MUST close the PTY device before waiting for the process to terminate.
    terminal().device().close();

    // Wait until the process is actually terminated.
    (void) Process::wait();
}
