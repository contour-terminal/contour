#pragma once

#include <vtpty/ImageSize.h>

#include <boxed-cpp/boxed.hpp>

namespace vtpty
{

namespace detail::tags
{
    struct LineCount
    {
    };
    struct ColumnCount
    {
    };
} // namespace detail::tags

/// ColumnCount simply represents a number of columns.
using ColumnCount = boxed::boxed<int, detail::tags::ColumnCount>;

/// LineCount represents a number of lines.
using LineCount = boxed::boxed<int, detail::tags::LineCount>;

struct PageSize
{
    LineCount lines;
    ColumnCount columns;

    [[nodiscard]] int area() const noexcept { return *lines * *columns; }
};

constexpr PageSize operator+(PageSize pageSize, LineCount lines) noexcept
{
    return PageSize { pageSize.lines + lines, pageSize.columns };
}

constexpr PageSize operator-(PageSize pageSize, LineCount lines) noexcept
{
    return PageSize { pageSize.lines - lines, pageSize.columns };
}

constexpr bool operator==(PageSize a, PageSize b) noexcept
{
    return a.lines == b.lines && a.columns == b.columns;
}

constexpr bool operator!=(PageSize a, PageSize b) noexcept
{
    return !(a == b);
}

constexpr ImageSize operator*(ImageSize a, PageSize b) noexcept
{
    return ImageSize { a.width * boxed_cast<Width>(b.columns), a.height * boxed_cast<Height>(b.lines) };
}

constexpr ImageSize operator/(ImageSize a, PageSize s) noexcept
{
    return { Width::cast_from(unbox(a.width) / unbox(s.columns)),
             Height::cast_from(unbox(a.height) / unbox(s.lines)) };
}
} // namespace vtpty
