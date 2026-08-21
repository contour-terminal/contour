// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <vtbackend/core/Primitives.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace vtbackend
{

/// Largest palette encodeSixel() will emit.
///
/// Sixel names a color by register, and how many registers exist is the DECODER's business: the VT340
/// had 16, and 256 is what terminals since have converged on. Encoding to that floor keeps the output
/// readable by anything that reads sixel at all.
inline constexpr size_t MaxSixelPaletteSize = 256;

/// Encodes tightly-packed RGBA8 pixels as a complete sixel sequence, `DCS` envelope included.
///
/// The inverse of @ref SixelParser, and deliberately its neighbour: the two have to agree about the
/// same wire format, and a round-trip through both is what checks that they do.
///
/// The envelope is part of the result rather than the caller's to add, because its `P2` parameter
/// states that unpainted pixels stay as they were — which is a fact about how these pixels were
/// encoded, not a framing detail. What comes back can therefore be written to a terminal verbatim,
/// which is the whole reason this format exists.
///
/// **Color is not preserved exactly.** Sixel states a color as three *percentages*, so an 8-bit
/// channel passes through 101 levels and can come back off by one. Channels that are already whole
/// percentages (0, 51, 102, 153, 204, 255) survive exactly.
///
/// A pixel whose alpha is below half is left unpainted rather than blended down: sixel has no alpha
/// channel, and leaving a pixel out is the only way to say "nothing here" rather than "black here".
///
/// @param rgba Tightly-packed RGBA8 pixels, top-left origin; exactly `width * height * 4` bytes.
/// @param size The image's extent; both axes must be non-zero.
/// @return The sixel sequence, `ESC P` … `ESC \` included.
[[nodiscard]] std::string encodeSixel(std::span<uint8_t const> rgba, ImageSize size);

} // namespace vtbackend
