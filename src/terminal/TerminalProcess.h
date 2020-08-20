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
#include <terminal/Terminal.h>
#include <terminal/Process.h>
#include <functional>
#include <thread>

namespace terminal {

// Process with a fully featured Terminal.
class TerminalProcess : public Terminal, public Process {
  public:
    TerminalProcess(
        Process::ExecInfo const& _shell,
        Size _winSize,
        Terminal::Events& _eventListener,
        std::optional<size_t> _maxHistoryLineCount,
        std::chrono::milliseconds _cursorBlinkInterval,
        std::chrono::steady_clock::time_point _now,
        std::string const& _wordDelimiters,
        CursorDisplay _cursorDisplay,
        CursorShape _cursorShape,
        Logger _logger
    );

    ~TerminalProcess();

    Terminal& terminal() noexcept { return *this; }
    Terminal const& terminal() const noexcept { return *this; }
};

} // namespace terminal
