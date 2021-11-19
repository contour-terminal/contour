#include <terminal/Line.h>
#include <terminal/Cell.h>

namespace terminal
{

template <typename Cell, bool Optimize>
typename Line<Cell, Optimize>::Buffer Line<Cell, Optimize>::reflow(ColumnCount _newColumnCount)
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

                auto removedColumns = Buffer(reflowStart, reflowEnd);
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

template class Line<Cell, true>;
template class Line<Cell, false>;

} // end namespace terminal
