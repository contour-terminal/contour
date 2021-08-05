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
#include <terminal/Coordinate.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>
#include <terminal/primitives.h>

#include <crispy/algorithm.h>
#include <crispy/indexed.h>
#include <crispy/point.h>
#include <crispy/range.h>
#include <crispy/size.h>
#include <crispy/span.h>
#include <crispy/times.h>
#include <crispy/utils.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/width.h>
#include <unicode/utf8.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
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

// {{{ CellFlags
enum class CellFlags : uint32_t
{
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
    Image = (1 << 15),

    // The following flags are for internal use only.
    Hover = (1 << 16), // Marks the cell with "Hyperlink is currently hovered" hint.
    CellSequenceStart = (1 << 17), // Marks the beginning of a consecutive sequence of non-empty grid cells.
    CellSequenceEnd = (1 << 18), // Marks the end of a consecutive sequence of non-empty grid cells.
};

constexpr CellFlags& operator|=(CellFlags& a, CellFlags b) noexcept
{
    a = static_cast<CellFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
	return a;
}

constexpr CellFlags& operator&=(CellFlags& a, CellFlags b) noexcept
{
    a = static_cast<CellFlags>(static_cast<unsigned>(a) & static_cast<unsigned>(b));
	return a;
}

/// Tests if @p b is contained in @p a.
constexpr bool operator&(CellFlags a, CellFlags b) noexcept
{
    return (static_cast<unsigned>(a) & static_cast<unsigned>(b)) != 0;
}

constexpr bool contains_all(CellFlags _base, CellFlags _test) noexcept
{
    return (static_cast<unsigned>(_base) & static_cast<unsigned>(_test)) == static_cast<unsigned>(_test);
}

/// Merges two CellFlags sets.
constexpr CellFlags operator|(CellFlags a, CellFlags b) noexcept
{
    return static_cast<CellFlags>(static_cast<unsigned>(a) | static_cast<unsigned>(b));
}

/// Inverts the flags set.
constexpr CellFlags operator~(CellFlags a) noexcept
{
    return static_cast<CellFlags>(~static_cast<unsigned>(a));
}

/// Tests for all flags cleared state.
constexpr bool operator!(CellFlags a) noexcept
{
    return static_cast<unsigned>(a) == 0;
}
// }}}

// {{{ GraphicsAttributes
/// Character graphics rendition information.
struct GraphicsAttributes {
    Color foregroundColor{DefaultColor()};
    Color backgroundColor{DefaultColor()};
    Color underlineColor{DefaultColor()};
    CellFlags styles{};

    RGBColor getUnderlineColor(ColorPalette const& _colorPalette, RGBColor _defaultColor) const noexcept
    {
        if (isDefaultColor(underlineColor))
            return _defaultColor;

        float const opacity = [this]() {
            if (styles & CellFlags::Faint)
                return 0.5f;
            else
                return 1.0f;
        }();

        bool const bright = (styles & CellFlags::Bold) != 0;
        return apply(_colorPalette, underlineColor, ColorTarget::Foreground, bright) * opacity;
    }

    std::pair<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette, bool _reverseVideo) const noexcept
    {
        float const opacity = [this]() { // TODO: don't make opacity dependant on Faint-attribute.
            if (styles & CellFlags::Faint)
                return 0.5f;
            else
                return 1.0f;
        }();

        bool const bright = (styles & CellFlags::Bold);

        auto const [fgColorTarget, bgColorTarget] =
            _reverseVideo
                ? std::pair{ ColorTarget::Background, ColorTarget::Foreground }
                : std::pair{ ColorTarget::Foreground, ColorTarget::Background };

        return (styles & CellFlags::Inverse) == 0
            ? std::pair{ apply(_colorPalette, foregroundColor, fgColorTarget, bright) * opacity,
                         apply(_colorPalette, backgroundColor, bgColorTarget, false) }
            : std::pair{ apply(_colorPalette, backgroundColor, bgColorTarget, bright) * opacity,
                         apply(_colorPalette, foregroundColor, fgColorTarget, false) };
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

// {{{ Cell
/// Grid cell with character and graphics rendition information.
class Cell {
  public:
    static size_t constexpr MaxCodepoints = 9;

    Cell(char32_t _codepoint, GraphicsAttributes _attrib) noexcept :
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        codepoints_{},
#else
        codepointCount_{0},
        codepoints_{},
#endif
        width_{1},
        attributes_{_attrib}
    {
        // setCharacter(_codepoint);
        if (_codepoint)
        {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
            codepoints_.assign(1, _codepoint);
#else
            codepointCount_ = 1;
            codepoints_[0] = _codepoint;
#endif
            width_ = static_cast<uint8_t>(std::max(unicode::width(_codepoint), 1));
        }
    }

    Cell() noexcept :
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        codepoints_{},
#else
        codepointCount_{0},
        codepoints_{},
#endif
        width_{1},
        attributes_{}
    {}

    void reset(GraphicsAttributes _attributes = {}) noexcept
    {
        attributes_ = _attributes;
        width_ = 1;
#if defined(LIBTERMINAL_HYPERLINKS)
        hyperlink_ = nullptr;
#endif
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        codepoints_.clear();
#else
        codepointCount_ = 0;
#endif
#if defined(LIBTERMINAL_IMAGES)
        imageFragment_.reset();
#endif
    }

#if defined(LIBTERMINAL_HYPERLINKS)
    void reset(GraphicsAttributes _attribs, HyperlinkRef const& _hyperlink) noexcept
    {
        attributes_ = _attribs;
        width_ = 1;
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        codepoints_.clear();
#else
        codepointCount_ = 0;
#endif
        hyperlink_ = _hyperlink;
#if defined(LIBTERMINAL_IMAGES)
        imageFragment_.reset();
#endif
    }
#endif

    Cell(Cell const&) = default;
    Cell(Cell&&) noexcept = default;
    Cell& operator=(Cell const&) = default;
    Cell& operator=(Cell&&) noexcept = default;

    std::u32string_view codepoints() const noexcept
    {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        return codepoints_;
#else
        return {codepoints_.data(), codepointCount_};
#endif
    }

    char32_t codepoint(size_t i) const noexcept
    {
#if !defined(NDEBUG)
        return codepoints_.at(i);
#else
        return codepoints_[i];
#endif
    }

    std::size_t codepointCount() const noexcept
    {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        return codepoints_.size();
#else
        return codepointCount_;
#endif
    }

#if defined(LIBTERMINAL_IMAGES)
    bool empty() const noexcept { return codepointCount() == 0 && !imageFragment_; }
#else
    bool empty() const noexcept { return codepointCount() == 0; }
#endif

    constexpr int width() const noexcept { return width_; }

    constexpr GraphicsAttributes const& attributes() const noexcept { return attributes_; }

#if defined(LIBTERMINAL_IMAGES)
    std::optional<ImageFragment> const& imageFragment() const noexcept { return imageFragment_; }

    void setImage(ImageFragment _imageFragment)
    {
        imageFragment_.emplace(std::move(_imageFragment));
        width_ = 1;
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
        codepoints_.clear();
#else
        codepointCount_ = 0;
#endif
    }

#if defined(LIBTERMINAL_HYPERLINKS)
    void setImage(ImageFragment _imageFragment, HyperlinkRef _hyperlink)
    {
        setImage(std::move(_imageFragment));
        hyperlink_ = std::move(_hyperlink);
    }
#endif
#endif

    void setCharacter(char32_t _codepoint) noexcept
    {
#if defined(LIBTERMINAL_IMAGES)
        imageFragment_.reset();
#endif
        if (_codepoint)
        {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
            codepoints_.assign(1, _codepoint);
#else
            codepointCount_ = 1;
            codepoints_[0] = _codepoint;
#endif
            width_ = static_cast<uint8_t>(std::max(unicode::width(_codepoint), 1));
        }
        else
        {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
            codepoints_.clear();
#else
            codepointCount_ = 0;
#endif
            width_ = 1;
        }
    }

    void setWidth(uint8_t _width) noexcept
    {
        width_ = _width;
    }

    int appendCharacter(char32_t _codepoint) noexcept
    {
#if defined(LIBTERMINAL_IMAGES)
        imageFragment_.reset();
#endif
        if (codepointCount() < MaxCodepoints)
        {
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
            codepoints_.push_back(_codepoint);
#else
            codepoints_[codepointCount_++] = _codepoint;
#endif

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
                width_ = static_cast<uint8_t>(width);
                return diff;
            }
        }
        return 0;
    }

    void setAttributes(GraphicsAttributes _attributes) noexcept
    {
        attributes_ = _attributes;
    }

    std::string toUtf8() const;

#if defined(LIBTERMINAL_HYPERLINKS)
    HyperlinkRef hyperlink() const noexcept { return hyperlink_; }
    void setHyperlink(HyperlinkRef const& _hyperlink) { hyperlink_ = _hyperlink; }
#endif

  private:
    /// Unicode codepoint to be displayed.
#if defined(CONTOUR_TERMINAL_CELL_USE_STRING)
    std::u32string codepoints_;
#else
    uint8_t codepointCount_;
    std::array<char32_t, 8> codepoints_;
#endif

    /// number of cells this cell spans. Usually this is 1, but it may be also 0 or >= 2.
    uint8_t width_;

    /// Graphics renditions, such as foreground/background color or other grpahics attributes.
    GraphicsAttributes attributes_;

#if defined(LIBTERMINAL_HYPERLINKS)
    HyperlinkRef hyperlink_ = nullptr;
#endif

    /// Image fragment to be rendered in this cell.
#if defined(LIBTERMINAL_IMAGES)
    std::optional<ImageFragment> imageFragment_;
#endif
};

inline bool operator==(Cell const& a, Cell const& b) noexcept
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

class Line { // {{{
  public:
    enum class Flags : uint8_t {
        None      = 0x0000,
        Wrappable = 0x0001,
        Wrapped   = 0x0002,
        Marked    = 0x0004,
    };

    using Buffer = std::vector<Cell>;
    using iterator = Buffer::iterator;
    using const_iterator = Buffer::const_iterator;
    using reverse_iterator = Buffer::reverse_iterator;

    Line(ColumnCount _numCols, Cell const& _defaultCell, Flags _flags) :
        buffer_(unbox<size_t>(_numCols), _defaultCell),
        flags_{static_cast<unsigned>(_flags)}
    {}

    Line(Buffer const& _init, Flags _flags) : Line(Buffer(_init), _flags) {}
    Line(Buffer&& _init, Flags _flags);
    Line(iterator const& _begin, iterator const& _end, Flags _flags);
    Line(ColumnCount _numCols, Buffer&& _init, Flags _flags);
    Line(ColumnCount _numCols, std::string_view const& _s, Flags _flags);

    Buffer& buffer() noexcept { return buffer_; }

    Line() = default;
    Line(Line const&) = default;
    Line(Line&&) = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) = default;

    void reset(GraphicsAttributes _attributes) noexcept
    {
        for (Cell& cell: buffer_)
            cell.reset(_attributes);
    }

    Buffer* operator->() noexcept { return &buffer_; }
    Buffer const* operator->() const noexcept { return &buffer_; }
    auto& operator[](std::size_t _index) { return buffer_[_index]; }
    auto const& operator[](std::size_t _index) const { return buffer_[_index]; }

    void prepend(Buffer const&);
    void append(Buffer const&);
    void append(int _count, Cell const& _initial);

    Buffer remove(iterator const& _from, iterator const& _to);

    /// Shhift left by @p _count cells and fill right with cells of @p _fill.
    ///
    /// @returns sequence of cells that have been shifted out.
    Buffer shift_left(int _count, Cell const& _fill);

    crispy::range<const_iterator> trim_blank_right() const;

    ColumnCount size() const noexcept { return ColumnCount::cast_from(buffer_.size()); }

    bool blank() const noexcept;

    // TODO (trimmed version of size()): int maxOccupiedColumns() const noexcept { return size(); }

    void resize(ColumnCount _size);
    [[nodiscard]] Buffer reflow(ColumnCount _column);

    iterator begin() { return buffer_.begin(); }
    iterator end() { return buffer_.end(); }
    const_iterator begin() const { return buffer_.begin(); }
    const_iterator end() const { return buffer_.end(); }
    reverse_iterator rbegin() { return buffer_.rbegin(); }
    reverse_iterator rend() { return buffer_.rend(); }
    const_iterator cbegin() const { return buffer_.cbegin(); }
    const_iterator cend() const { return buffer_.cend(); }

    bool marked() const noexcept { return isFlagEnabled(Flags::Marked); }
    void setMarked(bool _enable) { setFlag(Flags::Marked, _enable); }

    bool wrapped() const noexcept { return isFlagEnabled(Flags::Wrapped); }
    void setWrapped(bool _enable) { setFlag(Flags::Wrapped, _enable); }

    bool wrappable() const noexcept { return isFlagEnabled(Flags::Wrappable); }
    void setWrappable(bool _enable) { setFlag(Flags::Wrappable, _enable); }

    Flags wrappableFlag() const noexcept { return wrappable() ? Line::Flags::Wrappable : Line::Flags::None; }
    Flags markedFlag() const noexcept { return marked() ? Line::Flags::Marked : Line::Flags::None; }

    std::string toUtf8() const;
    std::string toUtf8Trimmed() const;

    void setText(std::string_view _u8string);

    Flags flags() const noexcept { return static_cast<Flags>(flags_); }

    Flags inheritableFlags() const noexcept
    {
        auto constexpr Inheritables = unsigned(Flags::Wrappable)
                                    | unsigned(Flags::Marked);
        return static_cast<Flags>(flags_ & Inheritables);
    }

    void setFlag(Flags _flag, bool _enable) noexcept
    {
        if (_enable)
            flags_ |= static_cast<unsigned>(_flag);
        else
            flags_ &= ~static_cast<unsigned>(_flag);
    }

    bool isFlagEnabled(Flags _flag) const noexcept { return (flags_ & static_cast<unsigned>(_flag)) != 0; }

  private:
    Buffer buffer_;
    unsigned flags_;
};

constexpr Line::Flags operator|(Line::Flags a, Line::Flags b) noexcept
{
    return Line::Flags(unsigned(a) | unsigned(b));
}

constexpr bool operator&(Line::Flags a, Line::Flags b) noexcept
{
    return (unsigned(a) & unsigned(b)) != 0;
}
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
 * Manages the screen grid buffer (main screen + scrollback history).
 *
 * <h3>Future motivations</h3>
 *
 * <ul>
 *   <li>manages text reflow upon resize
 *   <li>manages underlying disk storage for very old scrollback history lines.
 * </ul>
 *
 * <h3>Layout</h3>
 *
 * <pre>
 *      +0========================-3+   <-- scrollback top
 *      |1                        -2|
 *      |2   Scrollback history   -1|
 *      |3                         0|   <-- scrollback bottom
 *      +4-------------------------1+   <-- main page top
 *      |5                         2|
 *      |6   main page area        3|
 *      |7                         4|   <-- main page bottom
 *      +---------------------------+
 *       ^                          ^
 *       1                          screenSize.columns
 * </pre>
 */
class Grid {
  public:
    // TODO: Rename all "History" to "Scrollback"?

    Grid(PageSize _screenSize, bool _reflowOnResize, std::optional<LineCount> _maxHistoryLineCount);

    Grid(): Grid(PageSize{LineCount(25), ColumnCount(80)}, false, LineCount(0)) {}

    PageSize screenSize() const noexcept { return screenSize_; }

    /// Resizes the main page area of the grid and adapts the scrollback area's width accordingly.
    ///
    /// @param _screenSize          new size of the main page area
    /// @param _currentCursorPos    current cursor position
    /// @param _wrapPending         indicates whether a cursor wrap is pending before the next text write.
    ///
    /// @returns updated cursor position.
    Coordinate resize(PageSize _screenSize, Coordinate _currentCursorPos, bool _wrapPending);

    std::optional<LineCount> maxHistoryLineCount() const noexcept { return maxHistoryLineCount_; }
    void setMaxHistoryLineCount(std::optional<LineCount> _maxHistoryLineCount);

    bool reflowOnResize() const noexcept { return reflowOnResize_; }
    void setReflowOnResize(bool _enabled) { reflowOnResize_ = _enabled; }

    LineCount historyLineCount() const noexcept
    {
        return LineCount::cast_from(lines_.size()) - screenSize_.lines;
    }

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    void render(RendererT && _render, std::optional<StaticScrollbackPosition> _scrollOffset = std::nullopt) const;

    Line& absoluteLineAt(int _line) noexcept;
    Line const& absoluteLineAt(int _line) const noexcept;

    /// @returns reference to Line at given relative offset @p _line.
    Line& lineAt(int _line) noexcept;
    Line const& lineAt(int _line) const noexcept;

    /// Converts a relative line number into an absolute line number.
    int toAbsoluteLine(int _relativeLine) const noexcept;

    /// Converts an absolute line number into a relative line number.
    int toRelativeLine(int _absoluteLine) const noexcept;

    int computeRelativeLineNumberFromBottom(int _n) const noexcept;

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell& at(Coordinate const& _coord) noexcept;

    /// Gets a reference to the cell relative to screen origin (top left, 1:1).
    Cell const& at(Coordinate const& _coord) const noexcept;

    crispy::range<Lines::const_iterator> lines(LinePosition _start, LinePosition _end) const;
    crispy::range<Lines::iterator> lines(LinePosition _start, LinePosition _end);
    // TODO: ^^ these are actually of type HistoryLinePostiion ^^

    crispy::range<Lines::const_iterator> pageAtScrollOffset(std::optional<StaticScrollbackPosition> _scrollOffset) const;
    crispy::range<Lines::iterator> pageAtScrollOffset(std::optional<StaticScrollbackPosition> _scrollOffset);

    crispy::range<Lines::const_iterator> mainPage() const;
    crispy::range<Lines::iterator> mainPage();

    crispy::range<Lines::const_iterator> scrollbackLines() const;

    /// Completely deletes all scrollback lines.
    void clearHistory();

    /// Scrolls up by @p _n lines within the given margin.
    ///
    /// @param _n number of lines to scroll up within the given margin.
    /// @param _defaultAttributes SGR attributes the newly created grid cells will be initialized with.
    /// @param _margin the margin coordinates to perform the scrolling action into.
    void scrollUp(LineCount _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin);

    /// Scrolls down by @p _n lines within the given margin.
    ///
    /// @param _n number of lines to scroll down within the given margin.
    /// @param _defaultAttributes SGR attributes the newly created grid cells will be initialized with.
    /// @param _margin the margin coordinates to perform the scrolling action into.
    void scrollDown(LineCount _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin);

    std::string renderTextLineAbsolute(int row) const;
    std::string renderTextLine(int row) const;
    std::string renderText() const;

    /// Renders the full grid's text characters.
    ///
    /// Empty cells are represented as strings and lines split by LF.
    std::string renderAllText() const;

  private:
    /// Ensures the maxHistoryLineCount attribute will be satisified, potentially deleting any
    /// overflowing history line.
    void clampHistory();
    void appendNewLines(LineCount _count, GraphicsAttributes _attr);

    // private fields
    //
    PageSize screenSize_;
    bool reflowOnResize_;
    std::optional<LineCount> maxHistoryLineCount_;
    Lines lines_;
};

// {{{ inlines
template <typename RendererT>
inline void Grid::render(RendererT && _render, std::optional<StaticScrollbackPosition> _scrollOffset) const
{
    for (auto const && [rowNumber, line] : crispy::indexed(pageAtScrollOffset(_scrollOffset), 1))
    {
        for (auto const && [colNumber, column] : crispy::indexed(line, 1))
            _render({rowNumber, colNumber}, column);

        auto const columnCount = std::max(
            ColumnCount(0),
            screenSize_.columns - line.size()
        );
        for (auto const colNumber : crispy::times(unbox<int>(line.size()) + 1, unbox<int>(columnCount)))
            _render({rowNumber, colNumber}, Cell{});
    }
}

inline Line& Grid::absoluteLineAt(int _line) noexcept
{
    assert(crispy::ascending(0, _line, static_cast<int>(lines_.size()) - 1));
    return *next(lines_.begin(), _line);
}

inline Line const& Grid::absoluteLineAt(int _line) const noexcept
{
    return const_cast<Grid&>(*this).absoluteLineAt(_line);
}

inline Line& Grid::lineAt(int _line) noexcept
{
    assert(crispy::ascending(1 - *historyLineCount(), _line, *screenSize_.lines));

    return *next(lines_.begin(), *historyLineCount() + _line - 1);
}

inline Line const& Grid::lineAt(int _line) const noexcept
{
    return const_cast<Grid&>(*this).lineAt(_line);
}

inline int Grid::toAbsoluteLine(int _relativeLine) const noexcept
{
    return *historyLineCount() + _relativeLine - 1;
}

inline int Grid::toRelativeLine(int _absoluteLine) const noexcept
{
    return _absoluteLine - *historyLineCount();
}

inline Cell& Grid::at(Coordinate const& _coord) noexcept
{
    assert(crispy::ascending(1 - unbox<int>(historyLineCount()), _coord.row, unbox<int>(screenSize_.lines)));
    assert(crispy::ascending(1, _coord.column, unbox<int>(screenSize_.columns)));

    if (_coord.row > 0)
        return (*next(lines_.rbegin(), unbox<int>(screenSize_.lines) - _coord.row))[static_cast<size_t>(_coord.column - 1)];
    else
        return (*next(lines_.begin(), unbox<int>(historyLineCount()) + _coord.row - 1))[static_cast<size_t>(_coord.column - 1)];
}

inline Cell const& Grid::at(Coordinate const& _coord) const noexcept
{
    return const_cast<Grid&>(*this).at(_coord);
}

inline crispy::range<Lines::const_iterator> Grid::lines(LinePosition _start, LinePosition _end) const
{
    assert(crispy::ascending(0, *_start, int(lines_.size()) - 1) && "Absolute scroll offset must not be negative or overflowing.");
    assert(crispy::ascending(_start, _end, LinePosition::cast_from(lines_.size() - 1)) && "Absolute scroll offset must not be negative or overflowing.");

    return crispy::range<Lines::const_iterator>(
        next(lines_.cbegin(), unbox<long>(_start)),
        next(lines_.cbegin(), unbox<long>(_end))
    );
}

inline crispy::range<Lines::iterator> Grid::lines(LinePosition _start, LinePosition _end)
{
    assert(crispy::ascending(LinePosition{0}, _start, LinePosition::cast_from(lines_.size())) && "Absolute scroll offset must not be negative or overflowing.");
    assert(crispy::ascending(_start, _end, LinePosition::cast_from(lines_.size())) && "Absolute scroll offset must not be negative or overflowing.");

    return crispy::range<Lines::iterator>(
        next(lines_.begin(), unbox<long>(_start)),
        next(lines_.begin(), unbox<long>(_end))
    );
}

inline crispy::range<Lines::const_iterator> Grid::pageAtScrollOffset(std::optional<StaticScrollbackPosition> _scrollOffset) const
{
    assert(
        crispy::ascending(
            StaticScrollbackPosition(0),
            _scrollOffset.value_or(StaticScrollbackPosition{0}),
            boxed_cast<StaticScrollbackPosition>(historyLineCount())
        ) &&
        "Absolute scroll offset must not be negative or overflowing."
    );

    auto const start = std::next(lines_.cbegin(),
                                 unbox<long>(_scrollOffset.value_or(boxed_cast<StaticScrollbackPosition>(historyLineCount()))));
    auto const end = std::next(start, unbox<long>(screenSize_.lines));

    return crispy::range<Lines::const_iterator>(start, end);
}

inline crispy::range<Lines::iterator> Grid::pageAtScrollOffset(std::optional<StaticScrollbackPosition> _scrollOffset)
{
    assert(
        crispy::ascending(
            StaticScrollbackPosition{0},
            _scrollOffset.value_or(StaticScrollbackPosition{0}),
            boxed_cast<StaticScrollbackPosition>(historyLineCount())
        ) &&
        "Absolute scroll offset must not be negative or overflowing."
    );

    return crispy::range<Lines::iterator>(
        std::next(
            lines_.begin(),
            unbox<long>(_scrollOffset.value_or(boxed_cast<StaticScrollbackPosition>(historyLineCount())))
        ),
        lines_.end()
    );
}

inline crispy::range<Lines::const_iterator> Grid::mainPage() const
{
    return pageAtScrollOffset(std::nullopt);
}

inline crispy::range<Lines::iterator> Grid::mainPage()
{
    return pageAtScrollOffset(std::nullopt);
}

inline crispy::range<Lines::const_iterator> Grid::scrollbackLines() const
{
    return crispy::range<Lines::const_iterator>(
        lines_.cbegin(),
        std::next(
            lines_.cbegin(),
            unbox<long>(historyLineCount())
        )
    );
}
// }}}

} // end namespace

namespace fmt {
    template <>
    struct formatter<terminal::Line::Flags> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::Line::Flags _flags, FormatContext& ctx)
        {
            static const std::array<std::pair<terminal::Line::Flags, std::string_view>, 3> nameMap = {
                std::pair{ terminal::Line::Flags::Wrappable, std::string_view("Wrappable")},
                std::pair{ terminal::Line::Flags::Wrapped, std::string_view("Wrapped")},
                std::pair{ terminal::Line::Flags::Marked, std::string_view("Marked")}
            };
            std::string s;
            for (auto const& mapping : nameMap)
            {
                if (mapping.first & _flags)
                {
                    if (!s.empty())
                        s += ",";
                    s += mapping.second;
                }
            }
            return format_to(ctx.out(), s);
        }
    };
}
