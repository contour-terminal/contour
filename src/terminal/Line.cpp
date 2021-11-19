#include <terminal/Line.h>
#include <terminal/Cell.h>

#include <unicode/grapheme_segmenter.h>
#include <unicode/utf8.h>

using std::get;
using std::holds_alternative;
using std::min;

namespace terminal
{

template <typename Cell, bool Optimize>
typename Line<Cell, Optimize>::InflatedBuffer Line<Cell, Optimize>::reflow(ColumnCount _newColumnCount)
{
    using crispy::Comparison;
    switch (crispy::strongCompare(_newColumnCount, columnsUsed()))
    {
        case Comparison::Equal:
            break;
        case Comparison::Greater:
            buffer_.resize(unbox<size_t>(_newColumnCount));
            break;
        case Comparison::Less:
        {
            // TODO: properly handle wide character cells
            // - when cutting in the middle of a wide char, the wide char gets wrapped and an empty
            //   cell needs to be injected to match the expected column width.

            if (wrappable())
            {
                auto const [reflowStart, reflowEnd] = [this, _newColumnCount]()
                {
                    auto const reflowStart =
                        next(buffer_.begin(), *_newColumnCount /* - buffer_[_newColumnCount].width()*/);

                    auto reflowEnd = buffer_.end();

                    while (reflowEnd != reflowStart && prev(reflowEnd)->empty())
                        reflowEnd = prev(reflowEnd);

                    return std::tuple{reflowStart, reflowEnd};
                }();

                auto removedColumns = InflatedBuffer(reflowStart, reflowEnd);
                buffer_.erase(reflowStart, buffer_.end());
                assert(columnsUsed() == _newColumnCount);
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
                buffer_.resize(unbox<size_t>(_newColumnCount));
                assert(columnsUsed() == _newColumnCount);
                return {};
            }
        }
    }
    return {};
}

template <typename Cell, bool Optimize>
inline void Line<Cell, Optimize>::resize(ColumnCount _count)
{
    assert(*_count >= 0);
    buffer_.resize(unbox<size_t>(_count));
}

template <typename Cell, bool Optimize>
gsl::span<Cell const> Line<Cell, Optimize>::trim_blank_right() const noexcept
{
    auto i = buffer_.data();
    auto e = buffer_.data() + buffer_.size();

    while (i != e && (e - 1)->empty())
        --e;

    return gsl::make_span(i, e - i);
}

template <typename Cell, bool Optimize>
std::string Line<Cell, Optimize>::toUtf8() const
{
    std::string str;
    for (Cell const& cell: buffer_)
    {
        if (cell.codepointCount() == 0)
            str += ' ';
        else
            str += cell.toUtf8();
    }
    return str;
}

template <typename Cell, bool Optimize>
std::string Line<Cell, Optimize>::toUtf8Trimmed() const
{
    std::string output = toUtf8();
    while (!output.empty() && isspace(output.back()))
        output.pop_back();
    return output;
}

template <typename Cell>
InflatedLineBuffer<Cell> inflate(SimpleLineBuffer const& input)
{
    static constexpr char32_t ReplacementCharacter {0xFFFD};

    auto columns = InflatedLineBuffer<Cell>{};
    columns.reserve(unbox<size_t>(input.width));

    auto lastChar = char32_t{0};
    auto utf8DecoderState = unicode::utf8_decoder_state{};

    for (size_t i = 0; i < input.text.size(); ++i)
    {
        char ch = input.text[i];
        unicode::ConvertResult const r = unicode::from_utf8(utf8DecoderState, ch);
        if (holds_alternative<unicode::Incomplete>(r))
            continue;

        auto const nextChar = holds_alternative<unicode::Success>(r)
                            ? get<unicode::Success>(r).value
                            : ReplacementCharacter;
        auto const isAsciiBreakable = lastChar < 128 && nextChar < 128; // NB: This is an optimization for US-ASCII text versus grapheme cluster segmentation.

        if (!lastChar || isAsciiBreakable || unicode::grapheme_segmenter::breakable(lastChar, nextChar))
        {
            columns.emplace_back(Cell{});
            columns.back().write(input.attributes, nextChar,
                                 static_cast<uint8_t>(unicode::width(nextChar)));
        }
        else
        {
            Cell& prevCell = columns.back();
            auto const extendedWidth = prevCell.appendCharacter(nextChar);
            if (extendedWidth > 0)
            {
                auto const cellsAvailable = *input.width - static_cast<int>(columns.size()) + 1;
                auto const n = min(extendedWidth, cellsAvailable);
                for (int i = 1; i < n; ++i)
                    columns.emplace_back(Cell{input.attributes});
            }
        }
    }

    while (columns.size() < unbox<size_t>(input.width))
        columns.emplace_back(Cell{input.attributes});

    return columns;
}

template class Line<Cell, true>;
template class Line<Cell, false>;

} // end namespace terminal
