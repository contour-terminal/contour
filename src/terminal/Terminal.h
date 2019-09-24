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
#include <terminal/Logger.h>
#include <terminal/InputGenerator.h>
#include <terminal/PseudoTerminal.h>
#include <terminal/Screen.h>

#include <fmt/format.h>

#include <functional>
#include <mutex>
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
    using Hook = std::function<void(std::vector<Command> const& commands)>;

    explicit Terminal(WindowSize _winSize, Logger _logger = {}, Hook _onScreenCommands = {});
    ~Terminal() override;

    // Keyboard input handling
    bool send(char32_t _characterEvent, Modifier _modifier = Modifier::None);
    bool send(Key _key, Modifier _modifier = Modifier::None);
    //TODO: bool send(MouseButtonEvent _mouseButton, Modifier _modifier = Modifier::None);
    //TODO: bool send(MouseMoveEvent _mouseMove = Modifier::None);
    //TODO: void send(Signal _signalNumber = Modifier::None);

    /// Writes a given VT-sequence to screen.
    void writeToScreen(char const* data, size_t size);

    /// Thread-safe access to screen data for rendering
    void render(Screen::Renderer const& renderer) const;

    using Cursor = Screen::Cursor; //TODO: CursorShape shape;

    /// @returns the current Cursor state.
    Cursor cursor() const;

    /// @returns a screenshot, that is, a VT-sequence reproducing the current screen buffer.
    std::string screenshot() const;

    void resize(WindowSize const& _newWindowSize) override;

    /// Waits until process screen update thread has terminated.
    void wait();

    void setTabWidth(unsigned int _tabWidth);

  private:
    void flushInput();
    void screenUpdateThread();
    void useApplicationCursorKeys(bool _enable);
    void onScreenReply(std::string_view const& reply);
    void onScreenCommands(std::vector<Command> const& commands);

  private:
    Logger logger_;
    InputGenerator inputGenerator_;
    InputGenerator::Sequence pendingInput_;
    Screen screen_;
    Screen::Hook onScreenCommands_;
    std::mutex mutable screenLock_;
    std::thread screenUpdateThread_;
};

}  // namespace terminal
