// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Line.h>
#include <vtbackend/primitives.h>

#include <libunicode/grapheme_segmenter.h>
#include <libunicode/utf8.h>
#include <libunicode/width.h>

using std::get;
using std::holds_alternative;
using std::min;

namespace vtbackend
{

template <CellConcept Cell>
typename Line<Cell>::InflatedBuffer Line<Cell>::reflow(ColumnCount newColumnCount)
{
    using crispy::comparison;
    if (isTrivialBuffer())
    {
        switch (crispy::strongCompare(newColumnCount, ColumnCount::cast_from(trivialBuffer().text.size())))
        {
            case comparison::Greater: trivialBuffer().displayWidth = newColumnCount; return {};
            case comparison::Equal: return {};
            case comparison::Less:;
        }
    }
    auto& buffer = inflatedBuffer();
    // TODO: Efficiently handle TrivialBuffer-case.
    switch (crispy::strongCompare(newColumnCount, size()))
    {
        case comparison::Equal: break;
        case comparison::Greater: buffer.resize(unbox<size_t>(newColumnCount)); break;
        case comparison::Less: {
            // TODO: properly handle wide character cells
            // - when cutting in the middle of a wide char, the wide char gets wrapped and an empty
            //   cell needs to be injected to match the expected column width.

            if (wrappable())
            {
                auto const [reflowStart, reflowEnd] = [newColumnCount, &buffer]() {
                    auto const reflowStart =
                        next(buffer.begin(), *newColumnCount /* - buffer[newColumnCount].width()*/);

                    auto reflowEnd = buffer.end();

                    while (reflowEnd != reflowStart && prev(reflowEnd)->empty())
                        reflowEnd = prev(reflowEnd);

                    return std::tuple { reflowStart, reflowEnd };
                }();

                auto removedColumns = InflatedBuffer(reflowStart, reflowEnd);
                buffer.erase(reflowStart, buffer.end());
                assert(size() == newColumnCount);
#if 0
                if (removedColumns.size() > 0 && std::ranges::any_of(removedColumns, [](Cell const& x) {
                        if (!x.empty())
                            std::cout << std::format("non-empty cell in reflow: {}\n", x.toUtf8());
                        return !x.empty();
                    }))
                    printf("Wrapping around\n");
#endif
                return removedColumns;
            }
            else
            {
                buffer.resize(unbox<size_t>(newColumnCount));
                assert(size() == newColumnCount);
                return {};
            }
        }
    }
    return {};
}

template <CellConcept Cell>
inline void Line<Cell>::resize(ColumnCount count)
{
    assert(*count >= 0);
    if (1) // constexpr (Optimized)
    {
        if (isTrivialBuffer())
        {
            TrivialBuffer& buffer = trivialBuffer();
            buffer.displayWidth = count;
            return;
        }
    }
    inflatedBuffer().resize(unbox<size_t>(count));
}

template <CellConcept Cell>
gsl::span<Cell const> Line<Cell>::trim_blank_right() const noexcept
{
    auto i = inflatedBuffer().data();
    auto e = inflatedBuffer().data() + inflatedBuffer().size();

    while (i != e && (e - 1)->empty())
        --e;

    return gsl::make_span(i, e);
}

template <CellConcept Cell>
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

template <CellConcept Cell>
std::string Line<Cell>::toUtf8Trimmed() const
{
    return toUtf8Trimmed(true, true);
}

template <CellConcept Cell>
std::string Line<Cell>::toUtf8Trimmed(bool stripLeadingSpaces, bool stripTrailingSpaces) const
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

template <CellConcept Cell>
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

        if (unicode::grapheme_segmenter::is_breakable(lastChar, nextChar))
        {
            while (gapPending > 0)
            {
                columns.emplace_back(input.textAttributes.with(CellFlag::WideCharContinuation),
                                     input.hyperlink);
                --gapPending;
            }
            auto const charWidth = static_cast<int>(unicode::width(nextChar));
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

    // Handle trailing incomplete UTF-8 sequence by emitting a replacement character.
    // This can happen if the buffer was truncated mid-character.
    if (utf8DecoderState.expectedLength != 0)
    {
        while (gapPending > 0)
        {
            columns.emplace_back(input.textAttributes.with(CellFlag::WideCharContinuation), input.hyperlink);
            --gapPending;
        }
        columns.emplace_back(Cell {});
        columns.back().setHyperlink(input.hyperlink);
        columns.back().write(input.textAttributes, ReplacementCharacter, 1);
    }

    while (gapPending > 0)
    {
        columns.emplace_back(Cell { input.textAttributes, input.hyperlink });
        --gapPending;
    }

    // Note: The assertion below may fail if there was an incomplete UTF-8 sequence at the end
    // of the buffer, in which case we've added an extra cell for the replacement character.
    // This is intentional - we prefer showing corruption visibly rather than silently dropping data.
    assert(unbox(input.displayWidth) > 0);

    while (columns.size() < unbox<size_t>(input.displayWidth))
        columns.emplace_back(Cell { input.fillAttributes });

    return columns;
}
} // end namespace vtbackend

#include <vtbackend/cell/CompactCell.h>
template class vtbackend::Line<vtbackend::CompactCell>;

#include <vtbackend/cell/SimpleCell.h>
template class vtbackend::Line<vtbackend::SimpleCell>;
