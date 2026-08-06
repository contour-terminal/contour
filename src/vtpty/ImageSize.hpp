// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cmath>
#include <format>

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

/// Represents the width in pixels of an image (see ImageSize).
using Width = boxed::boxed<unsigned, detail::tags::Width>;

/// Represents the height in pixels of an image (see ImageSize).
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

// NB: There is deliberately no operator< for ImageSize. A lexicographic order reads like a
// component-wise comparison at the call site, so `std::min(requested, limit)` silently clamps only
// the width -- min({100, 999999}, {1920, 1080}) yields {100, 999999}. Use the component-wise min()
// and max() below to clamp a size against a limit.

/// Component-wise subtraction, saturating at zero.
///
/// Width and Height are unsigned; a plain subtraction would wrap around on underflow.
/// Degenerate inputs (subtrahend larger than minuend) clamp the affected axis to zero
/// instead — clamp, don't wrap (same philosophy as contour's window-geometry module).
constexpr ImageSize operator-(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { .width = a.width > b.width ? a.width - b.width : Width(0),
                       .height = a.height > b.height ? a.height - b.height : Height(0) };
}

/// Component-wise division by a scalar, rounding UP to the next full pixel.
///
/// Used to derive downsampled pixmap sizes from supersampled ones; rounding up
/// guarantees the result never loses a partially covered pixel row/column.
inline ImageSize operator/(ImageSize a, double scalar) noexcept
{
    return ImageSize { .width = Width::cast_from(std::ceil(double(*a.width) / scalar)),
                       .height = Height::cast_from(std::ceil(double(*a.height) / scalar)) };
}

/// Component-wise multiplication by a scalar, rounding UP to the next full pixel.
///
/// Contract: callers size render targets and supersampling buffers with this
/// (logical size × content scale); rounding up guarantees the target is never
/// smaller than the content it must hold.
inline ImageSize operator*(ImageSize a, double scalar) noexcept
{
    return ImageSize { .width = Width::cast_from(std::ceil(double(*a.width) * scalar)),
                       .height = Height::cast_from(std::ceil(double(*a.height) * scalar)) };
}

/// Component-wise minimum: each axis is clamped independently.
///
/// This, not std::min, is how a size is clamped against a limit -- see the note on the absent
/// operator< above.
constexpr ImageSize min(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { .width = std::min(a.width, b.width), .height = std::min(a.height, b.height) };
}

/// Component-wise maximum: each axis is grown independently.
constexpr ImageSize max(ImageSize a, ImageSize b) noexcept
{
    return ImageSize { .width = std::max(a.width, b.width), .height = std::max(a.height, b.height) };
}

} // namespace vtpty

template <>
struct std::formatter<vtpty::ImageSize>: std::formatter<std::string>
{
    auto format(vtpty::ImageSize value, auto& ctx) const
    {
        return formatter<std::string>::format(std::format("{}x{}", value.width.value, value.height.value),
                                              ctx);
    }
};
