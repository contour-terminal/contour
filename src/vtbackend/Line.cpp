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
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/utf8.h>
#include <unicode/width.h>

using std::get;
using std::holds_alternative;
using std::min;

namespace terminal
{

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Line<Cell>::Line(ColumnCount displayWidth, LineFlags flags, GraphicsAttributes attributes, InflatedBuffer inflatedBuffer):
    _displayWidth { displayWidth },
    _flags { static_cast<unsigned>(flags) },
    _trivialBuffer { displayWidth, attributes },
    _inflatedBuffer { inflatedBuffer }
{
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Line<Cell>::reset(LineFlags flags, GraphicsAttributes attributes) noexcept
{
    _flags = static_cast<unsigned>(flags) | static_cast<unsigned>(LineFlags::Trivial);
    _trivialBuffer = TrivialBuffer { _displayWidth, attributes };
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Line<Cell>::empty() const noexcept
{
    if (isTrivialBuffer())
        return trivialBuffer().text.empty();

    for (auto const& cell: inflatedBuffer())
        if (!cell.empty())
            return false;
    return true;
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Line<Cell>::fill(ColumnOffset _start, GraphicsAttributes _sgr, std::string_view _ascii)
{
    auto& buffer = inflatedBuffer();

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

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Line<Cell>::fill(LineFlags flags, GraphicsAttributes attributes, char32_t codepoint, uint8_t width) noexcept
{
    assert(!(static_cast<unsigned>(flags) & static_cast<unsigned>(LineFlags::Trivial)));

    if (!codepoint)
        reset(flags, attributes);
    else
    {
        _flags = static_cast<unsigned>(flags);
        for (Cell& cell: inflatedBuffer())
        {
            cell.reset();
            cell.write(attributes, codepoint, width);
        }
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
gsl::span<Cell> Line<Cell>::useRange(ColumnOffset _start, ColumnCount _count) noexcept
{
#if defined(__clang__) && __clang_major__ <= 11
    auto const bufferSpan = gsl::span(inflatedBuffer());
    return bufferSpan.subspan(unbox<size_t>(_start), unbox<size_t>(_count));
#else
    // Clang <= 11 cannot deal with this (e.g. FreeBSD 13 defaults to Clang 11).
    return gsl::span(inflatedBuffer()).subspan(unbox<size_t>(_start), unbox<size_t>(_count));
#endif
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
Cell& Line<Cell>::useCellAt(ColumnOffset _column) noexcept
{
    Require(ColumnOffset(0) <= _column);
    Require(_column <= ColumnOffset::cast_from(size())); // Allow off-by-one for sentinel.
    return inflatedBuffer()[unbox<size_t>(_column)];
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
uint8_t Line<Cell>::cellEmptyAt(ColumnOffset column) const noexcept
{
    if (isTrivialBuffer())
    {
        Require(ColumnOffset(0) <= column);
        Require(column < ColumnOffset::cast_from(size()));
        return unbox<size_t>(column) >= trivialBuffer().text.size()
               || trivialBuffer().text[column.as<size_t>()] == 0x20;
    }
    return inflatedBuffer()[unbox<size_t>(column)].empty();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
uint8_t Line<Cell>::cellWidthAt(ColumnOffset column) const noexcept
{
#if 0 // TODO: This optimization - but only when we return actual widths and not always 1.
    if (isTrivialBuffer())
    {
        Require(ColumnOffset(0) <= column);
        Require(column < ColumnOffset::cast_from(size()));
        return 1; // TODO: When trivial line is to support Unicode, this should be adapted here.
    }
#endif
    return inflatedBuffer()[unbox<size_t>(column)].width();
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
void Line<Cell>::inflate() noexcept
{
    // TODO(pr)
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
bool Line<Cell>::matchTextAt(std::u32string_view text, ColumnOffset startColumn) const noexcept
{
    if (isTrivialBuffer())
    {
        auto const u8Text = unicode::convert_to<char>(text);
        TrivialBuffer const& buffer = trivialBuffer();
        if (!buffer.usedColumns)
            return false;
        auto const column = std::min(startColumn, boxed_cast<ColumnOffset>(buffer.usedColumns - 1));
        if (text.size() > static_cast<size_t>(column.value - buffer.usedColumns.value))
            return false;
        auto const resultIndex = buffer.text.view()
                                     .substr(unbox<size_t>(column))
                                     .find(std::string_view(u8Text), unbox<size_t>(column));
        return resultIndex == 0;
    }
    else
    {
        auto const u8Text = unicode::convert_to<char>(text);
        InflatedBuffer const& cells = inflatedBuffer();
        if (text.size() > unbox<size_t>(size()) - unbox<size_t>(startColumn))
            return false;
        auto const baseColumn = unbox<size_t>(startColumn);
        size_t i = 0;
        while (i < text.size())
        {
            if (!CellUtil::beginsWith(text.substr(i), cells[baseColumn + i]))
                return false;
            ++i;
        }
        return i == text.size();
    }
}

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::optional<SearchResult> Line<Cell>::search(std::u32string_view text,
                                               ColumnOffset startColumn) const noexcept
{
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

template <typename Cell>
CRISPY_REQUIRES(CellConcept<Cell>)
std::optional<SearchResult> Line<Cell>::searchReverse(std::u32string_view text,
                                                      ColumnOffset startColumn) const noexcept
{
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
// =====================================================================================================

template <typename Cell>
typename Line<Cell>::InflatedBuffer Line<Cell>::reflow(ColumnCount _newColumnCount)
{
    using crispy::Comparison;
    if (isTrivialBuffer())
    {
        switch (crispy::strongCompare(_newColumnCount, ColumnCount::cast_from(trivialBuffer().text.size())))
        {
            case Comparison::Greater: trivialBuffer().displayWidth = _newColumnCount; return {};
            case Comparison::Equal: return {};
            case Comparison::Less:;
        }
    }
    auto& buffer = inflatedBuffer();
    // TODO: Efficiently handle TrivialBuffer-case.
    switch (crispy::strongCompare(_newColumnCount, size()))
    {
        case Comparison::Equal: break;
        case Comparison::Greater: buffer.resize(unbox<size_t>(_newColumnCount)); break;
        case Comparison::Less: {
            // TODO: properly handle wide character cells
            // - when cutting in the middle of a wide char, the wide char gets wrapped and an empty
            //   cell needs to be injected to match the expected column width.

            if (wrappable())
            {
                auto const [reflowStart, reflowEnd] = [_newColumnCount, &buffer]() {
                    auto const reflowStart =
                        next(buffer.begin(), *_newColumnCount /* - buffer[_newColumnCount].width()*/);

                    auto reflowEnd = buffer.end();

                    while (reflowEnd != reflowStart && prev(reflowEnd)->empty())
                        reflowEnd = prev(reflowEnd);

                    return std::tuple { reflowStart, reflowEnd };
                }();

                auto removedColumns = InflatedBuffer(reflowStart, reflowEnd);
                buffer.erase(reflowStart, buffer.end());
                assert(size() == _newColumnCount);
#if 0
                if (removedColumns.size() > 0 &&
                        std::any_of(removedColumns.begin(), removedColumns.end(),
                            [](Cell const& x)
                            {
                                if (!x.empty())
                                    fmt::print("non-empty cell in reflow: {}\n", x.toUtf8());
                                return !x.empty();
                            }))
                    printf("Wrapping around\n");
#endif
                return removedColumns;
            }
            else
            {
                buffer.resize(unbox<size_t>(_newColumnCount));
                assert(size() == _newColumnCount);
                return {};
            }
        }
    }
    return {};
}

template <typename Cell>
inline void Line<Cell>::resize(ColumnCount _count)
{
    assert(*_count >= 0);
    if (1) // constexpr (Optimized)
    {
        if (isTrivialBuffer())
        {
            TrivialBuffer& buffer = trivialBuffer();
            buffer.displayWidth = _count;
            return;
        }
    }
    inflatedBuffer().resize(unbox<size_t>(_count));
}

template <typename Cell>
gsl::span<Cell const> Line<Cell>::trim_blank_right() const noexcept
{
    auto i = inflatedBuffer().data();
    auto e = inflatedBuffer().data() + inflatedBuffer().size();

    while (i != e && (e - 1)->empty())
        --e;

    return gsl::make_span(i, e);
}

template <typename Cell>
std::string Line<Cell>::toUtf8() const
{
    if (isTrivialBuffer())
    {
        auto const& lineBuffer = trivialBuffer();
        auto str = std::string(lineBuffer.text.data(), lineBuffer.text.size());
        for (auto i = lineBuffer.usedColumns; i < lineBuffer.displayWidth; ++i)
            str += ' ';
        return str;
    }

    std::string str;
    for (Cell const& cell: inflatedBuffer())
    {
        if (cell.codepointCount() == 0)
            str += ' ';
        else
            str += cell.toUtf8();
    }
    return str;
}

template <typename Cell>
std::string Line<Cell>::toUtf8Trimmed() const
{
    std::string output = toUtf8();
    while (!output.empty() && isspace(output.back()))
        output.pop_back();

    size_t frontGap = 0;
    while (frontGap < output.size() && std::isspace(output[frontGap]))
        frontGap++;
    output = output.substr(frontGap);

    return output;
}

template <typename Cell>
InflatedLineBuffer<Cell> inflate(TrivialLineBuffer const& input)
{
    static constexpr char32_t ReplacementCharacter { 0xFFFD };

    auto columns = InflatedLineBuffer<Cell> {};
    columns.reserve(unbox<size_t>(input.displayWidth));

    auto lastChar = char32_t { 0 };
    auto utf8DecoderState = unicode::utf8_decoder_state {};
    auto gapPending = 0;

    for (char const ch: input.text.view())
    {
        unicode::ConvertResult const r = unicode::from_utf8(utf8DecoderState, static_cast<uint8_t>(ch));
        if (holds_alternative<unicode::Incomplete>(r))
            continue;

        auto const nextChar =
            holds_alternative<unicode::Success>(r) ? get<unicode::Success>(r).value : ReplacementCharacter;

        if (unicode::grapheme_segmenter::breakable(lastChar, nextChar))
        {
            while (gapPending > 0)
            {
                columns.emplace_back(Cell { input.textAttributes, input.hyperlink });
                --gapPending;
            }
            auto const charWidth = unicode::width(nextChar);
            columns.emplace_back(Cell {});
            columns.back().setHyperlink(input.hyperlink);
            columns.back().write(input.textAttributes, nextChar, static_cast<uint8_t>(charWidth));
            gapPending = charWidth - 1;
        }
        else
        {
            Cell& prevCell = columns.back();
            auto const extendedWidth = prevCell.appendCharacter(nextChar);
            if (extendedWidth > 0)
            {
                auto const cellsAvailable = *input.displayWidth - static_cast<int>(columns.size()) + 1;
                auto const n = min(extendedWidth, cellsAvailable);
                for (int i = 1; i < n; ++i)
                {
                    columns.emplace_back(Cell { input.textAttributes });
                    columns.back().setHyperlink(input.hyperlink);
                }
            }
        }
        lastChar = nextChar;
    }

    while (gapPending > 0)
    {
        columns.emplace_back(Cell { input.textAttributes, input.hyperlink });
        --gapPending;
    }

    assert(columns.size() == unbox<size_t>(input.usedColumns));

    while (columns.size() < unbox<size_t>(input.displayWidth))
        columns.emplace_back(Cell { input.fillAttributes });

    return columns;
}
} // end namespace terminal

#include <vtbackend/cell/CompactCell.h>
template class terminal::Line<terminal::CompactCell>;

#include <vtbackend/cell/SimpleCell.h>
template class terminal::Line<terminal::SimpleCell>;
