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

/// The name QML reads a toast host by.
/// @param host The host.
/// @return "titleBand", "contentTop" or "contentBottom".
[[nodiscard]] constexpr std::string_view toastHostName(ToastHost host) noexcept
{
    switch (host)
    {
        case ToastHost::TitleBand: return "titleBand";
        case ToastHost::ContentTop: return "contentTop";
        case ToastHost::ContentBottom: return "contentBottom";
    }
    return "titleBand";
}

/// Whether the active pane's session has ended: its footer (Restart, Close) then sits at the content's
/// bottom.
enum class PaneFooter : uint8_t
{
    Hidden = 0, ///< The session runs; no footer.
    Shown,      ///< The session ended; its footer is at the bottom.
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
    PaneFooter footer = PaneFooter::Hidden; ///< Whether an ended session's footer is at the bottom.
    int coveredRows = ToastCoveredRows;     ///< The rows a toast at the content's top would cover.
};

/// Where the toast goes: in the title band, centred, whenever a band shows at the top and is tall enough to
/// hold it; otherwise at the top centre of the content -- or at its bottom centre when the active pane's
/// cursor is in the rows a toast at the top would cover, so it is never over the prompt row -- unless the
/// session has ended, whose footer at the bottom it must not cover (an ended session has no prompt). A
/// concealed full-screen band (RevealOnEdge) is not there to hold it, nor a bottom-edge band, whose centre is
/// not the top centre the toast belongs to; Minimal's bare band is shorter than a toast.
/// @param in What decides it.
/// @return Where the toast goes.
[[nodiscard]] constexpr ToastHost placeToast(ToastPlacementInputs const& in) noexcept
{
    auto const bandHolds = in.edge == BarEdge::Top
                           && (in.band == BarPresence::Shown || in.band == BarPresence::Pinned)
                           && in.bandHeight >= in.toastHeight;
    if (bandHolds)
        return ToastHost::TitleBand;
    if (in.footer == PaneFooter::Shown)
        return ToastHost::ContentTop;
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

/// Where the toast goes, decided from the band as QML names it (UiStyleProvider::titleBand()): what the
/// window controller's toastHost() answers, and its test double's. A name QML does not use reads as no band
/// at the top.
/// @param band The band's presence, by name.
/// @param edge Its edge, by name.
/// @param bandHeight The band's height, in logical pixels.
/// @param toastHeight The toast's height, in logical pixels.
/// @param cursorRow The active pane's cursor row.
/// @param footer Whether the active pane's session ended.
/// @return The host, by name (toastHostName()).
[[nodiscard]] constexpr std::string_view placeToastNamed(std::string_view band,
                                                         std::string_view edge,
                                                         int bandHeight,
                                                         int toastHeight,
                                                         int cursorRow,
                                                         PaneFooter footer) noexcept
{
    return toastHostName(placeToast(ToastPlacementInputs {
        .band = barPresenceNamed(band).value_or(BarPresence::Hidden),
        .edge = barEdgeNamed(edge).value_or(BarEdge::Top),
        .bandHeight = bandHeight,
        .toastHeight = toastHeight,
        .cursorRow = cursorRow,
        .footer = footer,
        .coveredRows = ToastCoveredRows,
    }));
}

} // namespace contour::window
