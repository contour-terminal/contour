// This file is part of the "libterminal" project, http://github.com/christianparpart/libterminal>
//   (c) 2018 Christian Parpart <christian@parpart.family>
//
// Licensed under the MIT License (the "License"); you may not use this
// file except in compliance with the License. You may obtain a copy of
// the License at: http://opensource.org/licenses/MIT

#include <terminal/Terminal.h>

#include <terminal/Generator.h>
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
