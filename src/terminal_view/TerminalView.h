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
#include <terminal/Selector.h>
#include <terminal/Terminal.h>
#include <terminal/WindowSize.h>

#include <terminal_view/CellBackground.h>
#include <terminal_view/FontManager.h>
#include <terminal_view/GLCursor.h>
#include <terminal_view/GLLogger.h>
#include <terminal_view/GLTextShaper.h>

#include <glm/matrix.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace terminal::view {

class Font;

/// OpenGL-Terminal Object.
class TerminalView {
  public:
    TerminalView(terminal::WindowSize const& _winSize,
                 std::optional<size_t> _maxHistoryLineCount,
                 std::string const& _wordDelimiters,
                 Font& _regularFont,
                 CursorShape _cursorShape,
				 bool _cursorBlinking,
                 terminal::ColorProfile const& _colorProfile,
                 terminal::Opacity _backgroundOpacity,
                 std::string const& _shell,
                 glm::mat4 const& _projectionMatrix,
                 std::function<void()> _onScreenUpdate,
                 std::function<void()> _onWindowTitleChanged,
                 std::function<void(unsigned int, unsigned int, bool)> _resizeWindow,
				 std::function<void(std::function<void()>)> _post,
                 GLLogger& _logger);

    TerminalView(TerminalView const&) = delete;
    TerminalView(TerminalView&&) = delete;
    TerminalView& operator=(TerminalView const&) = delete;
    TerminalView& operator=(TerminalView&&) = delete;

    ~TerminalView();

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);

    bool send(terminal::InputEvent const& _inputEvent, std::chrono::steady_clock::time_point _now);

    /// Sends verbatim text to application.
    void sendPaste(std::string_view const& _text);

    void writeToScreen(std::string_view const& _text);

    /// Takes a screenshot of the current screen buffer in VT sequence format.
    std::string screenshot() const;

    /// Resizes the terminal view to the given number of pixels.
    ///
    /// It also computes the appropricate number of text lines and character columns
    /// and resizes the internal screen buffer as well as informs the connected
    /// PTY slave about the window resize event.
    void resize(unsigned _width, unsigned _height);

    Font const& regularFont() const noexcept { return regularFont_.get(); }

    void setWordDelimiters(std::string const& _wordDelimiters);
    void setFont(Font& _font);
    bool setFontSize(unsigned int _fontSize);
    bool setTerminalSize(terminal::WindowSize const& _newSize);

	std::chrono::duration<double, std::milli> cursorBlinkInterval() const noexcept
	{
		return std::chrono::duration<double, std::milli>{500.0};
	}

	void setCursorColor(terminal::RGBColor const& _color);
	void setCursorShape(CursorShape _shape);
	void setCursorBlinking(bool _blinking);

    /// Sets the projection matrix used for translating rendering coordinates.
    void setProjection(glm::mat4 const& _projectionMatrix);

    /// Checks if a render() method should be called by checking the dirty bit,
    /// and if so, clears the dirty bit and returns true, false otherwise.
    bool shouldRender(std::chrono::steady_clock::time_point const& _now);

    /// Renders the screen buffer to the current OpenGL screen.
    void render(std::chrono::steady_clock::time_point const& _now);

    /// Renders only the selected text.
    void renderSelection(terminal::Screen::Renderer _render) const;

    /// Clears selection, if any currently present.
    void clearSelection();

    /// Checks if there is still a slave connected to the PTY.
    bool alive() const;

    /// Waits until the PTY slave has terminated, and then closes the underlying terminal.
    ///
    /// The alive() test will fail after this call.
    void wait();

    terminal::ColorProfile const& colorProfile() const noexcept { return colorProfile_; }
    void setTabWidth(unsigned int _tabWidth);
    void setBackgroundOpacity(terminal::Opacity _opacity);

    std::string const& windowTitle() const noexcept { return terminal_.windowTitle(); }

    size_t scrollOffset() const noexcept { return scrollOffset_; }
    bool scrollUp(size_t _numLines);
    bool scrollDown(size_t _numLines);
    bool scrollToTop();
    bool scrollToBottom();

	terminal::Process const& process() const noexcept { return process_; }

  private:
    using cursor_pos_t = terminal::cursor_pos_t;
    using RGBColor = terminal::RGBColor;
    using GraphicsAttributes = terminal::ScreenBuffer::GraphicsAttributes;
    using Cell = terminal::ScreenBuffer::Cell;

    /// Renders and then clears current cell group if current @p _cell cannot be appended, or appends to current cell group otherwise.
    void fillCellGroup(cursor_pos_t _row, cursor_pos_t _col, Cell const& _cell);
    void renderCellGroup();
    void onScreenUpdateHook(std::vector<terminal::Command> const& _commands);

	void onSetCursorStyle(terminal::CursorDisplay _display, terminal::CursorStyle _style);

    glm::ivec2 makeCoords(cursor_pos_t col, cursor_pos_t row) const;
    std::pair<glm::vec4, glm::vec4> makeColors(GraphicsAttributes const& _attributes) const;

    /// @returns the coordinates with origin being at the top of the history.
    terminal::Coordinate absoluteCoordinate(terminal::Coordinate _viewportCoordinate) const noexcept;

    /// Tests whether given absolute line number [1..num] is within scrolling region
    bool isAbsoluteLineVisible(cursor_pos_t _row) const noexcept;

    std::vector<terminal::Selector::Range> selection() const;

	void post(std::function<void()> _fn)
	{
		if (post_)
			post_(move(_fn));
	}

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

    std::u32string wordDelimiters_;
    std::reference_wrapper<Font> regularFont_;
    GLTextShaper textShaper_;
    CellBackground cellBackground_;

    GLCursor cursor_;
	bool cursorBlinking_;
	unsigned cursorBlinkState_;
	std::chrono::steady_clock::time_point lastCursorBlink_;

    terminal::Terminal terminal_;
    terminal::Process process_;
    std::thread processExitWatcher_;

    std::function<void()> onScreenUpdate_;

    size_t scrollOffset_ = 0;
    terminal::Coordinate currentMousePosition_{0, 0};

    std::unique_ptr<terminal::Selector> selector_;

    // helpers for detecting double/tripple clicks
    std::chrono::steady_clock::time_point lastClick_{};
    unsigned int speedClicks_ = 0;

	std::function<void(std::function<void()>)> post_;
};

} // namespace terminal::view
