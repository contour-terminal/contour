// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <QtGui/QImage>

#include <cstdint>
#include <vector>

namespace contour::display
{

/// The pixel boundary between Qt and vtbackend, in one place.
///
/// Two unrelated features cross it — the `OSC 533` screenshot encoder going out, and the PNG image
/// decoder (`TerminalDisplay::setSession`) coming in — and both need the same two rules to be right:
/// straight (not premultiplied) alpha, and rows tightly packed. Stating them twice is what this
/// header exists to avoid, because getting the second one wrong produces a *skewed image* rather
/// than a crash, so a fix applied to one copy would not visibly fail in the other.

/// Converts to 8-bit-per-channel RGBA with straight alpha, rows top to bottom.
///
/// A real conversion, not a relabelling, wherever the source is premultiplied — as an RHI readback
/// is: a PNG written from premultiplied data has every translucent pixel too dark.
///
/// @param image Any QImage.
/// @return @p image in the wire format, or a null image if the conversion failed.
[[nodiscard]] QImage toWireFormat(QImage const& image);

/// @param image The image to measure.
/// @return @p image's extent, as vtbackend names it.
[[nodiscard]] vtbackend::ImageSize extentOf(QImage const& image) noexcept;

/// @param image The image to inspect; must already be in @ref toWireFormat's format.
/// @return Whether @p image's rows are already width * 4 bytes apart, so its own buffer can be read
///         directly instead of being copied. @see tightlyPackedRgba.
[[nodiscard]] bool isTightlyPacked(QImage const& image) noexcept;

/// Copies @p image out row by row rather than in one block.
///
/// QImage pads rows to its own alignment, so bytesPerLine() is not width * 4 in general, while every
/// consumer on the vtbackend side wants exactly width * height * 4 bytes.
///
/// At 32 bits per pixel Qt's stride formula happens to come out to exactly width * 4, so in practice
/// this copies a buffer that was already in the right shape — which is why @ref isTightlyPacked
/// exists for callers that only need to READ the pixels. The copy stays for callers that need to own
/// them, and because "happens to" is not a guarantee to build on.
///
/// @param image The image to copy; must already be in @ref toWireFormat's format.
/// @return Its pixels, tightly packed.
[[nodiscard]] std::vector<uint8_t> tightlyPackedRgba(QImage const& image);

} // namespace contour::display
