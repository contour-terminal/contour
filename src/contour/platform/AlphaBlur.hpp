// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <vector>

namespace contour::platform
{

/// A single-channel coverage plane.
///
/// A shadow is a blurred silhouette: only the alpha channel ever changes, so carrying three colour
/// channels through the blur would treble the work for bytes that stay constant.
struct AlphaPlane
{
    int width = 0;
    int height = 0;
    std::vector<uint8_t> samples;

    AlphaPlane(int w, int h):
        width { w }, height { h }, samples(static_cast<size_t>(w) * static_cast<size_t>(h), 0)
    {
    }

    [[nodiscard]] uint8_t& at(int x, int y) noexcept
    {
        return samples[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)];
    }

    [[nodiscard]] uint8_t at(int x, int y) const noexcept
    {
        return samples[(static_cast<size_t>(y) * static_cast<size_t>(width)) + static_cast<size_t>(x)];
    }
};

namespace detail
{
    /// One box-blur pass along a single row or column, as a sliding window.
    ///
    /// @param read   Samples the source at index i.
    /// @param write  Stores the result at index i.
    /// @param length How many samples the line has.
    /// @param radius Half the box width. The window spans [i - radius, i + radius].
    template <typename Read, typename Write>
    void boxBlurLine(Read read, Write write, int length, int radius)
    {
        // Edges extend rather than wrap, so the sum starts pre-loaded with `radius` copies of the
        // first sample -- otherwise a shadow would fade out at the plane's border, where it should
        // be uniform for the tile that gets stretched.
        auto sum = static_cast<int>(read(0)) * (radius + 1);
        for (auto const i: std::views::iota(1, std::min(radius + 1, length)))
            sum += read(i);
        if (length < radius + 1)
            sum += static_cast<int>(read(length - 1)) * (radius + 1 - length);

        auto const window = (2 * radius) + 1;
        // Divide by a reciprocal rather than by `window`: this is the innermost statement of six
        // passes over the whole plane, and a runtime integer division here is about a fifth of the
        // render. The ceil-reciprocal at 24 fractional bits reproduces the truncating division
        // exactly for every value a sum can take (at most 255 * window), so the output is
        // byte-for-byte identical -- which the determinism test would otherwise catch.
        auto const reciprocal = ((uint64_t { 1 } << 24) + window - 1) / static_cast<uint64_t>(window);
        for (auto const i: std::views::iota(0, length))
        {
            write(i, static_cast<uint8_t>((static_cast<uint64_t>(sum) * reciprocal) >> 24));
            auto const leaving = read(std::max(0, i - radius));
            auto const entering = read(std::min(length - 1, i + radius + 1));
            sum += static_cast<int>(entering) - static_cast<int>(leaving);
        }
    }
} // namespace detail

/// Blurs @p plane in place with a Gaussian of the given box @p radius.
///
/// Three box passes, which is the standard approximation of a Gaussian: the error is below what any
/// eye resolves in a drop shadow, and unlike Qt's private qt_blurImage it is ours, so it is
/// deterministic across Qt versions and testable without a paint device.
///
/// Shared by the window's shadow (published to the compositor as tiles) and the popups' (drawn into
/// one nine-patch image). Stacking translucent rectangles in QML was tried for the latter and is
/// what a real blur exists to avoid: eight steps over twenty-four pixels are invisible against a
/// dark background and visibly banded against a light one.
inline void blurAlphaPlane(AlphaPlane& plane, int radius)
{
    if (radius <= 0)
        return;

    auto scratch = AlphaPlane { plane.width, plane.height };
    auto const width = plane.width;
    auto const height = plane.height;

    // Both passes go through AlphaPlane::at rather than indexing by hand: four open-coded
    // `y * width + x` expressions are four chances at a silent off-by-one.
    for ([[maybe_unused]] auto const pass: std::views::iota(0, 3))
    {
        for (auto const y: std::views::iota(0, height))
            detail::boxBlurLine([&](int x) { return plane.at(x, y); },
                                [&](int x, uint8_t v) { scratch.at(x, y) = v; },
                                width,
                                radius);

        for (auto const x: std::views::iota(0, width))
            detail::boxBlurLine([&](int y) { return scratch.at(x, y); },
                                [&](int y, uint8_t v) { plane.at(x, y) = v; },
                                height,
                                radius);
    }
}

/// The box radius whose three passes reach as far as a Gaussian of CSS blur @p radius does.
///
/// Three boxes of half-width r reach 3r; the Gaussian's reach is about 1.41 * radius, and
/// r = radius / 2 gives 1.5 * radius -- close enough that the region cut around the silhouette
/// still contains the whole falloff.
[[nodiscard]] inline int boxRadiusFor(int radius) noexcept
{
    return std::max(1, (radius + 1) / 2);
}

} // namespace contour::platform
