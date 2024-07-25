// SPDX-License-Identifier: Apache-2.0
#include <vtbackend/GraphicsAttributes.h>
#include <vtbackend/Line.h>
#include <vtbackend/logging.h>
#include <vtbackend/primitives.h>

#include <libunicode/grapheme_line_segmenter.h>
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
                            fmt::print("non-empty cell in reflow: {}\n", x.toUtf8());
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
struct TrivialLineInflater
{
    TrivialLineBuffer const& input;
    InflatedLineBuffer<Cell> columns;

    explicit TrivialLineInflater(TrivialLineBuffer const& input): input { input }
    {
        columns.reserve(unbox<size_t>(input.displayWidth));
    }

    InflatedLineBuffer<Cell> inflate() &&
    {
        vtParserLog()("Inflating TrivialLineBuffer: '{}'", input.text.data() ? crispy::escape(input.text.data()) : "");
        auto lineSegmenter = unicode::grapheme_line_segmenter { *this, input.text.view() };
        [[maybe_unused]] auto result = lineSegmenter.process(std::numeric_limits<unsigned>::max());
        assert(result.stop_condition == unicode::StopCondition::EndOfInput);
        [[maybe_unused]] auto const flushed = lineSegmenter.flush(std::numeric_limits<unsigned>::max());
        assert(flushed.stop_condition == unicode::StopCondition::EndOfInput);
        vtParserLog()("Inflated {}/{} columns", columns.size(), input.displayWidth);

        // Fill remaining columns
        for (unsigned i = columns.size(); i < unbox<size_t>(input.displayWidth); ++i)
        {
            columns.emplace_back(input.fillAttributes);
        }
        assert(columns.size() == unbox<size_t>(input.displayWidth));

        return std::move(columns);
    }

    void on_invalid(std::string_view /*invalid*/) noexcept
    {
        fmt::print("inflate invalid\n");
        static constexpr char32_t ReplacementCharacter { 0xFFFD };

        columns.emplace_back();
        columns.back().setHyperlink(input.hyperlink);
        columns.back().write(input.textAttributes, ReplacementCharacter, 1);
    }

    void on_ascii(std::string_view text) noexcept
    {
        fmt::print("inflate ASCII: '{}'\n", text);
        for (auto const ch: text)
        {
            columns.emplace_back();
            columns.back().setHyperlink(input.hyperlink);
            columns.back().write(input.textAttributes, ch, 1);
        }
    }

    void on_grapheme_cluster(std::string_view text, unsigned width) noexcept
    {
        fmt::print("inflate GC: '{}', width: {}\n", text, width);
        columns.emplace_back(input.textAttributes, input.hyperlink);
        Cell& cell = columns.back();
        cell.setHyperlink(input.hyperlink);

        auto utf8DecoderState = unicode::utf8_decoder_state {};
        for (auto const ch: text)
        {
            unicode::ConvertResult const r = unicode::from_utf8(utf8DecoderState, static_cast<uint8_t>(ch));
            if (auto const* cp = std::get_if<unicode::Success>(&r))
            {
                std::cout << fmt::format(" - codepoint: U+{:X}\n", (unsigned) cp->value);
                if (cell.codepointCount() == 0)
                    cell.setCharacter(cp->value);
                else
                    (void) cell.appendCharacter(cp->value);
            }
        }

        fmt::print(" -> result (UTF-8): \"{}\"\n", cell.toUtf8());

        // Fill remaining columns for wide characters
        for (unsigned i = 1; i < width; ++i)
        {
            std::cout << fmt::format(" - continuation\n");
            columns.emplace_back(input.textAttributes.with(CellFlag::WideCharContinuation), input.hyperlink);
            cell.setWidth(width);
        }
    }
};

template <CellConcept Cell>
InflatedLineBuffer<Cell> inflate(TrivialLineBuffer const& input)
{
    return TrivialLineInflater<Cell>(input).inflate();
}

} // end namespace vtbackend

// {{{ Explicit instantiation of Line<Cell> for supported cell types.
#include <vtbackend/cell/CompactCell.h>
#include <vtbackend/cell/SimpleCell.h>

namespace vtbackend
{

template class Line<CompactCell>;
template class Line<SimpleCell>;
template InflatedLineBuffer<SimpleCell> inflate(TrivialLineBuffer const& input);

} // namespace vtbackend
// }}}
