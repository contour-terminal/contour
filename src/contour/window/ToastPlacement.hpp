// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/window/TitleBandLayout.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace contour::window
{

/// Where a window's toast goes.
enum class ToastHost : uint8_t
{
    TitleBand = 0, ///< Centred on the title band.
    ContentTop,    ///< At the top centre of the terminal content.
    ContentBottom, ///< At the bottom centre: the cursor is in the rows the top one would cover.
};

/// The rows at the top of the content a toast there would cover.
inline constexpr int ToastCoveredRows = 2;

/// What decides where the toast goes, as the toast shows.
struct ToastPlacementInputs
{
    BarPresence band = BarPresence::Hidden; ///< The title band's presence.
    BarEdge edge = BarEdge::Top;            ///< The edge it sits at.
    int bandHeight = 0;                     ///< Its height, in logical pixels.
    int toastHeight = 0;                    ///< The toast's height (size.toast), in logical pixels.
    int cursorRow = 0;                      ///< The active pane's cursor row, from the top of its page.
    int coveredRows = ToastCoveredRows;     ///< The rows a toast at the content's top would cover.
};

/// Where the toast goes: in the title band, centred, whenever a band shows at the top and is tall enough to
/// hold it; otherwise at the top centre of the content -- or at its bottom centre when the active pane's
/// cursor is in the rows a toast at the top would cover, so it is never over the prompt row. A concealed
/// full-screen band (RevealOnEdge) is not there to hold it, nor a bottom-edge band, whose centre is not the
/// top centre the toast belongs to; Minimal's bare band is shorter than a toast.
/// @param in What decides it.
/// @return Where the toast goes.
[[nodiscard]] constexpr ToastHost placeToast(ToastPlacementInputs const& in) noexcept
{
    auto const bandHolds = in.edge == BarEdge::Top
                           && (in.band == BarPresence::Shown || in.band == BarPresence::Pinned)
                           && in.bandHeight >= in.toastHeight;
    if (bandHolds)
        return ToastHost::TitleBand;
    return in.cursorRow < in.coveredRows ? ToastHost::ContentBottom : ToastHost::ContentTop;
}

/// The bar presence QML names @p name by (barPresenceName()); nothing for a name it does not use.
[[nodiscard]] constexpr std::optional<BarPresence> barPresenceNamed(std::string_view name) noexcept
{
    for (auto const index: std::views::iota(std::size_t { 0 }, BarPresenceCount))
        if (auto const presence = static_cast<BarPresence>(index); barPresenceName(presence) == name)
            return presence;
    return std::nullopt;
}

/// The bar edge QML names @p name by (barEdgeName()); nothing for a name it does not use.
[[nodiscard]] constexpr std::optional<BarEdge> barEdgeNamed(std::string_view name) noexcept
{
    for (auto const index: std::views::iota(std::size_t { 0 }, BarEdgeCount))
        if (auto const edge = static_cast<BarEdge>(index); barEdgeName(edge) == name)
            return edge;
    return std::nullopt;
}

} // namespace contour::window
