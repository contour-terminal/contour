// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>

namespace contour::display
{

/// Copies tightly packed RGBA pixels, converting them from straight to premultiplied alpha on the way.
///
/// An image texture is filtered and mipmapped, and both average neighbouring texels. Averaged as
/// straight alpha, a transparent texel's colour -- usually black -- bleeds into the opaque texels beside
/// it and darkens their edges; averaged premultiplied, each texel counts by its coverage, as it should.
/// The colour channels are rounded to the nearest value, so an opaque or fully transparent texel is
/// unchanged in every channel it keeps.
///
/// @param straight      RGBA pixels with straight alpha, four bytes each.
/// @param premultiplied Where the premultiplied pixels go; as long as @p straight. A trailing partial
///                      pixel is copied as it is.
constexpr void premultiplyAlpha(std::span<uint8_t const> straight, std::span<uint8_t> premultiplied) noexcept
{
    auto const length = std::min(straight.size(), premultiplied.size());
    auto const whole = length / 4 * 4;
    for (auto const offset: std::views::iota(size_t { 0 }, whole / 4)
                                | std::views::transform([](size_t pixel) { return pixel * 4; }))
    {
        auto const alpha = unsigned { straight[offset + 3] };
        for (auto const channel: std::views::iota(size_t { 0 }, size_t { 3 }))
            premultiplied[offset + channel] =
                static_cast<uint8_t>(((unsigned { straight[offset + channel] } * alpha) + 127) / 255);
        premultiplied[offset + 3] = straight[offset + 3];
    }
    std::ranges::copy(straight.subspan(whole, length - whole),
                      premultiplied.begin() + static_cast<ptrdiff_t>(whole));
}

} // namespace contour::display
