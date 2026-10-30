// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// What is being dragged over a pane.
enum class DropPayload : uint8_t
{
    None = 0, //!< Nothing the pane understands.
    Files,    //!< One or more files (a `text/uri-list` of file URLs).
    Text,     //!< Plain text or a non-file URL.
    Tab,      //!< A tab (`application/x-contour-tab`).
};

/// Number of @c DropPayload enumerators, derived from the last one.
inline constexpr size_t DropPayloadCount = static_cast<size_t>(DropPayload::Tab) + 1;

/// Whether the window can split a pane for a dropped tab.
enum class SplitSupport : uint8_t
{
    Unsupported =
        0,     //!< The window's session host cannot merge a tab (a daemon without Capability::TabMerge).
    Supported, //!< The window's session host merges a tab: in-process, or a daemon that serves it.
};

/// Where a dragged tab comes from, relative to the pane it hovers.
enum class DropOrigin : uint8_t
{
    OtherTab = 0, //!< From a tab other than the pane's own.
    OwnTab,       //!< From the tab the pane belongs to.
};

/// What a drop over a pane does.
enum class DropZone : uint8_t
{
    None = 0,    //!< Not a drop target: nothing accepts it.
    InsertText,  //!< Files or text: typed into the pane's session as a paste.
    PassThrough, //!< A tab the pane leaves alone: not accepted, so the drag ends as it does today (tear-off).
    SplitLeft,   //!< A tab becomes the half to the left of the pane.
    SplitRight,  //!< A tab becomes the half to the right of the pane.
    SplitTop,    //!< A tab becomes the half above the pane.
    SplitBottom, //!< A tab becomes the half below the pane.
};

/// A pane's extent in logical pixels.
struct DropExtent
{
    double width = 0.0;  ///< Its width.
    double height = 0.0; ///< Its height.
};

/// A pointer position in the pane's coordinates.
struct DropPoint
{
    double x = 0.0; ///< Distance from the pane's left edge.
    double y = 0.0; ///< Distance from the pane's top edge.
};

/// A rectangle in the pane's coordinates.
struct DropRect
{
    double x = 0.0;      ///< Its left edge.
    double y = 0.0;      ///< Its top edge.
    double width = 0.0;  ///< Its width.
    double height = 0.0; ///< Its height.

    bool operator==(DropRect const&) const = default;
};

/// The share of the pane's width (left, right) or height (top, bottom) that an edge band takes.
inline constexpr double DropEdgeFraction = 0.20;

/// Everything that decides where a drop lands.
struct DropZoneInputs
{
    DropPayload payload = DropPayload::None;        ///< What is dragged.
    DropExtent pane {};                             ///< The pane's extent.
    DropPoint at {};                                ///< The pointer.
    SplitSupport split = SplitSupport::Unsupported; ///< Whether this window can split for a tab.
    DropOrigin origin = DropOrigin::OtherTab;       ///< Where a dragged tab comes from.
};

/// A row of the zone table: a zone, the side it splits at, and the ghost's label.
struct DropZoneRow
{
    DropZone zone;                              ///< The zone this row describes.
    std::optional<vtworkspace::SplitSide> side; ///< The side a split zone grafts at; empty otherwise.
    std::string_view label;                     ///< The ghost's label; empty for a zone with no ghost.
};

/// The zone table, in enumerator order.
inline constexpr auto DropZoneRows = std::array {
    DropZoneRow { .zone = DropZone::None, .side = std::nullopt, .label = {} },
    DropZoneRow { .zone = DropZone::InsertText, .side = std::nullopt, .label = {} },
    DropZoneRow { .zone = DropZone::PassThrough, .side = std::nullopt, .label = {} },
    DropZoneRow {
        .zone = DropZone::SplitLeft, .side = vtworkspace::SplitSide::Left, .label = "Tab → split left" },
    DropZoneRow {
        .zone = DropZone::SplitRight, .side = vtworkspace::SplitSide::Right, .label = "Tab → split right" },
    DropZoneRow {
        .zone = DropZone::SplitTop, .side = vtworkspace::SplitSide::Top, .label = "Tab → split up" },
    DropZoneRow {
        .zone = DropZone::SplitBottom, .side = vtworkspace::SplitSide::Bottom, .label = "Tab → split down" },
};

/// Number of @c DropZone enumerators, derived from the last one.
inline constexpr size_t DropZoneCount = static_cast<size_t>(DropZone::SplitBottom) + 1;

static_assert(DropZoneRows.size() == DropZoneCount);

/// Returns the table row of @p zone.
/// @param zone The zone to look up.
[[nodiscard]] constexpr DropZoneRow const& dropZoneRowOf(DropZone zone) noexcept
{
    return DropZoneRows[static_cast<size_t>(zone)];
}

namespace detail
{
    /// How deep into an edge band the pointer is: 0 at the pane's edge, 1 at the band's inner border.
    /// Above 1 when the pointer is outside the band.
    [[nodiscard]] constexpr double bandDepth(double distanceFromEdge, double extent) noexcept
    {
        return distanceFromEdge / (extent * DropEdgeFraction);
    }
} // namespace detail

/// Decides what dropping @p in's payload at its pointer does.
///
/// Files and text insert anywhere over the pane and never split. A tab splits only when the window can
/// and it comes from another tab, and only in an edge band; elsewhere the pane leaves it alone. In a
/// corner the edge the pointer is proportionally nearer to wins, and a tie goes to left/right.
/// @param in The payload, the pane, the pointer and the window's capabilities.
/// @return The zone; @c DropZone::None for no payload, an empty pane or a pointer outside it.
[[nodiscard]] constexpr DropZone resolveDropZone(DropZoneInputs const& in) noexcept
{
    auto const [width, height] = in.pane;
    auto const [x, y] = in.at;
    if (in.payload == DropPayload::None || width <= 0.0 || height <= 0.0 || x < 0.0 || y < 0.0 || x >= width
        || y >= height)
        return DropZone::None;

    if (in.payload != DropPayload::Tab)
        return DropZone::InsertText;

    if (in.split == SplitSupport::Unsupported || in.origin == DropOrigin::OwnTab)
        return DropZone::PassThrough;

    auto const left = detail::bandDepth(x, width);
    auto const right = detail::bandDepth(width - x, width);
    auto const top = detail::bandDepth(y, height);
    auto const bottom = detail::bandDepth(height - y, height);

    // The pointer is in a band when its depth is below one; the right and bottom bands start at the
    // far side, so their distance is measured from the far edge (which the pointer never reaches).
    auto zone = DropZone::PassThrough;
    auto best = 1.0;
    auto const consider = [&](double depth, DropZone candidate) {
        if (depth < best)
        {
            best = depth;
            zone = candidate;
        }
    };
    // Left/right first, so that a tie keeps the vertical edge.
    consider(left, DropZone::SplitLeft);
    consider(right, DropZone::SplitRight);
    consider(top, DropZone::SplitTop);
    consider(bottom, DropZone::SplitBottom);
    return zone;
}

/// The ghost for a split zone: the half of the pane on the zone's side, inset from the pane's edges.
/// @param zone The zone.
/// @param pane The pane's extent.
/// @param inset How far the ghost sits from the pane's edges.
/// @return The rectangle; empty for a zone that is not a split.
[[nodiscard]] constexpr std::optional<DropRect> dropGhostOf(DropZone zone,
                                                            DropExtent pane,
                                                            double inset) noexcept
{
    auto const halfWidth = pane.width / 2.0;
    auto const halfHeight = pane.height / 2.0;
    auto const doubleInset = 2.0 * inset;
    auto const innerWidth = pane.width - doubleInset;
    auto const innerHeight = pane.height - doubleInset;
    auto const halfInnerWidth = halfWidth - doubleInset;
    auto const halfInnerHeight = halfHeight - doubleInset;
    switch (zone)
    {
        case DropZone::SplitLeft:
            return DropRect { .x = inset, .y = inset, .width = halfInnerWidth, .height = innerHeight };
        case DropZone::SplitRight:
            return DropRect {
                .x = halfWidth + inset, .y = inset, .width = halfInnerWidth, .height = innerHeight
            };
        case DropZone::SplitTop:
            return DropRect { .x = inset, .y = inset, .width = innerWidth, .height = halfInnerHeight };
        case DropZone::SplitBottom:
            return DropRect {
                .x = inset, .y = halfHeight + inset, .width = innerWidth, .height = halfInnerHeight
            };
        case DropZone::None:
        case DropZone::InsertText:
        case DropZone::PassThrough: break;
    }
    return std::nullopt;
}

} // namespace contour::window
