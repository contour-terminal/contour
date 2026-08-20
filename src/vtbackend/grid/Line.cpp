// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/grid/Line.hpp>

#include <vtbackend/SgrWriter.hpp>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

#include <ranges>

namespace vtbackend
{

LineSoA Line::reflow(ColumnCount newColumnCount)
{
    using crispy::Comparison;

    // Blank lines have no content to reflow — just adopt the new logical width.
    // No allocation, no overflow generated.
    if (isBlank())
    {
        _columns = newColumnCount;
        return {};
    }

    switch (crispy::strongCompare(newColumnCount, size()))
    {
        case Comparison::Equal: break;
        case Comparison::Greater:
            resizeLineSoA(_storage, newColumnCount);
            _columns = newColumnCount;
            break;
        case Comparison::Less: {
            if (wrappable())
            {
                auto const newCols = unbox<size_t>(newColumnCount);
                auto const oldCols = unbox<size_t>(_columns);

                // Trim trailing blank cells
                auto reflowEnd = oldCols;
                while (reflowEnd > newCols && _storage.codepoints[reflowEnd - 1] == 0)
                    --reflowEnd;

                auto const overflowCount = reflowEnd - newCols;
                if (overflowCount > 0)
                {
                    // Copy overflow columns into a new LineSoA
                    LineSoA overflow;
                    initializeLineSoA(overflow, ColumnCount::cast_from(overflowCount));
                    copyColumns(_storage, newCols, overflow, 0, overflowCount);

                    // Shrink this line
                    resizeLineSoA(_storage, newColumnCount);
                    _columns = newColumnCount;
                    return overflow;
                }

                resizeLineSoA(_storage, newColumnCount);
                _columns = newColumnCount;
                return {};
            }
            else
            {
                resizeLineSoA(_storage, newColumnCount);
                _columns = newColumnCount;
                return {};
            }
        }
    }
    return {};
}

std::string Line::toUtf8() const
{
    return toUtf8(ColumnOffset(0), ColumnOffset::cast_from(_columns));
}

std::string Line::toUtf8ColumnAligned() const
{
    return toUtf8(ColumnOffset(0), ColumnOffset::cast_from(_columns), ContinuationCell::Pad);
}

std::string Line::toUtf8(ColumnOffset begin, ColumnOffset end, ContinuationCell continuation) const
{
    auto const cols = unbox<size_t>(_columns);
    auto const first = std::min(static_cast<size_t>(std::max(*begin, 0)), cols);
    auto const last = std::min(static_cast<size_t>(std::max(*end, 0)), cols);
    if (first >= last)
        return {};

    if (isBlank())
    {
        // Not a braced init list: {n, ' '} would select std::string's initializer_list<char>
        // constructor and narrow the count to char. NRVO makes the named local free.
        auto blanks = std::string(static_cast<size_t>(last - first), ' ');
        return blanks;
    }

    std::string str;
    str.reserve(last - first); // exact for ASCII, a sound floor for anything wider

    int skipCount = 0;
    for (auto const i: std::views::iota(first, last))
    {
        if (skipCount > 0)
        {
            --skipCount;
            // A wide character's continuation cell: emit a padding space when the caller wants one
            // codepoint per grid column, otherwise let the leading cell stand for the whole glyph.
            if (continuation == ContinuationCell::Pad)
                str += ' ';
            continue;
        }
        if (_storage.clusterSize[i] == 0)
            str += ' ';
        else
        {
            forEachCodepoint(_storage, i, [&](char32_t cp) {
                unicode::convert_to<char>(std::u32string_view(&cp, 1), std::back_inserter(str));
            });
            skipCount = _storage.widths[i] - 1;
        }
    }
    return str;
}

std::string Line::toUtf8WithSgr(ColumnOffset begin, ColumnOffset end) const
{
    auto const cols = unbox<size_t>(_columns);
    auto const first = std::min(static_cast<size_t>(std::max(*begin, 0)), cols);
    auto const last = std::min(static_cast<size_t>(std::max(*end, 0)), cols);
    if (first >= last)
        return {};

    auto str = std::string {};

    // A blank line is uniformly its FILL rendition — one SGR up front covers the whole run. Not
    // necessarily the default one: Screen erases with the cursor's rendition, so `\e[41m\e[2J`
    // leaves every row blank on a red pen. Rendering those rows as bare spaces would silently drop
    // the colour of every cleared region from a `capture-pane -e`, while the rows that happen to
    // hold text keep theirs.
    if (isBlank())
    {
        auto spaces = std::string(static_cast<size_t>(last - first), ' ');
        auto const& fill = _storage.fillAttrs;
        if (fill == GraphicsAttributes {})
            return spaces;
        // Closed with a reset, exactly as the cell loop below does, so it cannot bleed onward.
        return makeSgrSequence(fill) + spaces + "\033[m";
    }

    auto current = GraphicsAttributes {}; // the default rendition is "in effect" at line start
    auto skipCount = 0;
    for (auto const i: std::views::iota(first, last))
    {
        if (skipCount > 0)
        {
            --skipCount; // a wide char's trailing cells share the lead cell's rendition
            continue;
        }
        if (auto const& attrs = _storage.sgr[i]; attrs != current)
        {
            str += makeSgrSequence(attrs);
            current = attrs;
        }
        if (_storage.clusterSize[i] == 0)
            str += ' ';
        else
        {
            forEachCodepoint(_storage, i, [&](char32_t cp) {
                unicode::convert_to<char>(std::u32string_view(&cp, 1), std::back_inserter(str));
            });
            skipCount = _storage.widths[i] - 1;
        }
    }
    if (current != GraphicsAttributes {})
        str += "\033[m"; // close the last open rendition so it cannot bleed onto the next line
    return str;
}

ColumnCount Line::trimmedColumns() const noexcept
{
    // A blank line is uniformly its fill rendition, so it either trims away entirely or not at all —
    // and where the fill is non-default it is a cleared region wearing a colour, which survives.
    if (isBlank())
        return _storage.fillAttrs == GraphicsAttributes {} ? ColumnCount(0) : _columns;

    auto const isDefaultCell = [this](size_t column) {
        auto const blankText = _storage.clusterSize[column] == 0
                               || (_storage.clusterSize[column] == 1 && _storage.codepoints[column] == U' ');
        return blankText && _storage.sgr[column] == GraphicsAttributes {}
               && _storage.hyperlinks[column] == HyperlinkId {}
               && !(_storage.imageFragments
                    && _storage.imageFragments->contains(static_cast<uint16_t>(column)));
    };

    auto end = unbox<size_t>(_columns);
    while (end > 0 && isDefaultCell(end - 1))
        --end;
    return ColumnCount::cast_from(end);
}

std::string Line::toUtf8Trimmed() const
{
    return toUtf8Trimmed(true, true);
}

std::string Line::toUtf8Trimmed(bool stripLeadingSpaces, bool stripTrailingSpaces) const
{
    std::string output = toUtf8();

    if (stripTrailingSpaces)
        while (!output.empty() && isspace(output.back()))
            output.pop_back();

    if (stripLeadingSpaces)
    {
        size_t frontGap = 0;
        while (frontGap < output.size() && std::isspace(output[frontGap]))
            frontGap++;
        output = output.substr(frontGap);
    }

    return output;
}

} // end namespace vtbackend
