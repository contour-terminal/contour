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
#include <terminal/Commands.h>
#include <terminal/Logger.h>
#include <terminal/OutputHandler.h>
#include <terminal/Parser.h>
#include <terminal/WindowSize.h>

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <list>
#include <optional>
#include <stack>
#include <string>
#include <string_view>
#include <vector>
#include <set>

namespace terminal {

class CharacterStyleMask {
  public:
	enum Mask : uint16_t {
		Bold = (1 << 0),
		Faint = (1 << 1),
		Italic = (1 << 2),
		Underline = (1 << 3),
		Blinking = (1 << 4),
		Inverse = (1 << 5),
		Hidden = (1 << 6),
		CrossedOut = (1 << 7),
		DoublyUnderlined = (1 << 8),
	};

	constexpr CharacterStyleMask() : mask_{} {}
	constexpr CharacterStyleMask(Mask m) : mask_{m} {}
	constexpr CharacterStyleMask(unsigned m) : mask_{m} {}
	constexpr CharacterStyleMask(CharacterStyleMask const& _other) noexcept : mask_{_other.mask_} {}

	constexpr CharacterStyleMask& operator=(CharacterStyleMask const& _other) noexcept
	{
		mask_ = _other.mask_;
		return *this;
	}

	constexpr unsigned mask() const noexcept { return mask_; }

	constexpr operator unsigned () const noexcept { return mask_; }

  private:
	unsigned mask_;
};

std::string to_string(CharacterStyleMask _mask);

constexpr bool operator==(CharacterStyleMask a, CharacterStyleMask b) noexcept
{
	return a.mask() == b.mask();
}

constexpr CharacterStyleMask& operator|=(CharacterStyleMask& a, CharacterStyleMask b) noexcept
{
    a = a | b;
	return a;
}

constexpr CharacterStyleMask& operator&=(CharacterStyleMask& a, CharacterStyleMask b) noexcept
{
    a = a & b;
	return a;
}

constexpr bool operator!(CharacterStyleMask a) noexcept
{
	return a.mask() == 0;
}

struct Margin {
	struct Range {
		unsigned int from;
		unsigned int to;

		constexpr unsigned int length() const noexcept { return to - from + 1; }
		constexpr bool operator==(Range const& rhs) const noexcept { return from == rhs.from && to == rhs.to; }
		constexpr bool operator!=(Range const& rhs) const noexcept { return !(*this == rhs); }

		constexpr bool contains(unsigned int _value) const noexcept { return from <= _value && _value <= to; }
	};

	Range vertical{}; // top-bottom
	Range horizontal{}; // left-right
};

/**
 * Screen Buffer, managing a single screen buffer.
 */
struct ScreenBuffer {
    /// ScreenBuffer's type, such as main screen or alternate screen.
    enum class Type {
        Main,
        Alternate
    };

    /// Character graphics rendition information.
    struct GraphicsAttributes {
        Color foregroundColor{DefaultColor{}};
        Color backgroundColor{DefaultColor{}};
        CharacterStyleMask styles{};
    };

    /// Grid cell with character and graphics rendition information.
    struct Cell {
        char32_t character{};
        GraphicsAttributes attributes{};
    };

	using LineBuffer = std::vector<Cell>;
    struct Line {
        LineBuffer buffer;
        bool marked = false;

        using iterator = LineBuffer::iterator;
        using const_iterator = LineBuffer::const_iterator;
        using reverse_iterator = LineBuffer::reverse_iterator;
        using size_type = LineBuffer::size_type;

        Line(size_t _numCols, Cell const& _defaultCell) : buffer{_numCols, _defaultCell} {}
        Line() = default;
        Line(Line const&) = default;
        Line(Line&&) = default;
        Line& operator=(Line const&) = default;
        Line& operator=(Line&&) = default;

        LineBuffer* operator->() noexcept { return &buffer; }
        LineBuffer const* operator->()  const noexcept { return &buffer; }
        auto& operator[](std::size_t _index) { return buffer[_index]; }
        auto const& operator[](std::size_t _index) const { return buffer[_index]; }
        auto size() const noexcept { return buffer.size(); }
        void resize(size_type _size) { buffer.resize(_size); }

        iterator begin() { return buffer.begin(); }
        iterator end() { return buffer.end(); }
        reverse_iterator rbegin() { return buffer.rbegin(); }
        reverse_iterator rend() { return buffer.rend(); }
        const_iterator cbegin() const { return buffer.cbegin(); }
        const_iterator cend() const { return buffer.cend(); }
    };
	using Lines = std::deque<Line>;

    struct Cursor : public Coordinate {
        bool visible = true;

        Cursor& operator=(Coordinate const& coords) noexcept {
            column = coords.column;
            row = coords.row;
            return *this;
        }
    };

	// Savable states for DECSC & DECRC
	struct SavedState {
		Coordinate cursorPosition;
		GraphicsAttributes graphicsRendition{};
		// TODO: CharacterSet for GL and GR
		bool autowrap = false;
		bool originMode = false;
		// TODO: Selective Erase Attribute (DECSCA)
		// TODO: Any single shift 2 (SS2) or single shift 3 (SS3) functions sent
	};

	ScreenBuffer(Type _type, WindowSize const& _size, std::optional<size_t> _maxHistoryLineCount)
		: type_{ _type },
          size_{ _size },
          maxHistoryLineCount_{ move(_maxHistoryLineCount) },
		  margin_{
			  {1, _size.rows},
			  {1, _size.columns}
		  },
		  lines{ _size.rows, Line{_size.columns, Cell{}} }
	{
		verifyState();
	}

    void reset()
    {
        *this = ScreenBuffer(type_, size_, maxHistoryLineCount_);
    }

    std::optional<size_t> findPrevMarker(size_t _currentScrollOffset) const;
    std::optional<size_t> findNextMarker(size_t _currentScrollOffset) const;

    Type type_;
	WindowSize size_;
    std::optional<size_t> maxHistoryLineCount_;
	Margin margin_;
	std::set<Mode> enabledModes_{};
	Cursor cursor{};
	Lines lines;
	Lines savedLines{};
	bool autoWrap{false};
	bool wrapPending{false};
	bool cursorRestrictedToMargin{false};
	unsigned int tabWidth{8};
    std::vector<cursor_pos_t> tabs;
	GraphicsAttributes graphicsRendition{};
	std::stack<SavedState> savedStates{};

	Lines::iterator currentLine{std::begin(lines)};
	Line::iterator currentColumn{currentLine->begin()};

	void appendChar(char32_t ch);

	// Applies LF but also moves cursor to given column @p _column.
	void linefeed(cursor_pos_t _column);

	void resize(WindowSize const& _winSize);
	WindowSize const& size() const noexcept { return size_; }

	void scrollUp(cursor_pos_t n);
	void scrollUp(cursor_pos_t n, Margin const& margin);
	void scrollDown(cursor_pos_t n);
	void scrollDown(cursor_pos_t n, Margin const& margin);
	void deleteChars(cursor_pos_t _lineNo, cursor_pos_t _n);
	void insertChars(cursor_pos_t _lineNo, cursor_pos_t _n);
	void insertColumns(cursor_pos_t _n);

	void setMode(Mode _mode, bool _enable);

    void clampSavedLines();
	void verifyState() const;
	void saveState();
	void restoreState();
	void updateCursorIterators();

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

	constexpr Coordinate realCursorPosition() const noexcept { return cursor; }

	constexpr Coordinate cursorPosition() const noexcept {
		if (!cursorRestrictedToMargin)
			return realCursorPosition();
		else
			return Coordinate{
				cursor.row - margin_.vertical.from + 1,
				cursor.column - margin_.horizontal.from + 1
			};
	}

	constexpr Coordinate origin() const noexcept {
		if (cursorRestrictedToMargin)
			return {margin_.vertical.from, margin_.horizontal.from};
		else
			return {1, 1};
	}

	Cell& at(cursor_pos_t row, cursor_pos_t col);
	Cell const& at(cursor_pos_t row, cursor_pos_t col) const;

	/// Retrieves the cell at given cursor, respecting origin mode.
	Cell& withOriginAt(cursor_pos_t row, cursor_pos_t col);

	/// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is enabled.
	Coordinate toRealCoordinate(Coordinate const& pos) const noexcept
	{
		if (!cursorRestrictedToMargin)
			return pos;
		else
			return { pos.row + margin_.vertical.from - 1, pos.column + margin_.horizontal.from - 1 };
	}

	/// Clamps given coordinates, respecting DECOM (Origin Mode).
	Coordinate clampCoordinate(Coordinate const& to) const noexcept
	{
		if (!cursorRestrictedToMargin)
			return {
				std::clamp(to.row, cursor_pos_t{ 1 }, size_.rows),
				std::clamp(to.column, cursor_pos_t{ 1 }, size_.columns)
			};
		else
			return {
				std::clamp(to.row, cursor_pos_t{ 1 }, margin_.vertical.to),
				std::clamp(to.column, cursor_pos_t{ 1 }, margin_.horizontal.to)
			};
	}

	void moveCursorTo(Coordinate to);
};

inline auto begin(ScreenBuffer::Line& _line) { return _line.begin(); }
inline auto end(ScreenBuffer::Line& _line) { return _line.end(); }
inline auto begin(ScreenBuffer::Line const& _line) { return _line.cbegin(); }
inline auto end(ScreenBuffer::Line const& _line) { return _line.cend(); }
inline ScreenBuffer::Line::const_iterator cbegin(ScreenBuffer::Line const& _line) { return _line.cbegin(); }
inline ScreenBuffer::Line::const_iterator cend(ScreenBuffer::Line const& _line) { return _line.cend(); }

/**
 * Terminal Screen.
 *
 * Implements the all Command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
class Screen {
  public:
	using Cell = ScreenBuffer::Cell;
	using Cursor = ScreenBuffer::Cursor;
    using Reply = std::function<void(std::string const&)>;
    using Renderer = std::function<void(cursor_pos_t row, cursor_pos_t col, Cell const& cell)>;
    using ModeSwitchCallback = std::function<void(bool)>;
    using ResizeWindowCallback = std::function<void(unsigned int, unsigned int, bool)>;
    using SetApplicationKeypadMode = std::function<void(bool)>;
    using SetBracketedPaste = std::function<void(bool)>;
	using OnSetCursorStyle = std::function<void(CursorDisplay, CursorShape)>;
    using OnBufferChanged = std::function<void(ScreenBuffer::Type)>;
    using Hook = std::function<void(std::vector<Command> const& commands)>;

  public:
    /**
     * Initializes the screen with the given screen size and callbaks.
     *
     * @param _size screen dimensions in number of characters per line and number of lines.
     * @param _reply reply-callback with the data to send back to terminal input.
     * @param _logger an optional logger for logging various events.
     * @param _error an optional logger for errors.
     * @param _onCommands hook to the commands being executed by the screen.
     */
    Screen(WindowSize const& _size,
           std::optional<size_t> _maxHistoryLineCount,
           ModeSwitchCallback _useApplicationCursorKeys,
           std::function<void()> _onWindowTitleChanged,
           ResizeWindowCallback _resizeWindow,
           SetApplicationKeypadMode _setApplicationkeypadMode,
           SetBracketedPaste _setBracketedPaste,
		   OnSetCursorStyle _setCursorStyle,
           Reply _reply,
           Logger _logger,
           bool _logRaw,
           bool _logTrace,
           Hook _onCommands,
           OnBufferChanged _onBufferChanged,
           std::function<void()> _bell,
           std::function<RGBColor(DynamicColorName)> _requestDynamicColor,
           std::function<void(DynamicColorName)> _resetDynamicColor,
           std::function<void(DynamicColorName, RGBColor const&)> _setDynamicColor
    );

    Screen(WindowSize const& _size,
           std::optional<size_t> _maxHistoryLineCount,
           ModeSwitchCallback _useApplicationCursorKeys,
           std::function<void()> _onWindowTitleChanged,
           ResizeWindowCallback _resizeWindow,
           SetApplicationKeypadMode _setApplicationkeypadMode,
           SetBracketedPaste _setBracketedPaste,
		   OnSetCursorStyle _setCursorStyle,
           Reply _reply,
           Logger _logger
    ) : Screen{
        _size,
        _maxHistoryLineCount,
        std::move(_useApplicationCursorKeys),
        std::move(_onWindowTitleChanged),
        std::move(_resizeWindow),
        std::move(_setApplicationkeypadMode),
        std::move(_setBracketedPaste),
        std::move(_setCursorStyle),
        std::move(_reply),
        std::move(_logger),
        true, // logs raw output by default?
        true, // logs trace output by default?
        {}, {}, {}, {}, {}, {}} {}

    Screen(WindowSize const& _size, Logger _logger) :
        Screen{_size, std::nullopt, {}, {}, {}, {}, {}, {}, {}, move(_logger), true, true, {}, {}, {}, {}, {}, {}} {}

    void setLogTrace(bool _enabled) { logTrace_ = _enabled; }
    bool logTrace() const noexcept { return logTrace_; }
    void setLogRaw(bool _enabled) { logRaw_ = _enabled; }
    bool logRaw() const noexcept { return logRaw_; }

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);
    size_t historyLineCount() const noexcept;

    /// Writes given data into the screen.
    void write(char const* _data, size_t _size);

    void write(Command const& _command);

    /// Writes given data into the screen.
    void write(std::string_view const& _text) { write(_text.data(), _text.size()); }

    /// Renders the full screen by passing every grid cell to the callback.
    void render(Renderer const& _renderer, size_t _scrollOffset = 0) const;

    /// Renders a single text line.
    std::string renderTextLine(cursor_pos_t _row) const;

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    std::string renderText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot() const;

    // {{{ Command processor
    void operator()(Bell const& v);
    void operator()(FullReset const& v);
    void operator()(Linefeed const& v);
    void operator()(Backspace const& v);
    void operator()(DeviceStatusReport const& v);
    void operator()(ReportCursorPosition const& v);
    void operator()(ReportExtendedCursorPosition const& v);
    void operator()(SendDeviceAttributes const& v);
    void operator()(SendTerminalId const& v);
    void operator()(ClearToEndOfScreen const& v);
    void operator()(ClearToBeginOfScreen const& v);
    void operator()(ClearScreen const& v);
    void operator()(ClearScrollbackBuffer const& v);
    void operator()(EraseCharacters const& v);
    void operator()(ScrollUp const& v);
    void operator()(ScrollDown const& v);
    void operator()(ClearToEndOfLine const& v);
    void operator()(ClearToBeginOfLine const& v);
    void operator()(ClearLine const& v);
    void operator()(CursorNextLine const& v);
    void operator()(CursorPreviousLine const& v);
    void operator()(InsertCharacters const& v);
    void operator()(InsertLines const& v);
    void operator()(InsertColumns const& v);
    void operator()(DeleteLines const& v);
    void operator()(DeleteCharacters const& v);
    void operator()(DeleteColumns const& v);
    void operator()(HorizontalPositionAbsolute const& v);
    void operator()(HorizontalPositionRelative const& v);
    void operator()(HorizontalTabClear const& v);
    void operator()(HorizontalTabSet const& v);
    void operator()(MoveCursorUp const& v);
    void operator()(MoveCursorDown const& v);
    void operator()(MoveCursorForward const& v);
    void operator()(MoveCursorBackward const& v);
    void operator()(MoveCursorToColumn const& v);
    void operator()(MoveCursorToBeginOfLine const& v);
    void operator()(MoveCursorTo const& v);
    void operator()(MoveCursorToLine const& v);
    void operator()(MoveCursorToNextTab const& v);
    void operator()(CursorBackwardTab const& v);
    void operator()(SaveCursor const& v);
    void operator()(RestoreCursor const& v);
    void operator()(Index const& v);
    void operator()(ReverseIndex const& v);
    void operator()(BackIndex const& v);
    void operator()(ForwardIndex const& v);
    void operator()(SetForegroundColor const& v);
    void operator()(SetBackgroundColor const& v);
    void operator()(SetCursorStyle const& v);
    void operator()(SetGraphicsRendition const& v);
    void operator()(SetMark const&);
    void operator()(SetMode const& v);
    void operator()(RequestMode const& v);
    void operator()(SetTopBottomMargin const& v);
    void operator()(SetLeftRightMargin const& v);
    void operator()(ScreenAlignmentPattern const& v);
    void operator()(SendMouseEvents const& v);
    void operator()(ApplicationKeypadMode const& v);
    void operator()(DesignateCharset const& v);
    void operator()(SingleShiftSelect const& v);
    void operator()(SoftTerminalReset const& v);
    void operator()(ChangeWindowTitle const& v);
    void operator()(ResizeWindow const& v);
    void operator()(SaveWindowTitle const& v);
    void operator()(RestoreWindowTitle const& v);
    void operator()(AppendChar const& v);

    void operator()(RequestDynamicColor const& v);
    void operator()(RequestTabStops const& v);
    void operator()(ResetDynamicColor const& v);
    void operator()(SetDynamicColor const& v);
    // }}}

    // reset screen
    void resetSoft();
    void resetHard();

    WindowSize const& size() const noexcept { return size_; }
    void resize(WindowSize const& _newSize);

    /// {{{ viewport management API
    size_t scrollOffset() const noexcept { return scrollOffset_; }
    bool isAbsoluteLineVisible(cursor_pos_t _row) const noexcept;
    bool scrollUp(size_t _numLines);
    bool scrollDown(size_t _numLines);
    bool scrollToTop();
    bool scrollToBottom();
    bool scrollMarkUp();
    bool scrollMarkDown();
    //}}}

    bool isCursorInsideMargins() const noexcept {
        if (!state_->margin_.vertical.contains(state_->cursor.row))
            return false;
        else if (isModeEnabled(Mode::LeftRightMargin) && !state_->margin_.horizontal.contains(state_->cursor.column))
            return false;
        else
            return true;
    }

    Coordinate realCursorPosition() const noexcept { return state_->realCursorPosition(); }
    Coordinate cursorPosition() const noexcept { return state_->cursorPosition(); }
    Cursor const& realCursor() const noexcept { return state_->cursor; }

    Cell const& currentCell() const noexcept
    {
        return *state_->currentColumn;
    }

    Cell& currentCell() noexcept
    {
        return *state_->currentColumn;
    }

    Cell& currentCell(Cell value)
    {
        *state_->currentColumn = std::move(value);
        return *state_->currentColumn;
    }

    void moveCursorTo(Coordinate to);

    Cell const& absoluteAt(Coordinate const& _coord) const;

    Cell const& at(cursor_pos_t _row, cursor_pos_t _col) const noexcept;

    /// Retrieves the cell at given cursor, respecting origin mode.
    Cell& withOriginAt(cursor_pos_t row, cursor_pos_t col) { return state_->withOriginAt(row, col); }

    bool isPrimaryScreen() const noexcept { return state_ == &primaryBuffer_; }
    bool isAlternateScreen() const noexcept { return state_ == &alternateBuffer_; }

    bool isModeEnabled(Mode m) const noexcept
    {
        if (m == Mode::UseAlternateScreen)
            return isAlternateScreen();
        else
            return state_->enabledModes_.find(m) != end(state_->enabledModes_);
    }

    bool verticalMarginsEnabled() const noexcept { return isModeEnabled(Mode::Origin); }
    bool horizontalMarginsEnabled() const noexcept { return isModeEnabled(Mode::LeftRightMargin); }

    Margin const& margin() const noexcept { return state_->margin_; }
    ScreenBuffer::Lines const& scrollbackLines() const noexcept { return state_->savedLines; }

    void setTabWidth(unsigned int _value)
    {
        // TODO: Find out if we need to have that attribute per buffer or if having it across buffers is sufficient.
        primaryBuffer_.tabWidth = _value;
        alternateBuffer_.tabWidth = _value;
    }

    /**
     * Returns the n'th saved line into the history scrollback buffer.
     *
     * @param _lineNumberIntoHistory the 1-based offset into the history buffer.
     *
     * @returns the textual representation of the n'th line into the history.
     */
    std::string renderHistoryTextLine(cursor_pos_t _lineNumberIntoHistory) const;

    std::string const& windowTitle() const noexcept { return windowTitle_; }

    std::optional<size_t> findPrevMarker(size_t _currentScrollOffset) const
    {
        return state_->findPrevMarker(_currentScrollOffset);
    }

    std::optional<size_t> findNextMarker(size_t _currentScrollOffset) const
    {
        return state_->findNextMarker(_currentScrollOffset);
    }

    ScreenBuffer::Type bufferType() const noexcept { return state_->type_; }

  private:
    void setBuffer(ScreenBuffer::Type _type);

    // interactive replies
    void reply(std::string const& message)
    {
        if (reply_)
            reply_(message);
    }

    template <typename... Args>
    void reply(std::string const& fmt, Args&&... args)
    {
        reply(fmt::format(fmt, std::forward<Args>(args)...));
    }

  private:
    Hook const onCommands_;
    Logger const logger_;
    bool logRaw_ = false;
    bool logTrace_ = false;
    ModeSwitchCallback useApplicationCursorKeys_;
    std::function<void()> onWindowTitleChanged_;
    ResizeWindowCallback resizeWindow_;
    SetApplicationKeypadMode setApplicationkeypadMode_;
    SetBracketedPaste setBracketedPaste_;
	OnSetCursorStyle setCursorStyle_;
    Reply const reply_;

    OutputHandler handler_;
    Parser parser_;

    ScreenBuffer primaryBuffer_;
    ScreenBuffer alternateBuffer_;
    ScreenBuffer* state_;

    WindowSize size_;
    std::optional<size_t> maxHistoryLineCount_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    size_t scrollOffset_;

    OnBufferChanged onBufferChanged_{};
    std::function<void()> bell_{};
    std::function<RGBColor(DynamicColorName)> requestDynamicColor_{};
    std::function<void(DynamicColorName)> resetDynamicColor_{};
    std::function<void(DynamicColorName, RGBColor const&)> setDynamicColor_{};
};

constexpr bool operator==(ScreenBuffer::GraphicsAttributes const& a, ScreenBuffer::GraphicsAttributes const& b) noexcept
{
    return a.backgroundColor == b.backgroundColor
        && a.foregroundColor == b.foregroundColor
        && a.styles == b.styles;
}

constexpr bool operator==(ScreenBuffer::Cell const& a, Screen::Cell const& b) noexcept
{
    return a.character == b.character && a.attributes == b.attributes;
}

}  // namespace terminal


namespace fmt {
    template <>
    struct formatter<terminal::ScreenBuffer::Type> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::ScreenBuffer::Type value, FormatContext& ctx)
        {
            switch (value)
            {
                case terminal::ScreenBuffer::Type::Main:
                    return format_to(ctx.out(), "main");
                case terminal::ScreenBuffer::Type::Alternate:
                    return format_to(ctx.out(), "alternate");
            }
            return format_to(ctx.out(), "({})", static_cast<unsigned>(value));
        }
    };
}
