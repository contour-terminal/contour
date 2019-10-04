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

Terminal::Terminal(WindowSize _winSize,
                   std::function<void()> _onWindowTitleChanged,
                   std::function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                   Logger _logger,
                   Hook _onScreenCommands)
  : PseudoTerminal{ _winSize },
    logger_{ _logger },
    inputGenerator_{},
    screen_{
        _winSize,
        bind(&Terminal::useApplicationCursorKeys, this, _1),
        move(_onWindowTitleChanged),
        move(_resizeWindow),
        bind(&InputGenerator::setApplicationKeypadMode, &inputGenerator_, _1),
        bind(&Terminal::onScreenReply, this, _1),
        logger_,
        bind(&Terminal::onScreenCommands, this, _1)
    },
    onScreenCommands_{ move(_onScreenCommands) },
    screenUpdateThread_{ bind(&Terminal::screenUpdateThread, this) }
{
}

Terminal::~Terminal()
{
    //wait();
}

void Terminal::useApplicationCursorKeys(bool _enable)
{
    auto const keyMode = _enable ? KeyMode::Application : KeyMode::Normal;
    inputGenerator_.setCursorKeysMode(keyMode);
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
        if (auto const n = read(buf, sizeof(buf)); n != -1)
        {
            //log("outputThread.data: {}", terminal::escape(buf, buf + n));
            lock_guard<decltype(screenLock_)> _l{ screenLock_ };
            screen_.write(buf, n);
        }
        else
            break;
    }
}

bool Terminal::send(InputEvent _inputEvent)
{
    // Early exit if KAM is enabled.
    if (screen_.isModeEnabled(Mode::KeyboardAction))
        return true;

    bool const success = inputGenerator_.generate(_inputEvent);
    flushInput();
    return success;
}

void Terminal::flushInput()
{
    inputGenerator_.swap(pendingInput_);
    write(pendingInput_.data(), pendingInput_.size());
    logger_(RawInputEvent{escape(begin(pendingInput_), end(pendingInput_))});
    pendingInput_.clear();
}

void Terminal::writeToScreen(char const* data, size_t size)
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    screen_.write(data, size);
}

Terminal::Cursor Terminal::cursor() const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    return screen_.realCursor();
}

string Terminal::screenshot() const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    return screen_.screenshot();
}

void Terminal::render(Screen::Renderer const& renderer) const
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    screen_.render(renderer);
}

void Terminal::resize(WindowSize const& _newWindowSize)
{
    lock_guard<decltype(screenLock_)> _l{ screenLock_ };
    screen_.resize(_newWindowSize);
    PseudoTerminal::resize(_newWindowSize);
}

void Terminal::wait()
{
    screenUpdateThread_.join();
}

void Terminal::setTabWidth(unsigned int _tabWidth)
{
    screen_.setTabWidth(_tabWidth);
}

}  // namespace terminal
