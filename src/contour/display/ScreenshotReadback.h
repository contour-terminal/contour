// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
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

/// Decides whether a captured framebuffer buffer must be vertically flipped to land in a top-left-origin
/// image (as QImage expects).
///
/// The RHI normalizes *texture* readback to a top-left origin on every backend, so an offscreen-texture
/// capture needs no flip. A *swapchain backbuffer* readback, by contrast, follows the backend's framebuffer
/// origin: bottom-left on OpenGL (QRhi::isYUpInFramebuffer() == true), which must be flipped. Expressing the
/// policy as data (two booleans) keeps it unit-testable without a GL context and documents exactly when the
/// flip applies.
/// @param capturedFromTexture true if the pixels came from an offscreen QRhiTexture readback (top-left
/// origin,
///                            no flip); false if from the swapchain backbuffer.
/// @param framebufferIsYUp    QRhi::isYUpInFramebuffer() for the active backend (only consulted for a
///                            backbuffer capture).
/// @return true if the buffer's rows must be reversed to obtain a top-left-origin image.
[[nodiscard]] constexpr bool screenshotNeedsVerticalFlip(bool capturedFromTexture,
                                                         bool framebufferIsYUp) noexcept
{
    // Offscreen texture readback is already top-left; only a Y-up backbuffer needs flipping.
    return !capturedFromTexture && framebufferIsYUp;
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
/// @param flip       Whether to reverse the row order (see screenshotNeedsVerticalFlip()).
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

    for (std::size_t row = 0; row < rows; ++row)
    {
        auto const srcRow = row;
        auto const dstRow = flip ? (static_cast<std::size_t>(height) - 1 - row) : row;
        auto const src = source.subspan(srcRow * rowBytes, rowBytes);
        std::ranges::copy(src, out.begin() + static_cast<std::ptrdiff_t>(dstRow * rowBytes));
    }
    return out;
}

} // namespace contour::display
