// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The image placement policies as they travel on the native protocol — the same shape
/// @ref vthost/MouseWire.h gives the mouse state, and for the same reason: a peer's byte becomes an
/// enumerator in exactly one place.
///
/// Decoding is total: a value a peer should not have sent degrades to that policy's default rather
/// than being cast into an enum that has no such enumerator. That distinction is not cosmetic here —
/// `computeTargetSize` and `computeTargetTopLeftOffset` (vtbackend/Image.cpp) switch over these
/// exhaustively and end in `std::unreachable()`, which the renderer reaches once per frame, on the
/// render thread, for every attached pane.

#include <vtbackend/core/Image.hpp>

#include <cstdint>

#include <vthost/WireEnum.hpp>

namespace vthost
{

/// @param value An ImageLayer as the wire spells it.
/// @return The layer it names, falling back to Replace — the layer a Sixel-style image occupies, and
///         what `ImageResize`'s own default pairs with — for an out-of-range value.
[[nodiscard]] constexpr vtbackend::ImageLayer imageLayerOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageLayer::Above).value_or(vtbackend::ImageLayer::Replace);
}

/// @param value An ImageResize as the wire spells it.
/// @return The resize policy it names, falling back to ResizeToFit (the documented default) for an
///         out-of-range value.
[[nodiscard]] constexpr vtbackend::ImageResize imageResizeOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageResize::StretchToFill)
        .value_or(vtbackend::ImageResize::ResizeToFit);
}

/// @param value An ImageAlignment as the wire spells it.
/// @return The alignment it names, falling back to MiddleCenter (the documented default) for an
///         out-of-range value.
[[nodiscard]] constexpr vtbackend::ImageAlignment imageAlignmentOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageAlignment::BottomEnd)
        .value_or(vtbackend::ImageAlignment::MiddleCenter);
}

/// @param value An ImageFormat as the wire spells it.
/// @return The format it names, falling back to Auto for an out-of-range value — which
///         `isConsistentPixmap` rejects, so an unresolvable format cannot reach an upload.
[[nodiscard]] constexpr vtbackend::ImageFormat imageFormatOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageFormat::PNG).value_or(vtbackend::ImageFormat::Auto);
}

} // namespace vthost
