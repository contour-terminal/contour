// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellUtil.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/cell/CellConcept.h>
#include <vtbackend/primitives.h>

#include <crispy/BufferObject.h>
#include <crispy/Comparison.h>
#include <crispy/assert.h>
#include <crispy/flags.h>

#include <libunicode/convert.h>

#include <gsl/span>
#include <gsl/span_ext>

#include <string>
#include <variant>
#include <vector>

namespace vtbackend
{

enum class LineFlag : uint8_t
{
    None = 0x0000,
    Wrappable = 0x0001,
    Wrapped = 0x0002,
    Marked = 0x0004,
    // TODO: DoubleWidth  = 0x0010,
    // TODO: DoubleHeight = 0x0020,
};

using LineFlags = crispy::flags<LineFlag>;

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
    crispy::buffer_fragment<char> text {};

    void reset(GraphicsAttributes attributes) noexcept
    {
        textAttributes = attributes;
        fillAttributes = attributes;
        hyperlink = {};
        usedColumns = {};
        text.reset();
    }
};

template <CellConcept Cell>
using InflatedLineBuffer = std::vector<Cell>;

/// Unpacks a TrivialLineBuffer into an InflatedLineBuffer<Cell>.
template <CellConcept Cell>
InflatedLineBuffer<Cell> inflate(TrivialLineBuffer const& input);

template <CellConcept Cell>
using LineStorage = std::variant<TrivialLineBuffer, InflatedLineBuffer<Cell>>;

/**
 * Line<Cell> API.
 *
 * TODO: Use custom allocator for ensuring cache locality of Cells to sibling lines.
 * TODO: Make the line optimization work.
 */
template <CellConcept Cell>
class Line
{
  public:
    Line() = default;
    Line(Line const&) = default;
    Line(Line&&) noexcept = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) noexcept = default;

    using TrivialBuffer = TrivialLineBuffer;
    using InflatedBuffer = InflatedLineBuffer<Cell>;
    using Storage = LineStorage<Cell>;
    using value_type = Cell;
    using iterator = typename InflatedBuffer::iterator;
    using reverse_iterator = typename InflatedBuffer::reverse_iterator;
    using const_iterator = typename InflatedBuffer::const_iterator;

    Line(LineFlags flags, TrivialBuffer buffer): _storage { std::move(buffer) }, _flags { flags } {}

    Line(LineFlags flags, InflatedBuffer buffer): _storage { std::move(buffer) }, _flags { flags } {}

    void reset(LineFlags flags, GraphicsAttributes attributes) noexcept
    {
        _flags = flags;
        if (isTrivialBuffer())
            trivialBuffer().reset(attributes);
        else
            setBuffer(TrivialBuffer { ColumnCount::cast_from(inflatedBuffer().size()), attributes });
    }

    void reset(LineFlags flags, GraphicsAttributes attributes, ColumnCount count) noexcept
    {
        _flags = flags;
        setBuffer(TrivialBuffer { count, attributes });
    }

    void fill(LineFlags flags,
              GraphicsAttributes const& attributes,
              char32_t codepoint,
              uint8_t width) noexcept
    {
        if (codepoint == 0)
            reset(flags, attributes);
        else
        {
            _flags = flags;
            for (Cell& cell: inflatedBuffer())
            {
                cell.reset();
                cell.write(attributes, codepoint, width);
            }
        }
    }

    /// Tests if all cells are empty.
    [[nodiscard]] bool empty() const noexcept
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
     * @p start offset into this line of the first charater
     * @p sgr graphics rendition for the line starting at @c start until the end
     * @p ascii the US-ASCII characters to fill with
     */
    void fill(ColumnOffset start, GraphicsAttributes const& sgr, std::string_view ascii)
    {
        auto& buffer = inflatedBuffer();

        assert(unbox<size_t>(start) + ascii.size() <= buffer.size());

        auto constexpr ASCII_Width = 1; // NOLINT
        auto const* s = ascii.data();

        Cell* i = &buffer[unbox<size_t>(start)];
        Cell* e = i + ascii.size();
        while (i != e)
            (i++)->write(sgr, static_cast<char32_t>(*s++), ASCII_Width);

        auto const e2 = buffer.data() + buffer.size();
        while (i != e2)
            (i++)->reset();
    }

    [[nodiscard]] ColumnCount size() const noexcept
    {
        if (isTrivialBuffer())
            return trivialBuffer().displayWidth;
        else
            return ColumnCount::cast_from(inflatedBuffer().size());
    }

    void resize(ColumnCount count);

    [[nodiscard]] gsl::span<Cell const> trim_blank_right() const noexcept;

    [[nodiscard]] gsl::span<Cell const> cells() const noexcept { return inflatedBuffer(); }

    [[nodiscard]] gsl::span<Cell> useRange(ColumnOffset start, ColumnCount count) noexcept
    {
#if defined(__clang__) && __clang_major__ <= 11
        auto const bufferSpan = gsl::span(inflatedBuffer());
        return bufferSpan.subspan(unbox<size_t>(start), unbox<size_t>(count));
#else
        // Clang <= 11 cannot deal with this (e.g. FreeBSD 13 defaults to Clang 11).
        return gsl::span(inflatedBuffer()).subspan(unbox<size_t>(start), unbox<size_t>(count));
#endif
    }

    [[nodiscard]] Cell& useCellAt(ColumnOffset column) noexcept
    {
        Require(ColumnOffset(0) <= column);
        Require(column <= ColumnOffset::cast_from(size())); // Allow off-by-one for sentinel.
        return inflatedBuffer()[unbox<size_t>(column)];
    }

    [[nodiscard]] uint8_t cellEmptyAt(ColumnOffset column) const noexcept
    {
        if (isTrivialBuffer())
        {
            Require(ColumnOffset(0) <= column);
            Require(column < ColumnOffset::cast_from(size()));
            return unbox<size_t>(column) >= trivialBuffer().text.size()
                   || trivialBuffer().text[column.as<size_t>()] == 0x20;
        }
        return inflatedBuffer().at(unbox<size_t>(column)).empty();
    }

    [[nodiscard]] uint8_t cellWidthAt(ColumnOffset column) const noexcept
    {
#if 0 // TODO: This optimization - but only when we return actual widths and not always 1.
        if (isTrivialBuffer())
        {
            Require(ColumnOffset(0) <= column);
            Require(column < ColumnOffset::cast_from(size()));
            return 1; // TODO: When trivial line is to support Unicode, this should be adapted here.
        }
#endif
        return inflatedBuffer().at(unbox<size_t>(column)).width();
    }

    [[nodiscard]] LineFlags flags() const noexcept { return static_cast<LineFlags>(_flags); }

    [[nodiscard]] bool marked() const noexcept { return isFlagEnabled(LineFlag::Marked); }
    void setMarked(bool enable) { setFlag(LineFlag::Marked, enable); }

    [[nodiscard]] bool wrapped() const noexcept { return isFlagEnabled(LineFlag::Wrapped); }
    void setWrapped(bool enable) { setFlag(LineFlag::Wrapped, enable); }

    [[nodiscard]] bool wrappable() const noexcept { return isFlagEnabled(LineFlag::Wrappable); }
    void setWrappable(bool enable) { setFlag(LineFlag::Wrappable, enable); }

    [[nodiscard]] LineFlags wrappableFlag() const noexcept
    {
        return wrappable() ? LineFlag::Wrappable : LineFlag::None;
    }
    [[nodiscard]] LineFlags wrappedFlag() const noexcept
    {
        return marked() ? LineFlag::Wrapped : LineFlag::None;
    }
    [[nodiscard]] LineFlags markedFlag() const noexcept
    {
        return marked() ? LineFlag::Marked : LineFlag::None;
    }

    [[nodiscard]] LineFlags inheritableFlags() const noexcept
    {
        auto constexpr Inheritables = LineFlags({ LineFlag::Wrappable, LineFlag::Marked });
        return _flags & Inheritables;
    }

    void setFlag(LineFlags flags, bool enable) noexcept
    {
        if (enable)
            _flags.enable(flags);
        else
            _flags.disable(flags);
    }

    [[nodiscard]] bool isFlagEnabled(LineFlags flags) const noexcept { return (_flags & flags).any(); }

    [[nodiscard]] InflatedBuffer reflow(ColumnCount newColumnCount);
    [[nodiscard]] std::string toUtf8() const;
    [[nodiscard]] std::string toUtf8Trimmed() const;
    [[nodiscard]] std::string toUtf8Trimmed(bool stripLeadingSpaces, bool stripTrailingSpaces) const;

    // Returns a reference to this mutable grid-line buffer.
    //
    // If this line has been stored in an optimized state, then
    // the line will be first unpacked into a vector of grid cells.
    [[nodiscard]] InflatedBuffer& inflatedBuffer();
    [[nodiscard]] InflatedBuffer const& inflatedBuffer() const;

    [[nodiscard]] TrivialBuffer& trivialBuffer() noexcept { return std::get<TrivialBuffer>(_storage); }
    [[nodiscard]] TrivialBuffer const& trivialBuffer() const noexcept
    {
        return std::get<TrivialBuffer>(_storage);
    }

    [[nodiscard]] bool isTrivialBuffer() const noexcept
    {
        return std::holds_alternative<TrivialBuffer>(_storage);
    }
    [[nodiscard]] bool isInflatedBuffer() const noexcept { return !isTrivialBuffer(); }

    void setBuffer(Storage buffer) noexcept { _storage = std::move(buffer); }

    // Tests if the given text can be matched in this line at the exact given start column, in sensetive
    // or insensitive mode.
    [[nodiscard]] bool matchTextAtWithSensetivityMode(std::u32string_view text,
                                                      ColumnOffset startColumn,
                                                      bool isCaseSensitive) const noexcept
    {
        if (isTrivialBuffer())
        {
            assert(false);
            auto const u8Text = unicode::convert_to<char>(text);
            TrivialBuffer const& buffer = trivialBuffer();
            if (!buffer.usedColumns)
                return false;
            auto const column = std::min(startColumn, boxed_cast<ColumnOffset>(buffer.usedColumns - 1));
            if (text.size() > static_cast<size_t>(column.value - buffer.usedColumns.value))
                return false;

            auto bufferCopyText = std::string(buffer.text.view());
            std::transform(bufferCopyText.begin(), bufferCopyText.end(), bufferCopyText.begin(), ::tolower);
            auto const resultIndex = bufferCopyText.substr(unbox<size_t>(column))
                                         .find(std::string_view(u8Text), unbox<size_t>(column));
            return resultIndex == 0;
        }
        else
        {
            InflatedBuffer const& cells = inflatedBuffer();
            if (text.size() > unbox<size_t>(size()) - unbox<size_t>(startColumn))
                return false;
            auto const baseColumn = unbox<size_t>(startColumn);
            size_t i = 0;
            while (i < text.size())
            {
                if (!CellUtil::beginsWith(text.substr(i), cells[baseColumn + i], isCaseSensitive))
                    return false;
                ++i;
            }
            return i == text.size();
        }
    }

    // Search a line from left to right if a complete match is found it returns the Column of
    // start of match and partialMatchLength is set to 0, since it's a full match but if a partial
    // match is found at the right end of line it returns startColumn as it is and the partialMatchLength
    // is set equal to match found at the left end of line.
    [[nodiscard]] std::optional<SearchResult> search(std::u32string_view text,
                                                     ColumnOffset startColumn,
                                                     bool isCaseSensitive) const noexcept
    {

        auto matchTextAt = [&](auto text, auto baseColumn) {
            return matchTextAtWithSensetivityMode(text, baseColumn, isCaseSensitive);
        };

        if (isTrivialBuffer())
        {
            auto const u8Text = unicode::convert_to<char>(text);
            TrivialBuffer const& buffer = trivialBuffer();
            if (!buffer.usedColumns)
                return std::nullopt;
            auto const column = std::min(startColumn, boxed_cast<ColumnOffset>(buffer.usedColumns - 1));
            auto const resultIndex = buffer.text.view().find(std::string_view(u8Text), unbox<size_t>(column));
            if (resultIndex != std::string_view::npos)
                return SearchResult { ColumnOffset::cast_from(resultIndex) };
            else
                return std::nullopt; // Not found, so stay with initial column as result.
        }
        else
        {
            InflatedBuffer const& buffer = inflatedBuffer();
            if (buffer.size() < text.size())
                return std::nullopt; // not found: line is smaller than search term

            auto baseColumn = startColumn;
            auto rightMostSearchPosition = ColumnOffset::cast_from(buffer.size());
            while (baseColumn < rightMostSearchPosition)
            {
                if (buffer.size() - unbox<size_t>(baseColumn) < text.size())
                {
                    text.remove_suffix(text.size() - (unbox<size_t>(size()) - unbox<size_t>(baseColumn)));
                    if (matchTextAt(text, baseColumn))
                        return SearchResult { startColumn, text.size() };
                }
                else if (matchTextAt(text, baseColumn))
                    return SearchResult { baseColumn };
                baseColumn++;
            }

            return std::nullopt; // Not found, so stay with initial column as result.
        }
    }

    // Search a line from right to left if a complete match is found it returns the Column of
    // start of match and  partialMatchLength is set to 0, since it's a full match but if a partial
    // match is found at the left end of line it returns startColumn as it is and the partialMatchLength
    // is set equal to match found at the left end of line.
    [[nodiscard]] std::optional<SearchResult> searchReverse(std::u32string_view text,
                                                            ColumnOffset startColumn,
                                                            bool isCaseSensitive) const noexcept
    {

        auto matchTextAt = [&](auto text, auto baseColumn) {
            return matchTextAtWithSensetivityMode(text, baseColumn, isCaseSensitive);
        };

        if (isTrivialBuffer())
        {
            auto const u8Text = unicode::convert_to<char>(text);
            TrivialBuffer const& buffer = trivialBuffer();
            if (!buffer.usedColumns)
                return std::nullopt;
            auto const column = std::min(startColumn, boxed_cast<ColumnOffset>(buffer.usedColumns - 1));
            auto const resultIndex =
                buffer.text.view().rfind(std::string_view(u8Text), unbox<size_t>(column));
            if (resultIndex != std::string_view::npos)
                return SearchResult { ColumnOffset::cast_from(resultIndex) };
            else
                return std::nullopt; // Not found, so stay with initial column as result.
        }
        else
        {
            InflatedBuffer const& buffer = inflatedBuffer();
            if (buffer.size() < text.size())
                return std::nullopt; // not found: line is smaller than search term

            // reverse search from right@column to left until match is complete.
            auto baseColumn = std::min(startColumn, ColumnOffset::cast_from(buffer.size() - text.size()));
            while (baseColumn >= ColumnOffset(0))
            {
                if (matchTextAt(text, baseColumn))
                    return SearchResult { baseColumn };
                baseColumn--;
            }
            baseColumn = ColumnOffset::cast_from(text.size() - 1);
            while (!text.empty())
            {
                if (matchTextAt(text, ColumnOffset(0)))
                    return SearchResult { startColumn, text.size() };
                baseColumn--;
                text.remove_prefix(1);
            }
            return std::nullopt; // Not found, so stay with initial column as result.
        }
    }

  private:
    Storage _storage;
    LineFlags _flags;
};

template <CellConcept Cell>
inline typename Line<Cell>::InflatedBuffer& Line<Cell>::inflatedBuffer()
{
    if (auto trivialbuffer = std::get_if<TrivialBuffer>(&_storage))
        _storage = inflate<Cell>(*trivialbuffer);
    return std::get<InflatedBuffer>(_storage);
}

template <CellConcept Cell>
inline typename Line<Cell>::InflatedBuffer const& Line<Cell>::inflatedBuffer() const
{
    return const_cast<Line<Cell>*>(this)->inflatedBuffer();
}

} // namespace vtbackend

template <>
struct fmt::formatter<vtbackend::LineFlags>: formatter<std::string>
{
    auto format(const vtbackend::LineFlags flags, format_context& ctx) const -> format_context::iterator
    {
        static const std::array<std::pair<vtbackend::LineFlags, std::string_view>, 3> nameMap = {
            std::pair { vtbackend::LineFlag::Wrappable, std::string_view("Wrappable") },
            std::pair { vtbackend::LineFlag::Wrapped, std::string_view("Wrapped") },
            std::pair { vtbackend::LineFlag::Marked, std::string_view("Marked") },
        };
        std::string s;
        for (auto const& mapping: nameMap)
        {
            if ((mapping.first & flags) != vtbackend::LineFlag::None)
            {
                if (!s.empty())
                    s += ",";
                s += mapping.second;
            }
        }
        return formatter<std::string>::format(s, ctx);
    }
};
