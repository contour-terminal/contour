#pragma once

#include <crispy/point.h>

#include <fmt/format.h>

#include <cmath>
#include <utility>

namespace crispy
{

struct [[nodiscard]] Size
{
    int width;
    int height;

    /// This iterator can be used to iterate through each and every point between (0, 0) and (width, height).
    struct iterator // NOLINT(readability-identifier-naming)
    {
      public:
        constexpr iterator(int width, int next) noexcept:
            _width { width }, _next { next }, _coord { makeCoordinate(next) }
        {
        }

        constexpr auto operator*() const noexcept { return _coord; }

        constexpr iterator& operator++() noexcept
        {
            _coord = makeCoordinate(++_next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return _next == other._next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return _next != other._next; }

      private:
        int _width;
        int _next;
        Point _coord { 0, 0 };

        constexpr Point makeCoordinate(int offset) const noexcept
        {
            return Point { offset % _width, offset / _width };
        }
    };

    [[nodiscard]] constexpr iterator begin() const noexcept { return iterator { width, 0 }; }
    [[nodiscard]] constexpr iterator end() const noexcept { return iterator { width, width * height }; }
};

constexpr Size::iterator begin(Size const& s) noexcept
{
    return s.begin();
}
constexpr Size::iterator end(Size const& s) noexcept
{
    return s.end();
}

constexpr int area(Size size) noexcept
{
    return size.width * size.height;
}

constexpr bool operator<(Size a, Size b) noexcept
{
    return a.width < b.width || (a.width == b.width && a.height < b.height);
}

constexpr bool operator==(Size const& a, Size const& b) noexcept
{
    return a.width == b.width && a.height == b.height;
}

constexpr bool operator!=(Size const& a, Size const& b) noexcept
{
    return !(a == b);
}

constexpr Size operator+(Size a, Size b) noexcept
{
    return Size { a.width + b.width, a.height + b.height };
}

constexpr Size operator-(Size a, Size b) noexcept
{
    return Size { a.width - b.width, a.height - b.height };
}

constexpr Size operator*(Size a, Size b) noexcept
{
    return Size { a.width * b.width, a.height * b.height };
}

inline Size operator*(Size a, double scalar) noexcept
{
    return Size { int(ceil(double(a.width) * scalar)), int(ceil(double(a.height) * scalar)) };
}

constexpr Size operator/(Size a, Size b) noexcept
{
    return Size { a.width / b.width, a.height / b.height };
}

} // end namespace crispy

template <>
struct fmt::formatter<crispy::Size>
{
    static auto parse(format_parse_context& ctx) -> format_parse_context::iterator { return ctx.begin(); }
    static auto format(const crispy::Size& value, format_context& ctx) -> format_context::iterator
    {
        return fmt::format_to(ctx.out(), "{}x{}", value.width, value.height);
    }
};
