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
#include <terminal/Coordinate.h>
#include <terminal/Color.h>
#include <terminal/Hyperlink.h>
#include <terminal/Image.h>

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

// {{{ GraphicsAttributes
/// Character graphics rendition information.
struct GraphicsAttributes {
    Color foregroundColor{DefaultColor{}};
    Color backgroundColor{DefaultColor{}};
    Color underlineColor{DefaultColor{}};
    CharacterStyleMask styles{};

    RGBColor getUnderlineColor(ColorPalette const& _colorPalette) const noexcept
    {
        float const opacity = [=]() {
            if (styles & CharacterStyleMask::Faint)
                return 0.5f;
            else
                return 1.0f;
        }();

        bool const bright = (styles & CharacterStyleMask::Bold) != 0;
        return apply(_colorPalette, underlineColor, ColorTarget::Foreground, bright) * opacity;
    }

    std::pair<RGBColor, RGBColor> makeColors(ColorPalette const& _colorPalette, bool _reverseVideo) const noexcept
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

    constexpr bool empty() const noexcept { return codepointCount_ == 0 && !imageFragment_; }

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

class Line { // {{{
  public:
    enum class Flags : uint8_t {
        None      = 0x0000,
        Wrappable = 0x0001,
        Wrapped   = 0x0002,
        Marked    = 0x0004,
    };

    using Buffer = std::deque<Cell>;
    using iterator = Buffer::iterator;
    using const_iterator = Buffer::const_iterator;
    using reverse_iterator = Buffer::reverse_iterator;

    Line(int _numCols, Cell const& _defaultCell, Flags _flags) :
        buffer_(static_cast<size_t>(_numCols), _defaultCell),
        flags_{static_cast<unsigned>(_flags)}
    {}

    Line(Buffer const& _init, Flags _flags) : Line(Buffer(_init), _flags) {}
    Line(Buffer&& _init, Flags _flags);
    Line(iterator const& _begin, iterator const& _end, Flags _flags);
    Line(int _numCols, Buffer&& _init, Flags _flags);
    Line(int _numCols, std::string_view const& _s, Flags _flags);

    Buffer& buffer() noexcept { return buffer_; }

    Line() = default;
    Line(Line const&) = default;
    Line(Line&&) = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) = default;

    Buffer* operator->() noexcept { return &buffer_; }
    Buffer const* operator->()  const noexcept { return &buffer_; }
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

    int size() const noexcept { return static_cast<int>(buffer_.size()); }

    bool blank() const noexcept;

    // TODO (trimmed version of size()): int maxOccupiedColumns() const noexcept { return size(); }

    void resize(int _size);
    [[nodiscard]] Buffer reflow(int _column);

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

    Grid(crispy::Size _screenSize, bool _reflowOnResize, std::optional<int> _maxHistoryLineCount);

    Grid() : Grid(crispy::Size{80, 25}, false, 0) {}

    crispy::Size screenSize() const noexcept { return screenSize_; }

    /// Resizes the main page area of the grid and adapts the scrollback area's width accordingly.
    ///
    /// @param _screenSize          new size of the main page area
    /// @param _currentCursorPos    current cursor position
    /// @param _wrapPending         indicates whether a cursor wrap is pending before the next text write.
    ///
    /// @returns updated cursor position.
    Coordinate resize(crispy::Size _screenSize, Coordinate _currentCursorPos, bool _wrapPending);

    std::optional<int> maxHistoryLineCount() const noexcept { return maxHistoryLineCount_; }
    void setMaxHistoryLineCount(std::optional<int> _maxHistoryLineCount);

    bool reflowOnResize() const noexcept { return reflowOnResize_; }
    void setReflowOnResize(bool _enabled) { reflowOnResize_ = _enabled; }

    int historyLineCount() const noexcept { return static_cast<int>(lines_.size()) - screenSize_.height; }

    /// Renders the full screen by passing every grid cell to the callback.
    template <typename RendererT>
    void render(RendererT && _render, std::optional<int> _scrollOffset = std::nullopt) const;

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

    crispy::range<Lines::const_iterator> lines(int _start, int _count) const;
    crispy::range<Lines::iterator> lines(int _start, int _count);

    crispy::range<Lines::const_iterator> pageAtScrollOffset(std::optional<int> _scrollOffset) const;
    crispy::range<Lines::iterator> pageAtScrollOffset(std::optional<int> _scrollOffset);

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
    void scrollUp(int _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin);

    /// Scrolls down by @p _n lines within the given margin.
    ///
    /// @param _n number of lines to scroll down within the given margin.
    /// @param _defaultAttributes SGR attributes the newly created grid cells will be initialized with.
    /// @param _margin the margin coordinates to perform the scrolling action into.
    void scrollDown(int _n, GraphicsAttributes const& _defaultAttributes, Margin const& _margin);

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
    void appendNewLines(int _count, GraphicsAttributes _attr);

  private:
    crispy::Size screenSize_;
    bool reflowOnResize_;
    std::optional<int> maxHistoryLineCount_;
    Lines lines_;
};

// {{{ inlines
template <typename RendererT>
inline void Grid::render(RendererT && _render, std::optional<int> _scrollOffset) const
{
    for (auto const && [rowNumber, line] : crispy::indexed(pageAtScrollOffset(_scrollOffset), 1))
    {
        for (auto const && [colNumber, column] : crispy::indexed(line, 1))
            _render({rowNumber, colNumber}, column);

        for (auto const colNumber : crispy::times(line.size() + 1, std::max(0, screenSize_.width - line.size())))
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
    assert(crispy::ascending(1 - historyLineCount(), _line, screenSize_.height));

    return *next(lines_.begin(), historyLineCount() + _line - 1);
}

inline Line const& Grid::lineAt(int _line) const noexcept
{
    return const_cast<Grid&>(*this).lineAt(_line);
}

inline int Grid::toAbsoluteLine(int _relativeLine) const noexcept
{
    return historyLineCount() + _relativeLine - 1;
}

inline int Grid::toRelativeLine(int _absoluteLine) const noexcept
{
    return _absoluteLine - historyLineCount();
}

inline Cell& Grid::at(Coordinate const& _coord) noexcept
{
    assert(crispy::ascending(1 - historyLineCount(), _coord.row, screenSize_.height));
    assert(crispy::ascending(1, _coord.column, screenSize_.width));

    if (_coord.row > 0)
        return (*next(lines_.rbegin(), screenSize_.height - _coord.row))[_coord.column - 1];
    else
        return (*next(lines_.begin(), historyLineCount() + _coord.row - 1))[_coord.column - 1];
}

inline Cell const& Grid::at(Coordinate const& _coord) const noexcept
{
    return const_cast<Grid&>(*this).at(_coord);
}

inline crispy::range<Lines::const_iterator> Grid::lines(int _start, int _end) const
{
    assert(crispy::ascending(0, _start, int(lines_.size()) - 1) && "Absolute scroll offset must not be negative or overflowing.");
    assert(crispy::ascending(_start, _end, int(lines_.size()) - 1) && "Absolute scroll offset must not be negative or overflowing.");

    return crispy::range<Lines::const_iterator>(
        next(lines_.cbegin(), _start),
        next(lines_.cbegin(), _end)
    );
}

inline crispy::range<Lines::iterator> Grid::lines(int _start, int _end)
{
    assert(crispy::ascending(0, _start, int(lines_.size())) && "Absolute scroll offset must not be negative or overflowing.");
    assert(crispy::ascending(_start, _end, int(lines_.size())) && "Absolute scroll offset must not be negative or overflowing.");

    return crispy::range<Lines::iterator>(
        next(lines_.begin(), _start),
        next(lines_.begin(), _end)
    );
}

inline crispy::range<Lines::const_iterator> Grid::pageAtScrollOffset(std::optional<int> _scrollOffset) const
{
    assert(crispy::ascending(0, _scrollOffset.value_or(0), historyLineCount()) && "Absolute scroll offset must not be negative or overflowing.");

    auto const start = std::next(lines_.cbegin(),
                                 static_cast<size_t>(_scrollOffset.value_or(historyLineCount())));
    auto const end = std::next(start, screenSize_.height);

    return crispy::range<Lines::const_iterator>(start, end);
}

inline crispy::range<Lines::iterator> Grid::pageAtScrollOffset(std::optional<int> _scrollOffset)
{
    assert(crispy::ascending(0, _scrollOffset.value_or(0), historyLineCount()) && "Absolute scroll offset must not be negative or overflowing.");

    return crispy::range<Lines::iterator>(
        std::next(
            lines_.begin(),
            static_cast<size_t>(_scrollOffset.value_or(historyLineCount()))
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
            static_cast<size_t>(historyLineCount())
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
