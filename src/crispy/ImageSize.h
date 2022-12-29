#pragma once

#include <crispy/boxed.h>

namespace crispy
{

namespace detail::tags
{
    struct Width
    {
    };
    struct Height
    {
    };
} // namespace detail::tags

/// Representsthe width in pixels of an image (see ImageSize).
using Width = crispy::boxed<unsigned, detail::tags::Width>;

/// Representsthe height in pixels of an image (see ImageSize).
using Height = crispy::boxed<unsigned, detail::tags::Height>;

/// ImageSize represents the 2-dimensional size of an image (pixmap).
struct ImageSize
{
    Width width;
    Height height;

    [[nodiscard]] constexpr size_t area() const noexcept
    {
        return unbox<size_t>(width) * unbox<size_t>(height);
    }
};

constexpr bool operator==(ImageSize a, ImageSize b) noexcept
{
    return a.width == b.width && a.height == b.height;
}
constexpr bool operator!=(ImageSize a, ImageSize b) noexcept
{
    return !(a == b);
}

constexpr bool operator<(ImageSize a, ImageSize b) noexcept
{
    return a.width < b.width || (a.width == b.width && a.height < b.height);
}

inline ImageSize operator+(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width + b.width, a.height + b.height };
}

inline ImageSize operator-(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width - b.width, a.height - b.height };
}

inline ImageSize operator*(ImageSize a, double scalar) noexcept
{
    return ImageSize { Width::cast_from(ceil(double(*a.width) * scalar)),
                       Height::cast_from(ceil(double(*a.height) * scalar)) };
}

inline ImageSize operator/(ImageSize a, double scalar) noexcept
{
    return ImageSize { Width::cast_from(ceil(double(*a.width) / scalar)),
                       Height::cast_from(ceil(double(*a.height) / scalar)) };
}

constexpr ImageSize operator/(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width / b.width, a.height / b.height };
}

} // namespace crispy

namespace fmt
{
template <>
struct formatter<crispy::ImageSize>
{
    template <typename ParseContext>
    constexpr auto parse(ParseContext& ctx)
    {
        return ctx.begin();
    }
    template <typename FormatContext>
    auto format(crispy::ImageSize value, FormatContext& ctx)
    {
        return fmt::format_to(ctx.out(), "{}x{}", value.width, value.height);
    }
};
} // namespace fmt
