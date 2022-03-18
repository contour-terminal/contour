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

#include <terminal/MockTerm.h>
#include <terminal/Screen.h>
#include <terminal/pty/MockPty.h>

#include <crispy/App.h>

#include <cstdlib>

namespace terminal
{

MockTerm::MockTerm(PageSize pageSize, LineCount maxHistoryLineCount):
    terminal { std::make_unique<MockPty>(pageSize),
               1024, // pty read buffer size
               *this,
               maxHistoryLineCount }
{
    char const* logFilterString = getenv("LOG");
    if (logFilterString)
    {
        logstore::configure(logFilterString);
        crispy::App::customizeLogStoreOutput();
    }

    // TODO: same as in Terminal's ctor
    screen().setMode(DECMode::AutoWrap, true);
    screen().setMode(DECMode::TextReflow, true);
    screen().setMode(DECMode::SixelCursorNextToGraphic, terminal.state().sixelCursorConformance);
}

} // namespace terminal
