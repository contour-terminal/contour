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

#include <terminal/GraphicsAttributes.h>
#include <terminal/Hyperlink.h>
#include <terminal/primitives.h>

#include <crispy/BufferObject.h>
#include <crispy/Comparison.h>
#include <crispy/assert.h>

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
    Wrappable = 0x0001,
    Wrapped = 0x0002,
    Marked = 0x0004,
    // TODO: DoubleWidth  = 0x0010,
    // TODO: DoubleHeight = 0x0020,
};

// clang-format off
template <typename, bool> struct OptionalProperty;
template <typename T> struct OptionalProperty<T, false> {};
template <typename T> struct OptionalProperty<T, true> { T value; };
// clang-format on

/**
 * Line storage with call columns sharing the same SGR attributes.
 */
struct TriviallyStyledLineBuffer
{
    ColumnCount displayWidth;
    GraphicsAttributes attributes;
    crispy::BufferFragment text {};
    HyperlinkId hyperlink {};
};

template <typename Cell>
using InflatedLineBuffer = std::vector<Cell>;

/// Unpacks a TriviallyStyledLineBuffer into an InflatedLineBuffer<Cell>.
template <typename Cell>
InflatedLineBuffer<Cell> inflate(TriviallyStyledLineBuffer const& input);

template <typename Cell>
using LineStorage = std::variant<TriviallyStyledLineBuffer, InflatedLineBuffer<Cell>>;

/**
 * Line<Cell> API.
 *
 * TODO: Use custom allocator for ensuring cache locality of Cells to sibling lines.
 * TODO: Make the line optimization work.
 */
template <typename Cell, bool Optimize = false>
class Line
{
  public:
    Line() = default;
    Line(Line const&) = default;
    Line(Line&&) noexcept = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) noexcept = default;

    static constexpr inline bool Optimized = Optimize;

    using TrivialBuffer = TriviallyStyledLineBuffer;
    using InflatedBuffer = InflatedLineBuffer<Cell>;
    using Storage = LineStorage<Cell>;
    using value_type = Cell;
    using iterator = typename InflatedBuffer::iterator;
    using reverse_iterator = typename InflatedBuffer::reverse_iterator;
    using const_iterator = typename InflatedBuffer::const_iterator;

    Line(ColumnCount _width, LineFlags _flags, GraphicsAttributes _templateSGR):
        storage_ { InflatedBuffer { _width.as<size_t>(), Cell { _templateSGR } /*, _allocator*/ } },
        flags_ { static_cast<unsigned>(_flags) }
    {
    }

    Line(ColumnCount _width, InflatedBuffer _buffer, LineFlags _flags):
        storage_ { InflatedBuffer { std::move(_buffer) } }, flags_ { static_cast<unsigned>(_flags) }
    {
        inflatedBuffer().resize(unbox<size_t>(_width));
    }

    Line(InflatedBuffer _buffer, LineFlags _flags):
        storage_ { InflatedBuffer { std::move(_buffer) } }, flags_ { static_cast<unsigned>(_flags) }
    {
    }

    void reset(LineFlags _flags, GraphicsAttributes _attributes) noexcept
    {
        flags_ = static_cast<unsigned>(_flags);
        setBuffer(TrivialBuffer { size(), _attributes });
    }

    void fill(LineFlags _flags,
              GraphicsAttributes const& _attributes,
              char32_t _codepoint,
              uint8_t _width) noexcept
    {
        if (_codepoint == 0)
            reset(_flags, _attributes);
        else
        {
            flags_ = static_cast<unsigned>(_flags);
            for (Cell& cell: editable())
            {
                cell.reset();
                cell.write(_attributes, _codepoint, _width);
            }
        }
    }

    /// Tests if all cells are empty.
    bool empty() const noexcept
    {
        if (isTrivialBuffer())
            return trivialBuffer().text.empty();

        for (auto const& cell: inflatedBuffer())
            if (!cell.empty())
                return false;
        return true;
    }

    /**
     * Fills this line with the given content.
     *
     * @p _start offset into this line of the first charater
     * @p _sgr graphics rendition for the line starting at @c _start until the end
     * @p _ascii the US-ASCII characters to fill with
     */
    void fill(ColumnOffset _start, GraphicsAttributes const& _sgr, std::string_view _ascii)
    {
        auto& buffer = editable();

        assert(unbox<size_t>(_start) + _ascii.size() <= buffer.size());

        auto constexpr ASCII_Width = 1;
        auto const* s = _ascii.data();

        Cell* i = &buffer[unbox<size_t>(_start)];
        Cell* e = i + _ascii.size();
        while (i != e)
            (i++)->write(_sgr, static_cast<char32_t>(*s++), ASCII_Width);

        auto const e2 = buffer.data() + buffer.size();
        while (i != e2)
            (i++)->reset();
    }

    ColumnCount size() const noexcept
    {
        if (isTrivialBuffer())
            return trivialBuffer().displayWidth;
        else
            return ColumnCount::cast_from(inflatedBuffer().size());
    }

    void resize(ColumnCount _count);

    gsl::span<Cell const> trim_blank_right() const noexcept;

    gsl::span<Cell const> cells() const noexcept { return editable(); }

    gsl::span<Cell> useRange(ColumnOffset _start, ColumnCount _count) noexcept
    {
#if defined(__clang__) && __clang_major__ <= 11
        auto const bufferSpan = gsl::span(editable());
        return bufferSpan.subspan(unbox<size_t>(_start), unbox<size_t>(_count));
#else
        // Clang <= 11 cannot deal with this (e.g. FreeBSD 13 defaults to Clang 11).
        return gsl::span(editable()).subspan(unbox<size_t>(_start), unbox<size_t>(_count));
#endif
    }

    Cell& useCellAt(ColumnOffset _column) noexcept
    {
        Require(ColumnOffset(0) <= _column);
        Require(_column <= ColumnOffset::cast_from(size())); // Allow off-by-one for sentinel.
        return editable()[unbox<size_t>(_column)];
    }

    uint8_t cellEmptyAt(ColumnOffset column) const noexcept
    {
        if (isTrivialBuffer())
        {
            Require(ColumnOffset(0) <= column);
            Require(column < ColumnOffset::cast_from(size()));
            return unbox<size_t>(column) >= trivialBuffer().text.size();
        }
        return editable().at(unbox<size_t>(column)).empty();
    }

    uint8_t cellWithAt(ColumnOffset column) const noexcept
    {
        if (isTrivialBuffer())
        {
            Require(ColumnOffset(0) <= column);
            Require(column < ColumnOffset::cast_from(size()));
            return 1; // TODO: When trivial line is to support Unicode, this should be adapted here.
        }
        return editable().at(unbox<size_t>(column)).width();
    }

    LineFlags flags() const noexcept { return static_cast<LineFlags>(flags_); }

    bool marked() const noexcept { return isFlagEnabled(LineFlags::Marked); }
    void setMarked(bool _enable) { setFlag(LineFlags::Marked, _enable); }

    bool wrapped() const noexcept { return isFlagEnabled(LineFlags::Wrapped); }
    void setWrapped(bool _enable) { setFlag(LineFlags::Wrapped, _enable); }

    bool wrappable() const noexcept { return isFlagEnabled(LineFlags::Wrappable); }
    void setWrappable(bool _enable) { setFlag(LineFlags::Wrappable, _enable); }

    LineFlags wrappableFlag() const noexcept { return wrappable() ? LineFlags::Wrappable : LineFlags::None; }
    LineFlags wrappedFlag() const noexcept { return marked() ? LineFlags::Wrapped : LineFlags::None; }
    LineFlags markedFlag() const noexcept { return marked() ? LineFlags::Marked : LineFlags::None; }

    LineFlags inheritableFlags() const noexcept
    {
        auto constexpr Inheritables = unsigned(LineFlags::Wrappable) | unsigned(LineFlags::Marked);
        return static_cast<LineFlags>(flags_ & Inheritables);
    }

    void setFlag(LineFlags _flag, bool _enable) noexcept
    {
        if (_enable)
            flags_ |= static_cast<unsigned>(_flag);
        else
            flags_ &= ~static_cast<unsigned>(_flag);
    }

    bool isFlagEnabled(LineFlags _flag) const noexcept
    {
        return (flags_ & static_cast<unsigned>(_flag)) != 0;
    }

    InflatedBuffer reflow(ColumnCount _newColumnCount);
    std::string toUtf8() const;
    std::string toUtf8Trimmed() const;

    // Returns a reference to this mutable grid-line buffer.
    //
    // If this line has been stored in an optimized state, then
    // the line will be first unpacked into a vector of grid cells.
    InflatedBuffer& editable();
    InflatedBuffer const& editable() const;

    TrivialBuffer& trivialBuffer() noexcept { return std::get<TrivialBuffer>(storage_); }
    TrivialBuffer const& trivialBuffer() const noexcept { return std::get<TrivialBuffer>(storage_); }

    InflatedBuffer const& inflatedBuffer() const noexcept { return std::get<InflatedBuffer>(storage_); }
    InflatedBuffer& inflatedBuffer() noexcept { return std::get<InflatedBuffer>(storage_); }

    bool isTrivialBuffer() const noexcept { return std::holds_alternative<TrivialBuffer>(storage_); }
    bool isInflatedBuffer() const noexcept { return std::holds_alternative<TrivialBuffer>(storage_); }

    void setBuffer(TrivialBuffer const& buffer) { storage_ = buffer; }
    void setBuffer(InflatedBuffer buffer) { storage_ = std::move(buffer); }

    void reset(GraphicsAttributes attributes, HyperlinkId hyperlink, crispy::BufferFragment text)
    {
        storage_ = TrivialBuffer { size(), attributes, std::move(text), hyperlink };
    }

  private:
    Storage storage_;
    unsigned flags_ = 0;
};

constexpr LineFlags operator|(LineFlags a, LineFlags b) noexcept
{
    return LineFlags(unsigned(a) | unsigned(b));
}

constexpr LineFlags operator~(LineFlags a) noexcept
{
    return LineFlags(~unsigned(a));
}

constexpr LineFlags operator&(LineFlags a, LineFlags b) noexcept
{
    return LineFlags(unsigned(a) & unsigned(b));
}

template <typename Cell, bool Optimize>
inline typename Line<Cell, Optimize>::InflatedBuffer& Line<Cell, Optimize>::editable()
{
    if (1) // constexpr (Optimized)
        if (std::holds_alternative<TrivialBuffer>(storage_))
            storage_ = inflate<Cell>(std::get<TrivialBuffer>(storage_));
    return std::get<InflatedBuffer>(storage_);
}

template <typename Cell, bool Optimize>
inline typename Line<Cell, Optimize>::InflatedBuffer const& Line<Cell, Optimize>::editable() const
{
    return const_cast<Line<Cell, Optimize>*>(this)->editable();
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
        return format_to(ctx.out(), s);
    }
};
} // namespace fmt
