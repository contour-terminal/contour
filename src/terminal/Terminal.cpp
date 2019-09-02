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
using namespace std::placeholders;

namespace terminal {

Terminal::Terminal(WindowSize _winSize, Logger _logger, Hook _onScreenCommands)
  : PseudoTerminal{ _winSize },
    logger_{move(_logger)},
    inputGenerator_{},
    screen_{
        _winSize.columns,
        _winSize.rows,
        bind(&Terminal::onScreenReply, this, _1),
        [this](auto const& msg) { logger_(msg); },
        bind(&Terminal::onScreenCommands, this, _1)
    },
    onScreenCommands_{ move(_onScreenCommands) },
    screenUpdateThread_{ bind(&Terminal::screenUpdateThread, this) }
{
}

void Terminal::onScreenReply(std::string_view const& reply)
{
    write(reply.data(), reply.size());
}

void Terminal::onScreenCommands(std::vector<Command> const& commands)
{
    // Screen output commands be here - anything this terminal is interested in?
    if (onScreenCommands_)
        onScreenCommands_(commands);
}

void Terminal::screenUpdateThread()
{
    for (;;)
    {
        char buf[4096];
        if (size_t const n = read(buf, sizeof(buf)); n > 0)
        {
            //log("outputThread.data: {}", terminal::escape(buf, buf + n));
            lock_guard<mutex> _l{ screenLock_ };
            screen_.write(buf, n);
        }
    }
}

bool Terminal::send(char32_t _characterEvent, Modifier _modifier)
{
    bool const success = inputGenerator_.generate(_characterEvent, _modifier);
    flushInput();
    return success;
}

bool Terminal::send(Key _key, Modifier _modifier)
{
    bool const success = inputGenerator_.generate(_key, _modifier);
    flushInput();
    return success;
}

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);

    for (auto const& seq : pendingInput_)
        write(seq.data(), seq.size());

    pendingInput_.clear();
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    lock_guard<mutex> _l{ screenLock_ };
    screen_.write(data, size);
}

void Terminal::render(Screen::Renderer const& renderer) const
{
    lock_guard<mutex> _l{ screenLock_ };
    screen_.render(renderer);
}

void Terminal::wait()
{
    screenUpdateThread_.join();
}

}  // namespace terminal
