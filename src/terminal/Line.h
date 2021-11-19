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

#include <terminal/primitives.h>
#include <terminal/GraphicsAttributes.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <crispy/Comparison.h>
#include <crispy/assert.h>

#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace terminal
{

enum class LineFlags : uint8_t
{
    None      = 0x0000,
    Wrappable = 0x0001,
    Wrapped   = 0x0002,
    Marked    = 0x0004,
    // TODO: DoubleWidth  = 0x0010,
    // TODO: DoubleHeight = 0x0020,
};

template <typename, bool> struct OptionalProperty;
template <typename T> struct OptionalProperty<T, false> {};
template <typename T> struct OptionalProperty<T, true> { T value; };

struct SimpleLineBuffer
{
    GraphicsAttributes attributes;
    std::string text; // TODO: Try std::string_view later to avoid scattered copies.
    ColumnCount width; // page display width
};

template <typename Cell>
using InflatedLineBuffer = std::vector<Cell>;

template <typename Cell>
InflatedLineBuffer<Cell> inflate(SimpleLineBuffer const& input);

template <typename Cell>
using LineStorage = std::variant<SimpleLineBuffer, InflatedLineBuffer<Cell>>;

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

    using InflatedBuffer = InflatedLineBuffer<Cell>;
    using Storage = LineStorage<Cell>;
    using value_type = Cell;
    using iterator = typename InflatedBuffer::iterator;
    using const_iterator = typename InflatedBuffer::const_iterator;

    Line(ColumnCount _width, LineFlags _flags, Cell _template = {}):
        buffer_(_width.as<size_t>(), _template /*, _allocator*/),
        flags_{static_cast<unsigned>(_flags)}
    {}

    Line(ColumnCount _width, InflatedBuffer _buffer, LineFlags _flags):
        buffer_{std::move(_buffer)},
        flags_{static_cast<unsigned>(_flags)}
    {
        buffer_.resize(unbox<size_t>(_width));
    }

    Line(InflatedBuffer _buffer, LineFlags _flags):
        buffer_{std::move(_buffer)},
        flags_{static_cast<unsigned>(_flags)}
    {}

    constexpr static inline bool ColumnOptimized = Optimize;

    // This is experimental (aka. buggy) and going to be replaced with another optimization idea soon.
    //#define LINE_AVOID_CELL_RESET 1

    void reset(LineFlags _flags, GraphicsAttributes _attributes) noexcept // TODO: optimize by having no need to O(n) iterate through all buffer cells.
    {
        flags_ = static_cast<unsigned>(_flags);

        // if constexpr (ColumnOptimized)
        // {
        //     #if !defined(LINE_AVOID_CELL_RESET)
        //     if (buffer_.back().backgroundColor() != _attributes.backgroundColor)
        //         // TODO: also styles and UL color
        //         markUsedFirst(ColumnCount::cast_from(buffer_.size()));
        //
        //     for (auto i = 0; i < *columnsUsed(); ++i)
        //         buffer_[i].reset(_attributes);
        //     #endif
        //     markUsedFirst(ColumnCount(0));
        // }
        // else
        {
            for (Cell& cell: buffer_)
                cell.reset(_attributes);
        }
    }

    void markUsedFirst(ColumnCount /*_n*/) noexcept
    {
        // if constexpr (ColumnOptimized)
        //     usedColumns_.value = _n;
    }

    void reset(LineFlags _flags, GraphicsAttributes const& _attributes,
               char32_t _codepoint, int _width) noexcept
    {
        flags_ = static_cast<unsigned>(_flags);
        markUsedFirst(size());
        for (Cell& cell: buffer_)
        {
            cell.reset();
            cell.write(_attributes, _codepoint, _width);
        }
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

        Cell* i = &buffer_[unbox<int>(_start)];
        Cell* e = i + _ascii.size();
        while (i != e)
            (i++)->write(_sgr, *s++, ASCII_Width);

        // if constexpr (ColumnOptimized)
        // {
        //     #if !defined(LINE_AVOID_CELL_RESET)
        //     auto const e2 = buffer.data() + unbox<long>(columnsUsed());
        //     while (i != e2)
        //         (i++)->reset();
        //     #endif
        //     usedColumns_.value = boxed_cast<ColumnCount>(_start) + ColumnCount::cast_from(_ascii.size());
        // }
        // else
        {
            auto const e2 = buffer.data() + buffer.size();
            while (i != e2)
                (i++)->reset();
        }
    }

    ColumnCount size() const noexcept { return ColumnCount::cast_from(buffer_.size()); }

    ColumnCount columnsUsed() const noexcept
    {
        // if constexpr (ColumnOptimized)
        //     return usedColumns_.value;
        // else
            return size();
    }

    void resize(ColumnCount _count);

    gsl::span<Cell const> trim_blank_right() const noexcept;

    gsl::span<Cell const> cells() const noexcept { return buffer_; }

    gsl::span<Cell> useRange(ColumnOffset _start, ColumnCount _count) noexcept
    {
        markUsedFirst(std::max(columnsUsed(),
                               boxed_cast<ColumnCount>(_start) + _count));
        return gsl::span(buffer_).subspan(unbox<size_t>(_start), unbox<size_t>(_count));
    }

    iterator begin() noexcept { return buffer_.begin(); }
    iterator end() noexcept { return std::next(std::begin(buffer_), unbox<int>(columnsUsed())); }

    const_iterator begin() const noexcept { return buffer_.begin(); }
    const_iterator end() const noexcept { return std::next(buffer_.begin(), unbox<int>(columnsUsed())); }

    Cell& front() noexcept { return buffer_.front(); }
    Cell const& front() const noexcept { return buffer_.front(); }

    Cell& back() noexcept { return *std::next(buffer_.begin(), unbox<int>(columnsUsed() - 1)); }
    Cell const& back() const noexcept { return *std::next(buffer_.begin(), unbox<int>(columnsUsed() - 1)); }

    Cell& useCellAt(ColumnOffset _column) noexcept
    {
        assert(ColumnOffset(0) <= _column);
        assert(_column < ColumnOffset::cast_from(buffer_.size())); // Allow off-by-one for sentinel.
        // if constexpr (ColumnOptimized)
        //     usedColumns_.value.value = std::max(usedColumns_.value.value, _column.value + 1);
        return editable().at(unbox<int>(_column));
    }

    Cell const& at(ColumnOffset _column) const noexcept
    {
        Expects(ColumnOffset(0) <= _column);
        Expects(_column < ColumnOffset::cast_from(buffer_.size())); // Allow off-by-one for sentinel.
        return buffer_[unbox<int>(_column)];
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
        auto constexpr Inheritables = unsigned(LineFlags::Wrappable)
                                    | unsigned(LineFlags::Marked);
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

private:
    InflatedBuffer buffer_;
    Storage storage_;
    unsigned flags_ = 0;
    // OptionalProperty<ColumnCount, ColumnOptimized> usedColumns_;
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
    // TODO: when we impement the line text buffer optimization,
    // then this is the place where we want to *promote* a possibly
    // optimized text buffer to a full grid line buffer.
#if 0
    if (std::holds_alternative<SimpleLineBuffer>(storage_))
        storage_ = inflate<Cell>(std::get<SimpleLineBuffer>(storage_));

    return std::get<InflatedBuffer>(storage_);
#else
    return buffer_;
#endif
}

}

namespace fmt // {{{
{
    template <>
    struct formatter<terminal::LineFlags> {
        template <typename ParseContext>
        constexpr auto parse(ParseContext& ctx) { return ctx.begin(); }
        template <typename FormatContext>
        auto format(const terminal::LineFlags _flags, FormatContext& ctx)
        {
            static const std::array<std::pair<terminal::LineFlags, std::string_view>, 3> nameMap = {
                std::pair{terminal::LineFlags::Wrappable, std::string_view("Wrappable")},
                std::pair{terminal::LineFlags::Wrapped, std::string_view("Wrapped")},
                std::pair{terminal::LineFlags::Marked, std::string_view("Marked")},
            };
            std::string s;
            for (auto const& mapping : nameMap)
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
} // }}}
