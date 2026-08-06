// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <span>
#include <vector>

namespace contour::display
{

/// Bytes per pixel of the RGBA8 screenshot format (4: R, G, B, A). Single source of truth for the
/// screenshot buffer-size arithmetic, kept next to the readback logic it governs.
inline constexpr std::size_t ScreenshotBytesPerPixel = 4;

/// Number of bytes a tightly-packed RGBA8 image of @p width x @p height occupies.
/// @param width  Image width in pixels.
/// @param height Image height in pixels.
/// @return width * height * 4, as a byte count.
[[nodiscard]] constexpr std::size_t screenshotBufferSize(int width, int height) noexcept
{
    if (width <= 0 || height <= 0)
        return 0;
    return static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * ScreenshotBytesPerPixel;
}

/// Copies a readback buffer into a tightly-packed, top-left-origin RGBA8 image buffer of the expected size,
/// optionally reversing rows (vertical flip) and defensively padding/truncating to the exact expected length.
///
/// The RHI may hand back a QByteArray whose length differs from width*height*4 (padded rows on some backends,
/// or an empty buffer before a capture completes); this normalizes it into a deterministic result the image
/// wrapper can consume without reading out of bounds. Pure and GPU-free so the flip + sizing is
/// unit-testable.
/// @param source     The raw readback bytes (may be shorter or longer than expected; may be empty).
/// @param width      Target image width in pixels.
/// @param height     Target image height in pixels.
/// @param flip       Whether to reverse the row order. Whose call this is depends on how the captured
///                   surface got its pixels; RhiRenderer::recordScreenshotPass() decides it.
/// @return A buffer of exactly screenshotBufferSize(width, height) bytes. Rows beyond what @p source supplies
///         are left zero-filled.
[[nodiscard]] inline std::vector<uint8_t> normalizeScreenshotBuffer(std::span<uint8_t const> source,
                                                                    int width,
                                                                    int height,
                                                                    bool flip)
{
    auto const expected = screenshotBufferSize(width, height);
    std::vector<uint8_t> out(expected, uint8_t { 0 });
    if (expected == 0)
        return out;

    auto const rowBytes = static_cast<std::size_t>(width) * ScreenshotBytesPerPixel;
    auto const availableRows = source.size() / rowBytes;
    auto const rows = std::min<std::size_t>(static_cast<std::size_t>(height), availableRows);

    for (auto const row: std::views::iota(std::size_t { 0 }, rows))
    {
        auto const dstRow = flip ? (static_cast<std::size_t>(height) - 1 - row) : row;
        auto const src = source.subspan(row * rowBytes, rowBytes);
        std::ranges::copy(src, out.begin() + static_cast<std::ptrdiff_t>(dstRow * rowBytes));
    }
    return out;
}

} // namespace contour::display
