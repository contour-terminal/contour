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
#pragma once

#include <terminal/Commands.h>
#include <terminal/InputGenerator.h>
#include <terminal/PseudoTerminal.h>
#include <terminal/Screen.h>

#include <fmt/format.h>

#include <functional>
#include <string_view>
#include <thread>
#include <vector>

namespace terminal {

/// Terminal API to manage keyboard and screen of a pseudo terminal.
///
/// With a terminal being attached to a Process, the terminal's screen
/// gets updated according to the process' outputted text,
/// whereas input to the process can be send high-level via the various
/// send(...) member functions.
class Terminal : public PseudoTerminal {
  public:
    using Logger = std::function<void(std::string_view const& message)>;
    using Hook = std::function<void(std::vector<Command> const& commands)>;

    Terminal(WindowSize _winSize, Logger _logger, Hook _onScreenCommands = {});

    // API for keyboard input handling
    bool send(wchar_t _characterEvent, Modifier _modifier);
    bool send(Key _key, Modifier _modifier);
    //TODO: bool send(MouseButtonEvent _mouseButton, Modifier _modifier);
    //TODO: bool send(MouseMoveEvent _mouseMove);
    //TODO: void send(Signal _signalNumber);

    // write to screen
    void writeToScreen(char const* data, size_t size);

    /// @returns const-reference screen of this terminal.
    Screen const& screen() const noexcept { return screen_; }

    /// Waits until process screen update thread has terminated.
    void join();

  private:
    void flushInput();
    void screenUpdateThread();
    void onScreenReply(std::string_view const& reply);
    void onScreenCommands(std::vector<Command> const& commands);

  private:
    Logger const logger_;
    InputGenerator inputGenerator_;
    InputGenerator::SequenceList pendingInput_;
    Screen screen_;
    Screen::Hook onScreenCommands_;
    std::thread screenUpdateThread_;
};

}  // namespace terminal
