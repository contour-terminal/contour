// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/CellProxy.h>
#include <vtbackend/CellUtil.h>
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Hyperlink.h>
#include <vtbackend/LineFlags.h>
#include <vtbackend/LineSoA.h>
#include <vtbackend/SoAClusterWriter.h>
#include <vtbackend/primitives.h>

#include <crispy/BufferObject.h>
#include <crispy/Comparison.h>
#include <crispy/assert.h>
#include <crispy/flags.h>

#include <libunicode/convert.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vtbackend
{

// clang-format off
template <typename, bool> struct OptionalProperty;
template <typename T> struct OptionalProperty<T, false> {};
template <typename T> struct OptionalProperty<T, true> { T value; };
// clang-format on

/// [[deprecated("Use LineSoA directly")]]
/// Kept for backward compatibility with RenderBufferBuilder.
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

/**
 * Line API backed by LineSoA (Structure-of-Arrays storage).
 */
class Line
{
  public:
    /// Buffer type for reflow overflow columns.
    using InflatedBuffer = LineSoA;

    Line() { initializeLineSoA(_storage, ColumnCount(0)); }

    Line(ColumnCount cols, LineFlags flags = {}, GraphicsAttributes attrs = {}):
        _columns { cols }, _flags { flags }
    {
        initializeLineSoA(_storage, cols, attrs);
    }

    /// Construct from TrivialLineBuffer (backward compat -- converts to SoA internally).
    Line(LineFlags flags, TrivialLineBuffer const& buffer): _columns { buffer.displayWidth }, _flags { flags }
    {
        initializeLineSoA(_storage, buffer.displayWidth, buffer.fillAttributes);
        if (!buffer.text.empty())
        {
            writeTextToSoA(_storage, 0, buffer.text.view(), buffer.textAttributes, buffer.hyperlink);
        }
    }

    /// Construct from a LineSoA (used by reflow to create new lines from overflow data).
    Line(LineFlags flags, LineSoA&& soa, ColumnCount cols):
        _storage { std::move(soa) }, _columns { cols }, _flags { flags }
    {
    }

    Line(Line const&) = default;
    Line(Line&&) noexcept = default;
    Line& operator=(Line const&) = default;
    Line& operator=(Line&&) noexcept = default;

    void reset(LineFlags flags, GraphicsAttributes attributes) noexcept
    {
        _flags = flags;
        resetLine(_storage, _columns, attributes);
    }

    void reset(LineFlags flags, GraphicsAttributes attributes, ColumnCount count) noexcept
    {
        _flags = flags;
        _columns = count;
        resizeLineSoA(_storage, count);
        resetLine(_storage, count, attributes);
    }

    /// Fill all cells with the given codepoint and attributes.
    void fill(LineFlags flags,
              GraphicsAttributes const& attributes,
              char32_t codepoint,
              uint8_t width) noexcept
    {
        _flags = flags;
        if (codepoint == 0)
        {
            resetLine(_storage, _columns, attributes);
        }
        else
        {
            for (size_t i = 0; i < unbox<size_t>(_columns); ++i)
                writeCellToSoA(_storage, i, codepoint, width, attributes);
        }
    }

    /// Fill from a column offset with ASCII text.
    void fill(ColumnOffset start, GraphicsAttributes const& sgr, std::string_view ascii)
    {
        assert(unbox<size_t>(start) + ascii.size() <= unbox<size_t>(_columns));
        auto constexpr AsciiWidth = 1;
        auto col = unbox<size_t>(start);
        for (char const ch: ascii)
            writeCellToSoA(_storage, col++, static_cast<char32_t>(ch), AsciiWidth, sgr);
        // Reset remaining cells on the line after the written text
        auto const remaining = unbox<size_t>(_columns) - col;
        if (remaining > 0)
            clearRange(_storage, col, remaining, GraphicsAttributes {});
    }

    /// Tests if all cells are empty.
    [[nodiscard]] bool empty() const noexcept
    {
        return trimBlankRight(_storage, unbox<size_t>(_columns)) == 0;
    }

    [[nodiscard]] ColumnCount size() const noexcept { return _columns; }

    void resize(ColumnCount count)
    {
        _columns = count;
        resizeLineSoA(_storage, count);
    }

    [[nodiscard]] CellProxy useCellAt(ColumnOffset column) noexcept
    {
        Require(ColumnOffset(0) <= column);
        Require(column <= ColumnOffset::cast_from(size())); // Allow off-by-one for sentinel.
        return CellProxy(_storage, unbox<size_t>(column));
    }

    [[nodiscard]] uint8_t cellEmptyAt(ColumnOffset column) const noexcept
    {
        Require(ColumnOffset(0) <= column);
        Require(column < ColumnOffset::cast_from(size()));
        auto const col = unbox<size_t>(column);
        return _storage.codepoints[col] == 0 || _storage.codepoints[col] == 0x20;
    }

    [[nodiscard]] uint8_t cellWidthAt(ColumnOffset column) const noexcept
    {
        return _storage.widths[unbox<size_t>(column)];
    }

    [[nodiscard]] LineFlags flags() const noexcept { return _flags; }
    [[nodiscard]] LineFlags& flags() noexcept { return _flags; }

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
        return wrapped() ? LineFlag::Wrapped : LineFlag::None;
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

    [[nodiscard]] LineSoA reflow(ColumnCount newColumnCount);
    [[nodiscard]] std::string toUtf8() const;
    [[nodiscard]] std::string toUtf8Trimmed() const;
    [[nodiscard]] std::string toUtf8Trimmed(bool stripLeadingSpaces, bool stripTrailingSpaces) const;

    /// Check if all cells share the same graphics attributes (uniform SGR).
    /// O(1) — reads a cached flag maintained by writeCellToSoA/resetLine.
    [[nodiscard]] bool isTrivialBuffer() const noexcept { return _storage.trivial; }

    /// Build a TrivialLineBuffer for the render fast path.
    /// Only valid when isTrivialBuffer() returns true.
    /// @param textOut receives the codepoints directly from SoA (no UTF-8 encoding).
    [[nodiscard]] TrivialLineBuffer trivialBuffer(std::u32string& textOut) const
    {
        auto const cols = unbox<size_t>(_columns);
        auto const used = trimBlankRight(_storage, cols);

        auto const textAttrs = (used > 0) ? _storage.sgr[0] : GraphicsAttributes {};

        // Direct copy from SoA codepoints — no UTF-8 encoding needed.
        textOut.resize(used);
        for (size_t i = 0; i < used; ++i)
            textOut[i] = (_storage.clusterSize[i] == 0) ? U' ' : _storage.codepoints[i];

        auto tb = TrivialLineBuffer {
            .displayWidth = _columns,
            .textAttributes = textAttrs,
            .fillAttributes = textAttrs,
            .hyperlink = (used > 0) ? _storage.hyperlinks[0] : HyperlinkId {},
            .usedColumns = ColumnCount::cast_from(used),
        };
        // text field left empty — caller passes textOut to the renderer directly
        return tb;
    }

    /// Access the underlying SoA storage.
    [[nodiscard]] LineSoA& storage() noexcept { return _storage; }
    [[nodiscard]] LineSoA const& storage() const noexcept { return _storage; }

    // Tests if the given text can be matched in this line at the exact given start column, in sensitive
    // or insensitive mode.
    [[nodiscard]] bool matchTextAtWithSensetivityMode(std::u32string_view text,
                                                      ColumnOffset startColumn,
                                                      bool isCaseSensitive) const noexcept
    {
        auto const cols = unbox<size_t>(size());
        auto const baseColumn = unbox<size_t>(startColumn);
        if (text.size() > cols - baseColumn)
            return false;

        size_t i = 0;
        while (i < text.size())
        {
            auto const col = baseColumn + i;
            auto const proxy = ConstCellProxy(_storage, col);
            if (!CellUtil::beginsWith(text.substr(i), proxy, isCaseSensitive))
                return false;
            ++i;
        }
        return i == text.size();
    }

    // Search a line from left to right
    [[nodiscard]] std::optional<SearchResult> search(std::u32string_view text,
                                                     ColumnOffset startColumn,
                                                     bool isCaseSensitive) const noexcept
    {
        auto const cols = unbox<size_t>(size());
        if (cols < text.size())
            return std::nullopt;

        auto matchTextAt = [&](auto text, auto baseColumn) {
            return matchTextAtWithSensetivityMode(text, baseColumn, isCaseSensitive);
        };

        auto baseColumn = startColumn;
        auto rightMostSearchPosition = ColumnOffset::cast_from(cols);
        while (baseColumn < rightMostSearchPosition)
        {
            if (cols - unbox<size_t>(baseColumn) < text.size())
            {
                auto partialText = text;
                partialText.remove_suffix(text.size() - (unbox<size_t>(size()) - unbox<size_t>(baseColumn)));
                if (matchTextAt(partialText, baseColumn))
                    return SearchResult { .column = startColumn, .partialMatchLength = partialText.size() };
            }
            else if (matchTextAt(text, baseColumn))
                return SearchResult { .column = baseColumn };
            baseColumn++;
        }

        return std::nullopt;
    }

    // Search a line from right to left
    [[nodiscard]] std::optional<SearchResult> searchReverse(std::u32string_view text,
                                                            ColumnOffset startColumn,
                                                            bool isCaseSensitive) const noexcept
    {
        auto const cols = unbox<size_t>(size());
        if (cols < text.size())
            return std::nullopt;

        auto matchTextAt = [&](auto text, auto baseColumn) {
            return matchTextAtWithSensetivityMode(text, baseColumn, isCaseSensitive);
        };

        auto baseColumn = std::min(startColumn, ColumnOffset::cast_from(cols - text.size()));
        while (baseColumn >= ColumnOffset(0))
        {
            if (matchTextAt(text, baseColumn))
                return SearchResult { .column = baseColumn };
            baseColumn--;
        }
        baseColumn = ColumnOffset::cast_from(text.size() - 1);
        auto remainingText = text;
        while (!remainingText.empty())
        {
            if (matchTextAt(remainingText, ColumnOffset(0)))
                return SearchResult { .column = startColumn, .partialMatchLength = remainingText.size() };
            baseColumn--;
            remainingText.remove_prefix(1);
        }
        return std::nullopt;
    }

  private:
    LineSoA _storage;
    ColumnCount _columns {};
    LineFlags _flags {};
};

} // namespace vtbackend

// LineFlags formatter is in LineFlags.h
