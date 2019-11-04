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
#include <terminal_view/TerminalView.h>
#include <terminal_view/GLLogger.h>
#include <terminal_view/FontManager.h>

#include <terminal/Util.h>

#include <ground/overloaded.h>
#include <ground/UTF8.h>

#include <GL/glew.h>
#include <glm/glm.hpp>

#include <array>
#include <chrono>
#include <iostream>
#include <utility>

using namespace std;
using namespace std::placeholders;

namespace terminal::view {

auto const envvars = terminal::Process::Environment{
    {"TERM", "xterm-256color"},
    {"COLORTERM", "xterm"},
    {"COLORFGBG", "15;0"},
    {"LINES", ""},
    {"COLUMNS", ""},
    {"TERMCAP", ""}
};

inline glm::vec4 makeColor(terminal::RGBColor const& _rgb, terminal::Opacity _opacity = terminal::Opacity::Opaque)
{
    return glm::vec4{
        _rgb.red / 255.0,
        _rgb.green / 255.0,
        _rgb.blue / 255.0,
        static_cast<unsigned>(_opacity) / 255.0
    };
}

TerminalView::TerminalView(std::chrono::steady_clock::time_point _now,
                           WindowSize const& _winSize,
                           optional<size_t> _maxHistoryLineCount,
                           std::string const& _wordDelimiters,
                           Font& _regularFont,
                           CursorShape _cursorShape, // TODO: remember !
						   CursorDisplay _cursorDisplay,
                           terminal::ColorProfile const& _colorProfile,
                           terminal::Opacity _backgroundOpacity,
                           string const& _shell,
                           glm::mat4 const& _projectionMatrix,
                           function<void()> _onScreenUpdate,
                           function<void()> _onWindowTitleChanged,
                           function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                           GLLogger& _logger) :
    logger_{ _logger },
    renderer_{
        _logger,
        _regularFont,
        _colorProfile,
        _backgroundOpacity,
        _projectionMatrix
    },
    process_{
        _shell,
        {_shell},
        envvars,
        _winSize,
        move(_maxHistoryLineCount),
        move(_onWindowTitleChanged),
        move(_resizeWindow),
		_now,
        _wordDelimiters,
        _cursorDisplay,
        _cursorShape,
        [this](terminal::LogEvent const& _event) { logger_(_event); },
        [_onScreenUpdate](auto const& /*_commands*/) { if (_onScreenUpdate) _onScreenUpdate(); }
    }
{
}

bool TerminalView::alive() const
{
    return process_.alive();
}

void TerminalView::resize(unsigned _width, unsigned _height)
{
    auto const newSize = terminal::WindowSize{
        static_cast<unsigned short>(_width / renderer_.cellWidth()),
        static_cast<unsigned short>(_height / renderer_.cellHeight())
    };

    WindowSize const oldSize = process_.screenSize();
    bool const doResize = newSize != oldSize; // process_.size();
    if (doResize)
        process_.resizeScreen(newSize);

    if (doResize)
    {
        terminal().clearSelection();

        cout << fmt::format(
            "Resized to {}x{} ({}x{}) (CharBox: {}x{})\n",
            newSize.columns, newSize.rows,
            _width, _height,
            renderer_.cellWidth(), renderer_.cellHeight()
        );
    }
}

void TerminalView::setCursorShape(CursorShape _shape)
{
    terminal().setCursorShape(_shape);
}

bool TerminalView::setTerminalSize(terminal::WindowSize const& _newSize)
{
    if (process_.terminal().screenSize() == _newSize)
        return false;

    process_.terminal().resizeScreen(_newSize);
    return true;
}


void TerminalView::render(chrono::steady_clock::time_point const& _now)
{
    renderer_.render(process_.terminal(), _now);
}

void TerminalView::wait()
{
    if (!process_.alive())
        return;

    process_.terminal().device().close();
    (void) process_.waitForExit();
}

} // namespace terminal::view
