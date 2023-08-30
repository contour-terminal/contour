#pragma once

#include <cmath>

#include <boxed-cpp/boxed.hpp>

namespace crispy
{

namespace detail::tags
{
    struct width
    {
    };
    struct height
    {
    };
} // namespace detail::tags

/// Representsthe width in pixels of an image (see ImageSize).
using width = boxed::boxed<unsigned, detail::tags::width>;

/// Representsthe height in pixels of an image (see ImageSize).
using height = boxed::boxed<unsigned, detail::tags::height>;

/// ImageSize represents the 2-dimensional size of an image (pixmap).
struct image_size
{
    crispy::width width;
    crispy::height height;

    [[nodiscard]] constexpr size_t area() const noexcept
    {
        return unbox<size_t>(width) * unbox<size_t>(height);
    }
};

constexpr bool operator==(image_size a, image_size b) noexcept
{
    return a.width == b.width && a.height == b.height;
}
constexpr bool operator!=(image_size a, image_size b) noexcept
{
    return !(a == b);
}

constexpr bool operator<(image_size a, image_size b) noexcept
{
    return a.width < b.width || (a.width == b.width && a.height < b.height);
}

inline image_size operator+(image_size a, image_size b) noexcept
{
    return image_size { a.width + b.width, a.height + b.height };
}

inline image_size operator-(image_size a, image_size b) noexcept
{
    return image_size { a.width - b.width, a.height - b.height };
}

inline image_size operator*(image_size a, double scalar) noexcept
{
    return image_size { width::cast_from(std::ceil(double(*a.width) * scalar)),
                        height::cast_from(std::ceil(double(*a.height) * scalar)) };
}

inline image_size operator/(image_size a, double scalar) noexcept
{
    return image_size { width::cast_from(std::ceil(double(*a.width) / scalar)),
                        height::cast_from(std::ceil(double(*a.height) / scalar)) };
}

constexpr image_size operator/(image_size a, image_size b) noexcept
{
    return image_size { a.width / b.width, a.height / b.height };
}

} // namespace crispy

template <>
struct fmt::formatter<crispy::image_size>: fmt::formatter<std::string>
{
    auto format(crispy::image_size value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x{}", value.width, value.height), ctx);
    }
};
