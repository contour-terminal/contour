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
#include <terminal/Debugger.h>
#include <terminal/Screen.h>

namespace terminal {

Command const* Debugger::nextCommand() const noexcept
{
    if (!queuedCommands_.empty())
        return &queuedCommands_.front();
    else
        return nullptr;
}

void Debugger::step()
{
    if (!queuedCommands_.empty())
    {
        auto const cmd = move(queuedCommands_.front());
        queuedCommands_.pop_front();
        screen_.write(cmd);
        pointer_++;
    }
}

void Debugger::flush()
{
    for (Command const& cmd : queuedCommands_)
        screen_.write(cmd);

    queuedCommands_.clear();
}

} // end namespace
