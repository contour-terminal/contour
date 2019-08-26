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

#include <terminal/OutputGenerator.h>
#include <terminal/Util.h>

using namespace std;

namespace terminal {

Terminal::Terminal(size_t cols,
                   size_t rows,
                   Screen::Reply reply,
                   Logger logger,
                   Hook onCommands)
  : logger_{move(logger)},
    screen_{
        cols,
        rows,
        move(reply),
        [this](auto const& msg) { logger_(msg); },
        move(onCommands)
    }
{
}

void Terminal::write(char const* data, size_t size)
{
    screen_.write(data, size);
}

}  // namespace terminal
