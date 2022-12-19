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

#include <vtbackend/CellUtil.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/cell/CellConcept.h>
#include <vtbackend/primitives.h>

#include <crispy/BufferObject.h>
#include <crispy/Comparison.h>
#include <crispy/assert.h>

#include <unicode/convert.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace terminal
{

enum class LineFlags : uint8_t
{
    None = 0x0000,
    Trivial = 0x0001,
    Wrappable = 0x0002,
    Wrapped = 0x0004,
    Marked = 0x0008,
    // TODO: DoubleWidth  = 0x0010,
    // TODO: DoubleHeight = 0x0020,
};

constexpr LineFlags operator~(LineFlags a) noexcept
{
    return LineFlags(~unsigned(a));
}

constexpr LineFlags operator|(LineFlags a, LineFlags b) noexcept
{
    return LineFlags(uint8_t(a) | uint8_t(b));
}

constexpr LineFlags operator&(LineFlags a, LineFlags b) noexcept
{
    return LineFlags(uint8_t(a) & uint8_t(b));
}

// clang-format off
template <typename, bool> struct OptionalProperty;
template <typename T> struct OptionalProperty<T, false> {};
template <typename T> struct OptionalProperty<T, true> { T value; };
// clang-format on

/**
 * Line storage with call columns sharing the same SGR attributes.
 */
struct TrivialLineBuffer
{
    ColumnCount displayWidth;
    GraphicsAttributes textAttributes;
    GraphicsAttributes fillAttributes = textAttributes;
    HyperlinkId hyperlink {};

    ColumnCount usedColumns {};
    crispy::BufferFragment<char> text {};

    void reset(GraphicsAttributes _attributes) noexcept
    {
        textAttributes = _attributes;
        fillAttributes = _attributes;
        hyperlink = {};
        usedColumns = {};
        text.reset();
    }
};

/**
 * Line<Cell> API.
 *
 * Highlevel line API reflecting either a trivial line or an inflated line.
 */
template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
class Line
{
  public:
    // {{{ defaults
    Line() = default;
    Line(Line const&) = default;
    Line(Line&&) noexcept = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) noexcept = default;
    // }}}

    // {{{ type defs
    using TrivialBuffer = TrivialLineBuffer;
    using InflatedBuffer = gsl::span<Cell>;

    using value_type = Cell;
    using iterator = typename InflatedBuffer::iterator;
    using reverse_iterator = typename InflatedBuffer::reverse_iterator;
    using const_iterator = typename InflatedBuffer::const_iterator;
    // }}}

    // Initializes thius line with the given attributes.
    //
    // @param displayWidth    the number of columns this line will span.
    // @param flags           line flags
    // @param attributes      default attributes for each cell.
    // @param inflatedBuffer  storage to be used for the inflated buffer.
    Line(ColumnCount displayWidth, LineFlags flags, GraphicsAttributes attributes, InflatedBuffer inflatedBuffer);

    // @returns number of columns this line maintains
    [[nodiscard]] ColumnCount size() const noexcept { return _displayWidth; }

    // Resets the current line with the given attribvutes.
    void reset(LineFlags flags, GraphicsAttributes attributes) noexcept;

    // TODO(pr) this function will become interesting. :-)
    void resize(ColumnCount _count);

    // TODO(pr) this function will become interesting. :-)
    [[nodiscard]] InflatedBuffer reflow(ColumnCount _newColumnCount);

    // ==========================================================

    // Fills each cell in the complete line with the given data.
    void fill(LineFlags flags, GraphicsAttributes attributes, char32_t codepoint, uint8_t width) noexcept;

    /// Tests if all cells are empty.
    [[nodiscard]] bool empty() const noexcept;

    /**
     * Fills this line with the given content.
     *
     * @param start   offset into this line of the first charater
     * @param sgr     graphics rendition for the line starting at @c _start until the end
     * @param text    the US-ASCII characters to fill with
     */
    void fill(ColumnOffset start, GraphicsAttributes sgr, std::string_view text);

    // {{{ inflated line range accessors
    [[nodiscard]] gsl::span<Cell const> trim_blank_right() const noexcept;
    [[nodiscard]] gsl::span<Cell const> cells() const noexcept { return inflatedBuffer(); }
    [[nodiscard]] gsl::span<Cell> useRange(ColumnOffset _start, ColumnCount _count) noexcept;

    // Returns a reference to this mutable grid-line buffer.
    //
    // If this line has been stored in an optimized state, then
    // the line will be first unpacked into a vector of grid cells.
    InflatedBuffer& inflatedBuffer();
    InflatedBuffer const& inflatedBuffer() const;

    [[nodiscard]] TrivialBuffer& trivialBuffer() noexcept { return _trivialBuffer; }
    [[nodiscard]] TrivialBuffer const& trivialBuffer() const noexcept { return _trivialBuffer; }

    // }}}

    // {{{ high level accessors to column properties
    [[nodiscard]] Cell& useCellAt(ColumnOffset _column) noexcept;
    [[nodiscard]] uint8_t cellEmptyAt(ColumnOffset column) const noexcept;
    [[nodiscard]] uint8_t cellWidthAt(ColumnOffset column) const noexcept;
    // }}}

    // {{{ line flags
    [[nodiscard]] LineFlags flags() const noexcept { return static_cast<LineFlags>(_flags); }

    [[nodiscard]] bool isTrivialBuffer() const noexcept { return isFlagEnabled(LineFlags::Trivial); }
    [[nodiscard]] bool isInflatedBuffer() const noexcept { return !isFlagEnabled(LineFlags::Trivial); }

    [[nodiscard]] bool marked() const noexcept { return isFlagEnabled(LineFlags::Marked); }
    void setMarked(bool _enable) { setFlag(LineFlags::Marked, _enable); }

    [[nodiscard]] bool wrapped() const noexcept { return isFlagEnabled(LineFlags::Wrapped); }
    void setWrapped(bool _enable) { setFlag(LineFlags::Wrapped, _enable); }

    [[nodiscard]] bool wrappable() const noexcept { return isFlagEnabled(LineFlags::Wrappable); }
    void setWrappable(bool _enable) { setFlag(LineFlags::Wrappable, _enable); }

    [[nodiscard]] LineFlags wrappableFlag() const noexcept
    {
        return wrappable() ? LineFlags::Wrappable : LineFlags::None;
    }

    [[nodiscard]] LineFlags wrappedFlag() const noexcept
    {
        return marked() ? LineFlags::Wrapped : LineFlags::None;
    }

    [[nodiscard]] LineFlags markedFlag() const noexcept
    {
        return marked() ? LineFlags::Marked : LineFlags::None;
    }

    [[nodiscard]] LineFlags inheritableFlags() const noexcept
    {
        auto constexpr Inheritables = unsigned(LineFlags::Wrappable) | unsigned(LineFlags::Marked);
        return static_cast<LineFlags>(_flags & Inheritables);
    }

    void setFlag(LineFlags flag, bool enable) noexcept
    {
        if (enable)
            _flags |= static_cast<unsigned>(flag);
        else
            _flags &= ~static_cast<unsigned>(flag);
    }

    [[nodiscard]] bool isFlagEnabled(LineFlags flag) const noexcept
    {
        return (_flags & static_cast<unsigned>(flag)) != 0;
    }
    // }}}

    // {{{ convenience helpers
    // Constructs an UTF-8 string containing the textual part of this line,
    // including trailing whitespace.
    // Be aware. This may be expensive!
    [[nodiscard]] std::string toUtf8() const;

    // Constructs an UTF-8 string containing the textual part of this line,
    // excluding trailing whitespace.
    // Be aware. This may be expensive!
    [[nodiscard]] std::string toUtf8Trimmed() const;

    // Tests if the given text can be matched in this line at the exact given start column.
    [[nodiscard]] bool matchTextAt(std::u32string_view text, ColumnOffset startColumn) const noexcept;

    // Search a line from left to right if a complete match is found it returns the Column of
    // start of match and partialMatchLength is set to 0, since it's a full match but if a partial
    // match is found at the right end of line it returns startColumn as it is and the partialMatchLength
    // is set equal to match found at the left end of line.
    [[nodiscard]] std::optional<SearchResult> search(std::u32string_view text,
                                                     ColumnOffset startColumn) const noexcept;

    // Search a line from right to left if a complete match is found it returns the Column of
    // start of match and  partialMatchLength is set to 0, since it's a full match but if a partial
    // match is found at the left end of line it returns startColumn as it is and the partialMatchLength
    // is set equal to match found at the left end of line.
    [[nodiscard]] std::optional<SearchResult> searchReverse(std::u32string_view text,
                                                            ColumnOffset startColumn) const noexcept;
    // }}}

  private:
    // Ensures this line is using the underlying inflated storage rather the trivial line storage.
    void inflate() noexcept;

    ColumnCount _displayWidth;
    unsigned _flags = 0;
    TrivialLineBuffer _trivialBuffer;
    InflatedBuffer _inflatedBuffer;
};

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline typename Line<Cell>::InflatedBuffer& Line<Cell>::inflatedBuffer()
{
    if (isTrivialBuffer())
        inflate();
    return _inflatedBuffer;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
inline typename Line<Cell>::InflatedBuffer const& Line<Cell>::inflatedBuffer() const
{
    return const_cast<Line<Cell>*>(this)->inflatedBuffer();
}

} // namespace terminal

namespace fmt // {{{
{
template <>
struct formatter<terminal::LineFlags>
{
    template <typename ParseContext>
    auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const terminal::LineFlags _flags, FormatContext& ctx) const
    {
        static const std::array<std::pair<terminal::LineFlags, std::string_view>, 3> nameMap = {
            std::pair { terminal::LineFlags::Wrappable, std::string_view("Wrappable") },
            std::pair { terminal::LineFlags::Wrapped, std::string_view("Wrapped") },
            std::pair { terminal::LineFlags::Marked, std::string_view("Marked") },
        };
        std::string s;
        for (auto const& mapping: nameMap)
        {
            if ((mapping.first & _flags) != terminal::LineFlags::None)
            {
                if (!s.empty())
                    s += ",";
                s += mapping.second;
            }
        }
        return fmt::format_to(ctx.out(), "{}", s);
    }
};
} // namespace fmt
