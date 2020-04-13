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
#include <terminal/TerminalProcess.h>
#include <terminal/Logger.h>
#include <terminal/WindowSize.h>

#include <terminal_view/FontManager.h>
#include <terminal_view/GLCursor.h>
#include <terminal_view/GLTextShaper.h>
#include <terminal_view/GLRenderer.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace terminal::view {

class Font;
struct ShaderConfig;

/// OpenGL-Terminal Object.
class TerminalView {
  public:
    TerminalView(std::chrono::steady_clock::time_point _now,
                 terminal::WindowSize const& _winSize,
                 std::optional<size_t> _maxHistoryLineCount,
                 std::string const& _wordDelimiters,
                 std::function<void()> _onSelectionComplete,
                 Screen::OnBufferChanged _onScreenBufferChanged,
                 std::function<void()> _bell,
                 Font& _regularFont,
                 CursorShape _cursorShape,
                 CursorDisplay _cursorDisplay,
                 std::chrono::milliseconds _cursorBlinkInterval,
                 terminal::ColorProfile _colorProfile,
                 terminal::Opacity _backgroundOpacity,
                 std::string const& _shell,
                 terminal::Process::Environment const& _env,
                 QMatrix4x4 const& _projectionMatrix,
                 std::function<void()> _onScreenUpdate,
                 std::function<void()> _onWindowTitleChanged,
                 std::function<void(unsigned int, unsigned int, bool)> _resizeWindow,
                 std::function<void()> _onTerminalClosed,
                 ShaderConfig const& _backgroundShaderConfig,
                 ShaderConfig const& _textShaderConfig,
                 ShaderConfig const& _cursorShaderConfig,
                 Logger _logger);

    TerminalView(TerminalView const&) = delete;
    TerminalView(TerminalView&&) = delete;
    TerminalView& operator=(TerminalView const&) = delete;
    TerminalView& operator=(TerminalView&&) = delete;
    ~TerminalView() = default;

    size_t cellHeight() const noexcept { return renderer_.cellHeight(); }
    size_t cellWidth() const noexcept { return renderer_.cellWidth(); }

    /// Resizes the terminal view to the given number of pixels.
    ///
    /// It also computes the appropricate number of text lines and character columns
    /// and resizes the internal screen buffer as well as informs the connected
    /// PTY slave about the window resize event.
    void resize(unsigned _width, unsigned _height);

    void setFont(Font& _font) { renderer_.setFont(_font); }
    bool setFontSize(unsigned int _fontSize) { return renderer_.setFontSize(_fontSize); }
    bool setTerminalSize(WindowSize const& _newSize);
    void setCursorShape(CursorShape _shape);
    void setBackgroundOpacity(terminal::Opacity _opacity) { renderer_.setBackgroundOpacity(_opacity); }
    void setProjection(QMatrix4x4 const& _projectionMatrix) { return renderer_.setProjection(_projectionMatrix); }

    /// Renders the screen buffer to the current OpenGL screen.
    uint64_t render(std::chrono::steady_clock::time_point const& _now);

    /// Checks if there is still a slave connected to the PTY.
    bool alive() const;

    /// Waits until the PTY slave has terminated, and then closes the underlying terminal.
    ///
    /// The alive() test will fail after this call.
    void wait();

    Process const& process() const noexcept { return process_; }
    Process& process() noexcept { return process_; }
    Terminal const& terminal() const noexcept { return process_.terminal(); }
    Terminal& terminal() noexcept { return process_.terminal(); }

    GLRenderer const& renderer() const { return renderer_; }

    void setColorProfile(terminal::ColorProfile const& _colors);

    RGBColor requestDynamicColor(DynamicColorName _name);
    void resetDynamicColor(DynamicColorName _name);
    void setDynamicColor(DynamicColorName _name, RGBColor const& value);

  private:
    Logger logger_;
    GLRenderer renderer_;
    TerminalProcess process_;
    ColorProfile colorProfile_;
    ColorProfile defaultColorProfile_;
};

class GLTextShaper;

void render(TerminalView const& _terminalView,
            std::chrono::steady_clock::time_point _now,
            GLTextShaper& _regularFont,
            CursorShape _cursorShape,
            terminal::ColorProfile const& _colorProfile,
            terminal::Opacity _backgroundOpacity,
            QMatrix4x4 const& _projectionMatrix,
            Logger _logger);

} // namespace terminal::view
