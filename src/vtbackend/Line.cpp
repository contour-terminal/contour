// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/Line.h>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

namespace vtbackend
{

LineSoA Line::reflow(ColumnCount newColumnCount)
{
    using crispy::comparison;

    // Blank lines have no content to reflow — just adopt the new logical width.
    // No allocation, no overflow generated.
    if (isBlank())
    {
        _columns = newColumnCount;
        return {};
    }

    switch (crispy::strongCompare(newColumnCount, size()))
    {
        case comparison::Equal: break;
        case comparison::Greater:
            resizeLineSoA(_storage, newColumnCount);
            _columns = newColumnCount;
            break;
        case comparison::Less: {
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

std::string Line::toUtf8(ColumnOffset begin, ColumnOffset end) const
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
    for (auto i = first; i < last; ++i)
    {
        if (skipCount > 0)
        {
            --skipCount;
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
