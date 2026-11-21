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
#include <memory>
#include <unordered_map>

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

/// @param value An ImageOrigin as the wire spells it.
/// @return The origin it names, falling back to Cursor for an out-of-range value: a cursor-placed image
///         paints its own alignment gap, so an unknown origin never leaves a gap unpainted.
[[nodiscard]] constexpr vtbackend::ImageOrigin imageOriginOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageOrigin::Placeholder).value_or(vtbackend::ImageOrigin::Cursor);
}

/// @param value An ImageFormat as the wire spells it.
/// @return The format it names, falling back to Auto for an out-of-range value — which
///         `isConsistentPixmap` rejects, so an unresolvable format cannot reach an upload.
[[nodiscard]] constexpr vtbackend::ImageFormat imageFormatOf(uint8_t value) noexcept
{
    return enumUpTo(value, vtbackend::ImageFormat::PNG).value_or(vtbackend::ImageFormat::Auto);
}

/// Numbers the placements a connection is told about, for ImageCellEntry::placement.
///
/// A placement is a RasterizedImage, and nothing in one names it across deltas: its address can be reused
/// once it is freed. So each is remembered beside a weak reference, and an address whose placement has
/// died is a new placement with a new ordinal. Ordinals are never reused within one instance.
class ImagePlacementOrdinals
{
  public:
    /// @param placement A placement held by a shared_ptr (every ImageFragment's is).
    /// @return The ordinal @p placement was given before while it lives, or a fresh one.
    [[nodiscard]] uint32_t ordinalOf(vtbackend::RasterizedImage const& placement)
    {
        auto& entry = _ordinals[&placement];
        if (entry.ordinal == 0 || entry.placement.lock().get() != &placement)
            entry = Entry { .placement = placement.weak_from_this(), .ordinal = ++_lastOrdinal };
        return entry.ordinal;
    }

    /// Forgets the placements that no longer exist, so the table stays as large as what is alive.
    void prune()
    {
        std::erase_if(_ordinals, [](auto const& item) { return item.second.placement.expired(); });
    }

    /// @return How many placements the table remembers.
    [[nodiscard]] std::size_t size() const noexcept { return _ordinals.size(); }

  private:
    struct Entry
    {
        std::weak_ptr<vtbackend::RasterizedImage const> placement;
        uint32_t ordinal = 0; ///< 0: not numbered yet.
    };
    std::unordered_map<vtbackend::RasterizedImage const*, Entry> _ordinals;
    uint32_t _lastOrdinal = 0;
};

} // namespace vthost
