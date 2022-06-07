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
    struct iterator
    {
      public:
        constexpr iterator(int _width, int _next) noexcept:
            width { _width }, next { _next }, coord { makeCoordinate(_next) }
        {
        }

        constexpr auto operator*() const noexcept { return coord; }

        constexpr iterator& operator++() noexcept
        {
            coord = makeCoordinate(++next);
            return *this;
        }

        constexpr iterator& operator++(int) noexcept
        {
            ++*this;
            return *this;
        }

        constexpr bool operator==(iterator const& other) const noexcept { return next == other.next; }
        constexpr bool operator!=(iterator const& other) const noexcept { return next != other.next; }

      private:
        int width;
        int next;
        Point coord { 0, 0 };

        constexpr Point makeCoordinate(int offset) noexcept
        {
            return Point { offset % width, offset / width };
        }
    };

    constexpr iterator begin() const noexcept { return iterator { width, 0 }; }
    constexpr iterator end() const noexcept { return iterator { width, width * height }; }

    constexpr iterator begin() noexcept { return iterator { width, 0 }; }
    constexpr iterator end() noexcept { return iterator { width, width * height }; }
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

constexpr bool operator==(Size const& _a, Size const& _b) noexcept
{
    return _a.width == _b.width && _a.height == _b.height;
}

constexpr bool operator!=(Size const& _a, Size const& _b) noexcept
{
    return !(_a == _b);
}

constexpr Size operator+(Size _a, Size _b) noexcept
{
    return Size { _a.width + _b.width, _a.height + _b.height };
}

constexpr Size operator-(Size _a, Size _b) noexcept
{
    return Size { _a.width - _b.width, _a.height - _b.height };
}

constexpr Size operator*(Size _a, Size _b) noexcept
{
    return Size { _a.width * _b.width, _a.height * _b.height };
}

inline Size operator*(Size _a, double _scalar) noexcept
{
    return Size { int(ceil(double(_a.width) * _scalar)), int(ceil(double(_a.height) * _scalar)) };
}

constexpr Size operator/(Size _a, Size _b) noexcept
{
    return Size { _a.width / _b.width, _a.height / _b.height };
}

} // end namespace crispy

namespace fmt
{
template <>
struct formatter<crispy::Size>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(const crispy::Size& value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}x{}", value.width, value.height);
    }
};
} // namespace fmt
