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

#include <terminal/Color.h>
#include <terminal/Process.h>
#include <terminal/Terminal.h>
#include <terminal/WindowSize.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

#include <glterminal/CellBackground.h>
#include <glterminal/FontManager.h>
#include <glterminal/GLCursor.h>
#include <glterminal/GLLogger.h>
#include <glterminal/GLTextShaper.h>

#include <glm/matrix.hpp>

class Font;

/// OpenGL-Terminal Object.
class GLTerminal {
  public:
    GLTerminal(terminal::WindowSize const& _winSize,
               unsigned _width, unsigned _height,
               Font& _fontFamily,
               CursorShape _cursorShape,
               glm::vec3 const& _cursorColor,
               terminal::ColorProfile const& _colorProfile,
               terminal::Opacity _backgroundOpacity,
               std::string const& _shell,
               glm::mat4 const& _projectionMatrix,
               std::function<void()> _onScreenUpdate,
               GLLogger& _logger);

    GLTerminal(GLTerminal const&) = delete;
    GLTerminal(GLTerminal&&) = delete;
    GLTerminal& operator=(GLTerminal const&) = delete;
    GLTerminal& operator=(GLTerminal&&) = delete;

    ~GLTerminal();

    bool send(char32_t _characterEvent, terminal::Modifier _modifier);
    bool send(terminal::Key _key, terminal::Modifier _modifier);

    /// Takes a screenshot of the current screen buffer in VT sequence format.
    std::string screenshot() const;

    /// Resizes the terminal view to the given number of pixels.
    ///
    /// It also computes the appropricate number of text lines and character columns
    /// and resizes the internal screen buffer as well as informs the connected
    /// PTY slave about the window resize event.
    void resize(unsigned _width, unsigned _height);

    bool setFontSize(unsigned int _fontSize);
    bool setTerminalSize(terminal::WindowSize const& _newSize);

    /// Sets the projection matrix used for translating rendering coordinates.
    void setProjection(glm::mat4 const& _projectionMatrix);

    /// Checks if a render() method should be called by checking the dirty bit,
    /// and if so, clears the dirty bit and returns true, false otherwise.
    bool shouldRender();

    /// Renders the screen buffer to the current OpenGL screen.
    void render();

    /// Checks if there is still a slave connected to the PTY.
    bool alive() const;

    /// Waits until the PTY slave has terminated, and then closes the underlying terminal.
    ///
    /// The alive() test will fail after this call.
    void wait();

    terminal::ColorProfile const& colorProfile() const noexcept { return colorProfile_; }
    void setTabWidth(unsigned int _tabWidth);
    void setBackgroundOpacity(terminal::Opacity _opacity);

  private:
    using cursor_pos_t = terminal::cursor_pos_t;
    using RGBColor = terminal::RGBColor;
    using GraphicsAttributes = terminal::Screen::GraphicsAttributes;
    using Cell = terminal::Screen::Cell;

    /// Renders and then clears current cell group if current @p _cell cannot be appended, or appends to current cell group otherwise.
    void fillCellGroup(cursor_pos_t _row, cursor_pos_t _col, Cell const& _cell);
    void renderCellGroup();
    void onScreenUpdateHook(std::vector<terminal::Command> const& _commands);

    glm::ivec2 makeCoords(cursor_pos_t col, cursor_pos_t row) const;
    std::pair<glm::vec4, glm::vec4> makeColors(GraphicsAttributes const& _attributes) const;

  private:
    bool alive_ = true;

    /// Holds an array of directly connected characters on a single line that all share the same visual attributes.
    struct PendingDraw {
        cursor_pos_t lineNumber{};
        cursor_pos_t startColumn{};
        GraphicsAttributes attributes{};
        std::vector<char32_t> text{};

        void reset(cursor_pos_t _row, cursor_pos_t _col, GraphicsAttributes const& _attributes, char32_t _char)
        {
            lineNumber = _row;
            startColumn = _col;
            attributes = _attributes;
            text.clear();
            text.push_back(_char);
        }
    };
    PendingDraw pendingDraw_;

    struct Margin {
        unsigned left{};
        unsigned bottom{};
    };
    Margin margin_{};

    GLLogger& logger_;

    /// Boolean, indicating whether the terminal's screen buffer contains updates to be rendered.
    std::atomic<bool> updated_;

    terminal::ColorProfile const& colorProfile_;
    terminal::Opacity backgroundOpacity_;

    Font& regularFont_;
    GLTextShaper textShaper_;
    CellBackground cellBackground_;
    GLCursor cursor_;

    terminal::Terminal terminal_;
    terminal::Process process_;
    std::thread processExitWatcher_;

    std::function<void()> onScreenUpdate_;
};
