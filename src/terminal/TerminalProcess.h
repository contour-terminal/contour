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
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <functional>
#include <thread>

namespace terminal {

// Process with a fully featured Terminal.
class TerminalProcess : public Terminal, public Process {
  public:
    TerminalProcess(
		const std::string& _path,
		std::vector<std::string> const& _args,
		Environment const& _env,
        WindowSize _winSize,
        std::optional<size_t> _maxHistoryLineCount,
        std::chrono::milliseconds _cursorBlinkInterval,
        std::function<void()> _changeWindowTitleCallback,
        std::function<void(unsigned int, unsigned int, bool)> _resizeWindow,
        std::function<RGBColor(DynamicColorName)> _requestDynamicColor,
        std::function<void(DynamicColorName)> _resetDynamicColor,
        std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor,
        std::chrono::steady_clock::time_point _now,
        std::string const& _wordDelimiters,
        std::function<void()> _onSelectionComplete,
        Screen::OnBufferChanged _onScreenBufferChanged,
        std::function<void()> _bell,
        CursorDisplay _cursorDisplay,
        CursorShape _cursorShape,
        Hook _onScreenCommands,
        std::function<void()> _onTerminalClosed,
        Logger _logger
    );

    ~TerminalProcess();

    Terminal& terminal() noexcept { return *this; }
    Terminal const& terminal() const noexcept { return *this; }
};

} // namespace terminal
