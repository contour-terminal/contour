// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>

#include <boxed-cpp/boxed.hpp>

namespace vtpty
{

// clang-format off
namespace detail::tags
{
    struct Width {};
    struct Height {};
}
// clang-format on

/// Representsthe width in pixels of an image (see ImageSize).
using Width = boxed::boxed<unsigned, detail::tags::Width>;

/// Representsthe height in pixels of an image (see ImageSize).
using Height = boxed::boxed<unsigned, detail::tags::Height>;

/// ImageSize represents the 2-dimensional size of an image (pixmap).
struct ImageSize
{
    vtpty::Width width;
    vtpty::Height height;

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

constexpr ImageSize operator+(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width + b.width, a.height + b.height };
}

constexpr ImageSize operator-(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width - b.width, a.height - b.height };
}

constexpr ImageSize operator/(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { a.width / b.width, a.height / b.height };
}

inline ImageSize operator/(ImageSize a, double scalar) noexcept
{
    return ImageSize { Width::cast_from(std::ceil(double(*a.width) / scalar)),
                       Height::cast_from(std::ceil(double(*a.height) / scalar)) };
}

inline ImageSize operator*(ImageSize a, double scalar) noexcept
{
    return ImageSize { Width::cast_from(std::ceil(double(*a.width) * scalar)),
                       Height::cast_from(std::ceil(double(*a.height) * scalar)) };
}

} // namespace vtpty

template <>
struct fmt::formatter<vtpty::ImageSize>: fmt::formatter<std::string>
{
    auto format(vtpty::ImageSize value, format_context& ctx) -> format_context::iterator
    {
        return formatter<std::string>::format(fmt::format("{}x{}", value.width, value.height), ctx);
    }
};
