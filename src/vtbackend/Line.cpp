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
    std::string str;
    auto const cols = unbox<size_t>(_columns);
    int skipCount = 0;
    for (size_t i = 0; i < cols; ++i)
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
