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
#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <terminal/Screen.h>

#include <fmt/format.h>

#include <functional>
#include <string_view>

namespace terminal {

class Terminal {
  public:
    using Logger = std::function<void(std::string_view const& message)>;
    using Hook = std::function<void(std::vector<Command> const& commands)>;

    Terminal(
        size_t cols,
        size_t rows,
        Screen::Reply reply,
        Logger logger,
        Hook onCommands);

    // write to screen
    void write(char const* data, size_t size);

    Screen const& screen() const noexcept { return screen_; }
    Screen& screen() noexcept { return screen_; }

    // TODO: API for keyboard input handling

  private:
    Logger const logger_;
    Screen screen_;
};

}  // namespace terminal
