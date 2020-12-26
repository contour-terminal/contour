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
#pragma once

#include <terminal/Charset.h>
#include <terminal/Color.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/InputGenerator.h> // MouseTransport
#include <terminal/Logger.h>
#include <terminal/Parser.h>
#include <terminal/ScreenEvents.h>
#include <terminal/Sequencer.h>
#include <terminal/VTType.h>
#include <terminal/Size.h>

#include <crispy/algorithm.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/width.h>
#include <unicode/utf8.h>

#include <fmt/format.h>

#include <algorithm>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stack>
#include <string>
#include <string_view>
#include <vector>

namespace terminal {

// {{{ Modes
/// API for setting/querying terminal modes.
///
/// This abstracts away the actual implementation for more intuitive use and easier future adaptability.
class Modes {
  public:
    void set(AnsiMode _mode, bool _enabled)
    {
        if (_enabled)
            ansi_.insert(_mode);
        else if (auto i = ansi_.find(_mode); i != ansi_.end())
            ansi_.erase(i);
    }

    void set(DECMode  _mode, bool _enabled)
    {
        if (_enabled)
            dec_.insert(_mode);
        else if (auto i = dec_.find(_mode); i != dec_.end())
            dec_.erase(i);
    }

    bool enabled(AnsiMode _mode) const noexcept { return ansi_.find(_mode) != ansi_.end(); }

    bool enabled(DECMode _mode) const noexcept { return dec_.find(_mode) != dec_.end(); }

    void save(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode : _modes)
            savedModes_[mode].push_back(enabled(mode));
    }

    void restore(std::vector<DECMode> const& _modes)
    {
        for (DECMode const mode : _modes)
        {
            if (auto i = savedModes_.find(mode); i != savedModes_.end() && !i->second.empty())
            {
                auto& saved = i->second;
                set(mode, saved.back());
                saved.pop_back();
            }
        }
    }

  private:
    // TODO: make this a vector<bool> by casting from Mode, but that requires ensured small linearity in Mode enum values.
    std::set<AnsiMode> ansi_;
    std::set<DECMode> dec_;
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes
};
// }}}

// {{{ CharacterStyleMask
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
        CurlyUnderlined = (1 << 9),
        DottedUnderline = (1 << 10),
        DashedUnderline = (1 << 11),
        Framed = (1 << 12),
        Encircled = (1 << 13),
        Overline = (1 << 14),
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

constexpr bool operator==(CharacterStyleMask const& a, CharacterStyleMask const& b) noexcept
{
	return a.mask() == b.mask();
}

constexpr CharacterStyleMask& operator|=(CharacterStyleMask& a, CharacterStyleMask const& b) noexcept
{
    a = a | b;
	return a;
}

constexpr CharacterStyleMask& operator&=(CharacterStyleMask& a, CharacterStyleMask const& b) noexcept
{
    a = a & b;
	return a;
}

constexpr bool operator!(CharacterStyleMask const& a) noexcept
{
	return a.mask() == 0;
}
// }}}

// {{{ Margin
struct Margin {
	struct Range {
		int from;
		int to;

		constexpr int length() const noexcept { return to - from + 1; }
		constexpr bool operator==(Range const& rhs) const noexcept { return from == rhs.from && to == rhs.to; }
		constexpr bool operator!=(Range const& rhs) const noexcept { return !(*this == rhs); }

		constexpr bool contains(int _value) const noexcept { return from <= _value && _value <= to; }
	};

	Range vertical{}; // top-bottom
	Range horizontal{}; // left-right
};
// }}}

// {{{ GraphicsAttributes
/// Character graphics rendition information.
struct GraphicsAttributes {
    Color foregroundColor{DefaultColor{}};
    Color backgroundColor{DefaultColor{}};
    Color underlineColor{DefaultColor{}};
    CharacterStyleMask styles{};

    RGBColor getUnderlineColor(ColorProfile const& _colorProfile) const noexcept
    {
        float const opacity = [=]() {
            if (styles & CharacterStyleMask::Faint)
                return 0.5f;
            else
                return 1.0f;
        }();

        bool const bright = (styles & CharacterStyleMask::Bold) != 0;
        return apply(_colorProfile, underlineColor, ColorTarget::Foreground, bright) * opacity;
    }

    std::pair<RGBColor, RGBColor> makeColors(ColorProfile const& _colorProfile, bool _reverseVideo) const noexcept
    {
        float const opacity = [=]() { // TODO: don't make opacity dependant on Faint-attribute.
            if (styles & CharacterStyleMask::Faint)
                return 0.5f;
            else
                return 1.0f;
        }();

        bool const bright = (styles & CharacterStyleMask::Bold) != 0;

        auto const [fgColorTarget, bgColorTarget] =
            _reverseVideo
                ? std::pair{ ColorTarget::Background, ColorTarget::Foreground }
                : std::pair{ ColorTarget::Foreground, ColorTarget::Background };

        return (styles & CharacterStyleMask::Inverse) == 0
            ? std::pair{ apply(_colorProfile, foregroundColor, fgColorTarget, bright) * opacity,
                    apply(_colorProfile, backgroundColor, bgColorTarget, bright) }
            : std::pair{ apply(_colorProfile, backgroundColor, bgColorTarget, bright) * opacity,
                    apply(_colorProfile, foregroundColor, fgColorTarget, bright) };
    }
};

constexpr bool operator==(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return a.backgroundColor == b.backgroundColor
        && a.foregroundColor == b.foregroundColor
        && a.styles == b.styles
        && a.underlineColor == b.underlineColor;
}

constexpr bool operator!=(GraphicsAttributes const& a, GraphicsAttributes const& b) noexcept
{
    return !(a == b);
}
// }}}

// {{{ Cursor
/// Terminal cursor data structure.
///
/// NB: Take care what to store here, as DECSC/DECRC will save/restore this struct.
struct Cursor
{
    Coordinate position{1, 1};
    bool autoWrap = false;
    bool originMode = false;
    bool visible = true;
    GraphicsAttributes graphicsRendition{};
    CharsetMapping charsets{};
    // TODO: selective erase attribute
    // TODO: SS2/SS3 states
    // TODO: CharacterSet for GL and GR
};
// }}}

// {{{ Cell
/// Grid cell with character and graphics rendition information.
class Cell {
  public:
    static size_t constexpr MaxCodepoints = 9;

    Cell(char32_t _ch, GraphicsAttributes _attrib) noexcept :
        codepoints_{},
        attributes_{std::move(_attrib)},
        width_{1},
        codepointCount_{0}
    {
        setCharacter(_ch);
    }

    constexpr Cell() noexcept :
        codepoints_{},
        attributes_{},
        width_{1},
        codepointCount_{0}
    {}

    void reset() noexcept
    {
        attributes_ = {};
        codepointCount_ = 0;
        width_ = 1;
        hyperlink_ = nullptr;
        imageFragment_.reset();
    }

    void reset(GraphicsAttributes _attribs, HyperlinkRef const& _hyperlink) noexcept
    {
        attributes_ = std::move(_attribs);
        codepointCount_ = 0;
        width_ = 1;
        hyperlink_ = _hyperlink;
        imageFragment_.reset();
    }

    Cell(Cell const&) = default;
    Cell(Cell&&) noexcept = default;
    Cell& operator=(Cell const&) = default;
    Cell& operator=(Cell&&) noexcept = default;

    constexpr std::u32string_view codepoints() const noexcept
    {
        return std::u32string_view{codepoints_.data(), codepointCount_};
    }

    constexpr char32_t codepoint(size_t i) const noexcept { return codepoints_[i]; }
    constexpr int codepointCount() const noexcept { return codepointCount_; }

    constexpr bool empty() const noexcept { return codepointCount_ == 0; }

    constexpr int width() const noexcept { return width_; }

    constexpr GraphicsAttributes const& attributes() const noexcept { return attributes_; }
    constexpr GraphicsAttributes& attributes() noexcept { return attributes_; }

    std::optional<ImageFragment> const& imageFragment() const noexcept { return imageFragment_; }

    void setImage(ImageFragment _imageFragment, HyperlinkRef _hyperlink)
    {
        imageFragment_.emplace(std::move(_imageFragment));
        hyperlink_ = std::move(_hyperlink);
        width_ = 1;
        codepointCount_ = 0;
    }

    void setCharacter(char32_t _codepoint) noexcept
    {
        imageFragment_.reset();
        codepoints_[0] = _codepoint;
        if (_codepoint)
        {
            codepointCount_ = 1;
            width_ = std::max(unicode::width(_codepoint), 1);
        }
        else
        {
            codepointCount_ = 0;
            width_ = 1;
        }
    }

    void setWidth(int _width) noexcept
    {
        width_ = _width;
    }

    int appendCharacter(char32_t _codepoint) noexcept
    {
        imageFragment_.reset();
        if (codepointCount_ < MaxCodepoints)
        {
            codepoints_[codepointCount_] = _codepoint;
            codepointCount_++;

            constexpr bool AllowWidthChange = false; // TODO: make configurable

            auto const width = [&]() {
                switch (_codepoint)
                {
                    case 0xFE0E:
                        return 1;
                    case 0xFE0F:
                        return 2;
                    default:
                        return unicode::width(_codepoint);
                }
            }();

            if (width != width_ && AllowWidthChange)
            {
                int const diff = width - width_;
                width_ = width;
                return diff;
            }
        }
        return 0;
    }

    std::string toUtf8() const;

    HyperlinkRef hyperlink() const noexcept { return hyperlink_; }
    void setHyperlink(HyperlinkRef const& _hyperlink) { hyperlink_ = _hyperlink; }

  private:
    /// Unicode codepoint to be displayed.
    std::array<char32_t, MaxCodepoints> codepoints_;

    /// Graphics renditions, such as foreground/background color or other grpahics attributes.
    GraphicsAttributes attributes_;

    /// number of cells this cell spans. Usually this is 1, but it may be also 0 or >= 2.
    uint8_t width_;

    /// Number of combined codepoints stored in this cell.
    uint8_t codepointCount_;

    HyperlinkRef hyperlink_ = nullptr;

    /// Image fragment to be rendered in this cell.
    std::optional<ImageFragment> imageFragment_;
};

constexpr bool operator==(Cell const& a, Cell const& b) noexcept
{
    if (a.codepointCount() != b.codepointCount())
        return false;

    if (!(a.attributes() == b.attributes()))
        return false;

    for (auto const i : crispy::times(a.codepointCount()))
        if (a.codepoint(i) != b.codepoint(i))
            return false;

    return true;
}

// }}}

struct Line { // {{{
    using LineBuffer = std::vector<Cell>;
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
    const_iterator begin() const { return buffer.begin(); }
    const_iterator end() const { return buffer.end(); }
    reverse_iterator rbegin() { return buffer.rbegin(); }
    reverse_iterator rend() { return buffer.rend(); }
    const_iterator cbegin() const { return buffer.cbegin(); }
    const_iterator cend() const { return buffer.cend(); }
};
// }}}

using Lines = std::deque<Line>;
using ColumnIterator = Line::iterator;
using LineIterator = Lines::iterator;

inline auto begin(Line& _line) { return _line.begin(); }
inline auto end(Line& _line) { return _line.end(); }
inline auto begin(Line const& _line) { return _line.cbegin(); }
inline auto end(Line const& _line) { return _line.cend(); }
inline Line::const_iterator cbegin(Line const& _line) { return _line.cbegin(); }
inline Line::const_iterator cend(Line const& _line) { return _line.cend(); }

/**
 * Terminal Screen.
 *
 * Implements the all VT command types and applies all instruction
 * to an internal screen buffer, maintaining width, height, and history,
 * allowing the object owner to control which part of the screen (or history)
 * to be viewn.
 */
class Screen {
  public:
    /**
     * Initializes the screen with the given screen size and callbaks.
     *
     * @param _size screen dimensions in number of characters per line and number of lines.
     * @param _eventListener Interface to some VT sequence related callbacks.
     * @param _logger an optional logger for logging various events.
     * @param _logRaw whether or not to log raw VT sequences.
     * @param _logTrace whether or not to log VT sequences in trace mode.
     * @param _maxHistoryLineCount number of lines the history must not exceed.
     */
    Screen(Size const& _size,
           ScreenEvents& _eventListener,
           Logger const& _logger = Logger{},
           bool _logRaw = false,
           bool _logTrace = false,
           std::optional<size_t> _maxHistoryLineCount = std::nullopt,
           Size _maxImageSize = Size{800, 600},
           int _maxImageColorRegisters = 256,
           bool _sixelCursorConformance = true
    );

    void setLogTrace(bool _enabled) { logTrace_ = _enabled; }
    bool logTrace() const noexcept { return logTrace_; }
    void setLogRaw(bool _enabled) { logRaw_ = _enabled; }
    bool logRaw() const noexcept { return logRaw_; }

    void setMaxImageColorRegisters(int _value) noexcept { sequencer_.setMaxImageColorRegisters(_value); }
    void setSixelCursorConformance(bool _value) noexcept { sixelCursorConformance_ = _value; }

    constexpr Size cellPixelSize() const noexcept { return cellPixelSize_; }

    constexpr void setCellPixelSize(Size _cellPixelSize)
    {
        cellPixelSize_ = _cellPixelSize;
    }

    void setTerminalId(VTType _id) noexcept
    {
        terminalId_ = _id;
    }

    void setMaxHistoryLineCount(std::optional<size_t> _maxHistoryLineCount);
    int historyLineCount() const noexcept { return static_cast<int>(savedLines_.size()); }

    /// Writes given data into the screen.
    void write(char const* _data, size_t _size);

    /// Writes given data into the screen.
    void write(std::string_view const& _text) { write(_text.data(), _text.size()); }

    void write(std::u32string_view const& _text);

    void writeText(char32_t _char);

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    void render(RendererT _renderer, std::optional<int> _scrollOffset = std::nullopt) const;

    /// Renders a single text line.
    std::string renderTextLine(int _row) const;

    /// Renders the full screen as text into the given string. Each line will be terminated by LF.
    std::string renderText() const;

    /// Takes a screenshot by outputting VT sequences needed to render the current state of the screen.
    ///
    /// @note Only the screenshot of the current buffer is taken, not both (main and alternate).
    ///
    /// @returns necessary commands needed to draw the current screen state,
    ///          including initial clear screen, and initial cursor hide.
    std::string screenshot() const;

    void setFocus(bool _focused) { focused_ = _focused; }
    bool focused() const noexcept { return focused_; }

    // {{{ VT API
    void linefeed(); // LF

    void clearToBeginOfLine();
    void clearToEndOfLine();
    void clearLine();

    void clearToBeginOfScreen();
    void clearToEndOfScreen();
    void clearScreen();

    void clearScrollbackBuffer();

    void eraseCharacters(int _n);  // ECH
    void insertCharacters(int _n); // ICH
    void deleteCharacters(int _n); // DCH
    void deleteColumns(int _n);    // DECDC
    void insertLines(int _n);      // IL
    void insertColumns(int _n);    // DECIC

    void deleteLines(int _n);      // DL

    void backIndex();    // DECBI
    void forwardIndex(); // DECFI

    void moveCursorBackward(int _n);      // CUB
    void moveCursorDown(int _n);          // CUD
    void moveCursorForward(int _n);       // CUF
    void moveCursorTo(int _n);            // CUP
    void moveCursorToBeginOfLine();       // CR
    void moveCursorToColumn(int _n);      // CHA
    void moveCursorToLine(int _n);        // VPA
    void moveCursorToNextLine(int _n);    // CNL
    void moveCursorToNextTab();           // HT
    void moveCursorToPrevLine(int _n);    // CPL
    void moveCursorUp(int _n);            // CUU

    void cursorBackwardTab(int _n);       // CBT
    void cursorForwardTab(int _n);        // CHT
    void backspace();                     // BS
    void horizontalTabClear(HorizontalTabClear _which); // TBC
    void horizontalTabSet();              // HTS

    void index(); // IND
    void reverseIndex(); // RI

    void setMark();
    void deviceStatusReport();            // DSR
    void reportCursorPosition();          // CPR
    void reportExtendedCursorPosition();  // DECXCPR
    void selectConformanceLevel(VTType _level);
    void requestDynamicColor(DynamicColorName _name);
    void sendDeviceAttributes();
    void sendTerminalId();

    /// Sets the current working directory as file:// URL.
    void setCurrentWorkingDirectory(std::string const& _url);             // OSC 7

    /// @returns either an empty string or a file:// URL of the last set working directory.
    std::string currentWorkingDirectory() const noexcept { return currentWorkingDirectory_; }

    void hyperlink(std::string const& _id, std::string const& _uri);      // OSC 8
    void notify(std::string const& _title, std::string const& _content);  // OSC 777

    void setForegroundColor(Color const& _color);
    void setBackgroundColor(Color const& _color);
    void setUnderlineColor(Color const& _color);
    void setCursorStyle(CursorDisplay _display, CursorShape _shape);
    void setGraphicsRendition(GraphicsRendition _rendition);
    void requestMode(std::variant<AnsiMode, DECMode> _mode);
    void setTopBottomMargin(std::optional<int> _top, std::optional<int> _bottom);
    void setLeftRightMargin(std::optional<int> _left, std::optional<int> _right);
    void screenAlignmentPattern();
    void sendMouseEvents(MouseProtocol _protocol, bool _enable);
    void applicationKeypadMode(bool _enable);
    void designateCharset(CharsetTable _table, CharsetId _charset);
    void singleShiftSelect(CharsetTable _table);
    void requestPixelSize(RequestPixelSize _area);
    void sixelImage(Size _pixelSize, Image::Data&& _rgba);
    void requestStatusString(RequestStatusString _value);
    void requestTabStops();
    void resetDynamicColor(DynamicColorName _name);
    void setDynamicColor(DynamicColorName _name, RGBColor const& _color);
    void dumpState();
    void smGraphics(XtSmGraphics::Item _item, XtSmGraphics::Action _action, XtSmGraphics::Value _value);
    // }}}

    std::shared_ptr<Image const> uploadImage(ImageFormat _format, Size _imageSize, Image::Data&& _pixmap);

    /**
     * Renders an image onto the screen.
     *
     * @p _imageRef Reference to the image to be rendered.
     * @p _topLeft Screen coordinate to start rendering the top/left corner of the image.
     * @p _gridSize Screen grid size to span the image into.
     * @p _imageOffset Offset into the image in screen grid coordinate to start rendering from.
     * @p _imageSize Size of the full image in Screen grid coordinates.
     * @p _alignmentPolicy render the image using the given image alignment policy.
     * @p _resizePolicy render the image using the given image resize policy.
     * @p _autoScroll Boolean indicating whether or not the screen should scroll if the image cannot be fully displayed otherwise.
     */
    void renderImage(std::shared_ptr<Image const> const& _imageRef,
                     Coordinate _topLeft,
                     Size _gridSize,
                     Coordinate _imageOffset,
                     Size _imageSize,
                     ImageAlignment _alignmentPolicy,
                     ImageResize _resizePolicy,
                     bool _autoScroll);

    void dumpState(std::string const& _message) const;

    // reset screen
    void resetSoft();
    void resetHard();

    // for DECSC and DECRC
    void setMode(AnsiMode _mode, bool _enabled);
    void setMode(DECMode _mode, bool _enabled);
    void saveCursor();
    void restoreCursor();
    void restoreCursor(Cursor const& _savedCursor);
    void saveModes(std::vector<DECMode> const& _modes);
    void restoreModes(std::vector<DECMode> const& _modes);

    Size const& size() const noexcept { return size_; }
    void resize(Size const& _newSize);

    /// Implements semantics for  DECCOLM / DECSCPP.
    void resizeColumns(int _newColumnCount, bool _clear);

    bool isCursorInsideMargins() const noexcept
    {
        bool const insideVerticalMargin = margin_.vertical.contains(cursor_.position.row);
        bool const insideHorizontalMargin = !isModeEnabled(DECMode::LeftRightMargin)
                                         || margin_.horizontal.contains(cursor_.position.column);
        return insideVerticalMargin && insideHorizontalMargin;
    }

    constexpr Coordinate realCursorPosition() const noexcept { return cursor_.position; }

    constexpr Coordinate cursorPosition() const noexcept {
        if (!cursor_.originMode)
            return realCursorPosition();
        else
            return Coordinate{
                cursor_.position.row - margin_.vertical.from + 1,
                cursor_.position.column - margin_.horizontal.from + 1
            };
    }

    constexpr Coordinate origin() const noexcept {
        if (cursor_.originMode)
            return {margin_.vertical.from, margin_.horizontal.from};
        else
            return {1, 1};
    }

    Cursor const& cursor() const noexcept { return cursor_; }

    /// Returns identity if DECOM is disabled (default), but returns translated coordinates if DECOM is enabled.
    Coordinate toRealCoordinate(Coordinate const& pos) const noexcept
    {
        if (!cursor_.originMode)
            return pos;
        else
            return { pos.row + margin_.vertical.from - 1, pos.column + margin_.horizontal.from - 1 };
    }

    /// Clamps given coordinates, respecting DECOM (Origin Mode).
    Coordinate clampCoordinate(Coordinate const& coord) const noexcept
    {
        if (!cursor_.originMode)
            return clampToOrigin(coord);
        else
            return clampToScreen(coord);
    }

    /// Clamps given logical coordinates to margins as used in when DECOM (origin mode) is enabled.
    Coordinate clampToOrigin(Coordinate const& coord) const noexcept
    {
        return {
            std::clamp(coord.row, int{0}, margin_.vertical.length()),
            std::clamp(coord.column, int{0}, margin_.horizontal.length())
        };
    }

    Coordinate clampToScreen(Coordinate const& coord) const noexcept
    {
        return {
            std::clamp(coord.row, int{ 1 }, size_.height),
            std::clamp(coord.column, int{ 1 }, size_.width)
        };
    }

    // Tests if given coordinate is within the visible screen area.
    constexpr bool contains(Coordinate const& _coord) const noexcept
    {
        return 1 <= _coord.row && _coord.row <= size_.height
            && 1 <= _coord.column && _coord.column <= size_.width;
    }

    Cell const& currentCell() const noexcept
    {
        return *currentColumn_;
    }

    Cell& currentCell() noexcept
    {
        return *currentColumn_;
    }

    Cell& currentCell(Cell value)
    {
        *currentColumn_ = std::move(value);
        return *currentColumn_;
    }

    void moveCursorTo(Coordinate to);

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(Coordinate const& _coord) noexcept
    {
        assert(crispy::ascending(1 - historyLineCount(), _coord.row, size_.height));
        assert(crispy::ascending(1, _coord.column, size_.width));

        if (_coord.row > 0)
            return (*next(begin(lines()), _coord.row - 1))[_coord.column - 1];
        else
            return (*next(rbegin(savedLines_), -_coord.row))[_coord.column - 1];
    }

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(Coordinate const& _coord) const noexcept
    {
        return const_cast<Screen&>(*this).at(_coord);
    }

    bool isPrimaryScreen() const noexcept { return activeBuffer_ == &lines_[0]; }
    bool isAlternateScreen() const noexcept { return activeBuffer_ == &lines_[1]; }

    bool isModeEnabled(AnsiMode m) const noexcept { return modes_.enabled(m); }
    bool isModeEnabled(DECMode m) const noexcept { return modes_.enabled(m); }

    bool isModeEnabled(std::variant<AnsiMode, DECMode> m) const {
        if (std::holds_alternative<AnsiMode>(m))
            return modes_.enabled(std::get<AnsiMode>(m));
        else
            return modes_.enabled(std::get<DECMode>(m));
    }

    bool verticalMarginsEnabled() const noexcept { return isModeEnabled(DECMode::Origin); }
    bool horizontalMarginsEnabled() const noexcept { return isModeEnabled(DECMode::LeftRightMargin); }

    Margin const& margin() const noexcept { return margin_; }
    Lines const& scrollbackLines() const noexcept { return savedLines_; }

    void setTabWidth(int _value)
    {
        tabWidth_ = _value;
    }

    /**
     * Returns the n'th saved line into the history scrollback buffer.
     *
     * @param _lineNumberIntoHistory the 1-based offset into the history buffer.
     *
     * @returns the textual representation of the n'th line into the history.
     */
    std::string renderHistoryTextLine(int _lineNumberIntoHistory) const;

    std::string const& windowTitle() const noexcept { return windowTitle_; }

    /// Finds the next marker right after the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<int> findMarkerForward(int _currentCursorLine) const;

    /// Finds the previous marker right next to the given line position.
    ///
    /// @paramn _currentCursorLine the line number of the current cursor (1..N) for screen area, or
    ///                            (0..-N) for savedLines area
    /// @return cursor position relative to screen origin (1, 1), that is, if line Number os >= 1, it's
    ///         in the screen area, and in the savedLines area otherwise.
    std::optional<int> findMarkerBackward(int _currentCursorLine) const;

    /// ScreenBuffer's type, such as main screen or alternate screen.
    ScreenType bufferType() const noexcept { return screenType_; }

    bool synchronizeOutput() const noexcept { return false; } // TODO

    ScreenEvents& eventListener() noexcept { return eventListener_; }
    ScreenEvents const& eventListener()  const noexcept { return eventListener_; }

    void setWindowTitle(std::string const& _title);
    void saveWindowTitle();
    void restoreWindowTitle();

    void setMaxImageSize(Size _size) noexcept { sequencer_.setMaxImageSize(_size); }

    void scrollUp(int n) { scrollUp(n, margin_); }
    void scrollDown(int n) { scrollDown(n, margin_); }

    void verifyState() const;

    // interactive replies
    void reply(std::string const& message)
    {
        eventListener_.reply(message);
    }

    template <typename... Args>
    void reply(std::string const& fmt, Args&&... args)
    {
        reply(fmt::format(fmt, std::forward<Args>(args)...));
    }

  private:
    void setBuffer(ScreenType _type);

    Coordinate resizeBuffer(Size const& _newSize, Lines& _buffer, Lines& _savedLines) const;

    Lines& primaryBuffer() noexcept { return lines_[0]; }
    Lines& alternateBuffer() noexcept { return lines_[1]; }

    Lines const& lines() const noexcept { return *activeBuffer_; }
    Lines& lines() noexcept { return *activeBuffer_; }

    void clearAllTabs();
    void clearTabUnderCursor();
    void setTabUnderCursor();

    /// Applies LF but also moves cursor to given column @p _column.
    void linefeed(int _column);

    void writeCharToCurrentAndAdvance(char32_t _codepoint);
    void clearAndAdvance(int _offset);

    void clampSavedLines();
    void clampSavedLines(Lines& _savedLines) const;

    void fail(std::string const& _message) const;

    void updateCursorIterators()
    {
        currentLine_ = next(begin(lines()), cursor_.position.row - 1);
        updateColumnIterator();
    }

    void updateColumnIterator()
    {
        currentColumn_ = columnIteratorAt(cursor_.position.column);
    }

    /// @returns an iterator to @p _n columns after column @p _begin.
    ColumnIterator columnIteratorAt(ColumnIterator _begin, int _n)
    {
        return next(_begin, _n - 1);
    }

    /// @returns an iterator to the real column number @p _n.
    ColumnIterator columnIteratorAt(int _n)
    {
        return columnIteratorAt(std::begin(*currentLine_), _n);
    }

    /// @returns an iterator to the real column number @p _n.
    ColumnIterator columnIteratorAt(int _n) const
    {
        return const_cast<Screen*>(this)->columnIteratorAt(_n);
    }

    void scrollUp(int n, Margin const& margin);
    void scrollDown(int n, Margin const& margin);
    void insertChars(int _lineNo, int _n);
    void deleteChars(int _lineNo, int _n);

    /// Sets the current column to given logical column number.
    void setCurrentColumn(int _n);

  private:
    ScreenEvents& eventListener_;

    Logger const logger_;
    bool logRaw_ = false;
    bool logTrace_ = false;
    bool focused_ = true;

    Size cellPixelSize_; ///< contains the pixel size of a single cell, or area(cellPixelSize_) == 0 if unknown.

    VTType terminalId_ = VTType::VT525;

    Modes modes_;
    std::map<DECMode, std::vector<bool>> savedModes_; //!< saved DEC modes

    int maxImageColorRegisters_;
    std::shared_ptr<ColorPalette> imageColorPalette_;
    ImagePool imagePool_;

    Sequencer sequencer_;
    parser::Parser parser_;
    int64_t instructionCounter_ = 0;

    Size size_;
    std::optional<size_t> maxHistoryLineCount_;
    std::string windowTitle_{};
    std::stack<std::string> savedWindowTitles_{};

    bool sixelCursorConformance_ = true;

    // XXX moved from ScreenBuffer
    Margin margin_;
    int wrapPending_ = 0;
    int tabWidth_{8};
    std::vector<int> tabs_;

    // main/alt screen and history
    //
    std::array<Lines, 2> lines_;
    ScreenType screenType_ = ScreenType::Main;
    Lines* activeBuffer_;
    Lines savedLines_{};

    // cursor related
    //
    Cursor cursor_;
    Cursor savedCursor_;
    Cursor savedPrimaryCursor_; //!< saved cursor of primary-screen when switching to alt-screen.
    LineIterator currentLine_;
    ColumnIterator currentColumn_;
    ColumnIterator lastColumn_;
    Coordinate lastCursorPosition_;

    std::string currentWorkingDirectory_ = {};

    // Hyperlink related
    //
    HyperlinkRef currentHyperlink_ = {};
    std::unordered_map<std::string, HyperlinkRef> hyperlinks_; // TODO: use a deque<> instead, always push_back, lookup reverse, evict in front.
};

// {{{ template functions
template <typename RendererT>
void Screen::render(RendererT _render, std::optional<int> _scrollOffset) const
{
    if (!_scrollOffset.has_value())
    {
        crispy::for_each(
            crispy::times(1, size_.height) * crispy::times(1, size_.width),
            [&](auto const& _pos) {
                auto const [row, col] = _pos;
                auto const pos = Coordinate{row, col};
                _render({row, col}, at(pos));
            }
        );
    }
    else
    {
        _scrollOffset = std::clamp(*_scrollOffset, 0, historyLineCount());

        int rowNumber = 1;

        // render first part from history
        for (auto line = next(begin(const_cast<Screen*>(this)->savedLines_), *_scrollOffset);
                line != end(savedLines_) && rowNumber <= size_.height;
                ++line, ++rowNumber)
        {
            if (static_cast<int>(line->size()) < size_.width)
                line->resize(size_.width); // TODO: don't resize; fill the gap with stub render calls instead

            auto column = begin(*line);
            for (int colNumber = 1; colNumber <= size_.width; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }

        // render second part from main screen buffer
        for (auto line = begin(lines()); rowNumber <= size_.height; ++line, ++rowNumber)
        {
            auto column = begin(*line);
            for (int colNumber = 1; colNumber <= size_.width; ++colNumber, ++column)
                _render({rowNumber, colNumber}, *column);
        }
    }
}
// }}}

}  // namespace terminal

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::Margin::Range> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Margin::Range range, FormatContext& ctx)
        {
            return format_to(ctx.out(), "{}..{}", range.from, range.to);
        }
    };

    template <>
    struct formatter<terminal::Cursor> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Cursor cursor, FormatContext& ctx)
        {
            return format_to(ctx.out(), "({}:{}{})", cursor.position.row, cursor.position.column, cursor.visible ? "" : ", (invis)");
        }
    };

    template <>
    struct formatter<terminal::Cell> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(terminal::Cell const& cell, FormatContext& ctx)
        {
            std::string codepoints;
            for (auto const i : crispy::times(cell.codepointCount()))
            {
                if (i)
                    codepoints += ", ";
                codepoints += fmt::format("{:02X}", static_cast<unsigned>(cell.codepoint(i)));
            }
            return format_to(ctx.out(), "(chars={}, width={})", codepoints, cell.width());
        }
    };

    template <>
    struct formatter<terminal::ScreenType> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(const terminal::ScreenType value, FormatContext& ctx)
        {
            switch (value)
            {
                case terminal::ScreenType::Main:
                    return format_to(ctx.out(), "main");
                case terminal::ScreenType::Alternate:
                    return format_to(ctx.out(), "alternate");
            }
            return format_to(ctx.out(), "({})", static_cast<unsigned>(value));
        }
    };

    template <>
    struct formatter<terminal::CharacterStyleMask> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx)
        {
            return ctx.begin();
        }

        template <typename FormatContext>
        auto format(terminal::CharacterStyleMask const& _mask, FormatContext& ctx)
        {
            using Mask = terminal::CharacterStyleMask;
            auto constexpr mappings = std::array<std::pair<Mask, std::string_view>, 12>{
                std::pair{Mask::Bold, "bold"},
                std::pair{Mask::Faint, "faint"},
                std::pair{Mask::Italic, "italic"},
                std::pair{Mask::Underline, "underline"},
                std::pair{Mask::Blinking, "blinking"},
                std::pair{Mask::Inverse, "inverse"},
                std::pair{Mask::Hidden, "hidden"},
                std::pair{Mask::CrossedOut, "crossedOut"},
                std::pair{Mask::DoublyUnderlined, "doublyUnderlined"},
                std::pair{Mask::CurlyUnderlined, "curlyUnderlined"},
                std::pair{Mask::Framed, "framed"},
                std::pair{Mask::Overline, "overline"}
            };
            int i = 0;
            std::ostringstream os;
            for (auto const& mapping : mappings)
            {
                if (_mask.mask() & mapping.first)
                {
                    if (i) os << ", ";
                    os << mapping.second;
                }
            }
            return format_to(ctx.out(), "{}", os.str());
        }
    };
} // }}}
