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
    Tab,      //!< A tab of two or more panes (`application/x-contour-node`).
    Pane,     //!< A pane card, or a tab of a single pane.
};

/// Number of @c DropPayload enumerators, derived from the last one.
inline constexpr size_t DropPayloadCount = static_cast<size_t>(DropPayload::Pane) + 1;

/// Where a dragged tab comes from, relative to the pane it hovers.
enum class DropOrigin : uint8_t
{
    OtherTab = 0, //!< From a tab other than the pane's own.
    OwnTab,       //!< From the tab the pane belongs to.
    OwnPane,      //!< From the very pane it hovers.
};

/// Why a pane or tab cannot be moved into the window it hovers.
enum class MoveRefusal : uint8_t
{
    None = 0,     //!< Nothing stands in the way.
    OtherProcess, //!< The window belongs to another Contour process.
    OldDaemon,    //!< The window's daemon is too old to move panes.
    Tmux,         //!< The window is a tmux window.
};

/// The title of the refusal shown for any @c MoveRefusal.
inline constexpr std::string_view RefusalTitle = "Can't move a pane here";

/// The body of the refusal for @p refusal.
/// @param refusal The reason.
/// @return The text; empty for @c MoveRefusal::None.
[[nodiscard]] constexpr std::string_view refusalTextOf(MoveRefusal refusal) noexcept
{
    switch (refusal)
    {
        case MoveRefusal::None: break;
        case MoveRefusal::OtherProcess:
            return "This window belongs to another Contour process. Run Contour in daemon mode to move panes "
                   "between any windows.";
        case MoveRefusal::OldDaemon:
            return "This window's daemon is too old to move panes. Update Contour on the daemon side.";
        case MoveRefusal::Tmux: return "Panes can't be moved into a tmux window yet.";
    }
    return {};
}

/// Where a pane or tab is dropped, as the move table tells the places apart.
enum class MovePlace : uint8_t
{
    Edge = 0,        //!< Beside a pane of the window the drag came from.
    EdgeOtherWindow, //!< Beside a pane of another window.
    Swap,            //!< Onto a pane, trading places with it.
    OnTab,           //!< Onto a tab in the strip: beside that tab's active pane.
    Gap,             //!< Into a gap of a tab strip: a reorder, or a move to another window.
    NewWindow,       //!< Outside every window: a window of its own.
};

/// Number of @c MovePlace enumerators, derived from the last one.
inline constexpr size_t MovePlaceCount = static_cast<size_t>(MovePlace::NewWindow) + 1;

/// What a drag moves, as the move table tells it apart.
enum class MoveSubject : uint8_t
{
    Pane = 0, //!< One pane out of a tab of several.
    WholeTab, //!< A whole tab: a tab, or the only pane of one.
};

/// What a window's host can move.
enum class HostMoves : uint8_t
{
    Nodes = 0, //!< Any pane or tab, anywhere: this process, or a daemon serving MoveNode.
    Tabs,      //!< A daemon older than MoveNode (v0.8.0): whole tabs, by the tab verbs it served.
    Nothing,   //!< A tmux server: nothing moves into its windows yet.
};

/// How a move is carried out.
enum class MoveRoute : uint8_t
{
    Refused = 0, //!< Not at all: the host serves no route for it.
    Node,        //!< As a node move: in process, or by the daemon's MoveNode.
    TabMove,     //!< By the tab verbs every daemon served: reorder, move to another window, tear off.
};

/// A row of the move table: what a host older than MoveNode still does for a drop.
struct MoveRouteRow
{
    HostMoves host;      ///< The host.
    MovePlace place;     ///< Where the drop is.
    MoveSubject subject; ///< What is dragged.
    MoveRoute route;     ///< How it is carried out.
};

/// The move table for hosts older than MoveNode: what the released daemon served before panes moved, and
/// only that. A host that moves nodes takes every drop as a node move; anything a row does not name is
/// refused.
inline constexpr auto LegacyMoveRoutes = std::array {
    MoveRouteRow { .host = HostMoves::Tabs,
                   .place = MovePlace::Gap,
                   .subject = MoveSubject::WholeTab,
                   .route = MoveRoute::TabMove },
    MoveRouteRow { .host = HostMoves::Tabs,
                   .place = MovePlace::NewWindow,
                   .subject = MoveSubject::WholeTab,
                   .route = MoveRoute::TabMove },
};

/// How @p host carries out a drop of @p subject at @p place.
/// @param host What the window's host can move.
/// @param place Where the drop is.
/// @param subject What is dragged.
/// @return Node for a host that moves nodes; otherwise the matching LegacyMoveRoutes row's route, or
///         Refused.
[[nodiscard]] constexpr MoveRoute moveRouteOf(HostMoves host, MovePlace place, MoveSubject subject) noexcept
{
    if (host == HostMoves::Nodes)
        return MoveRoute::Node;
    for (auto const& row: LegacyMoveRoutes)
        if (row.host == host && row.place == place && row.subject == subject)
            return row.route;
    return MoveRoute::Refused;
}

/// Whose ids a drag names, relative to the window it hovers.
enum class DragHost : uint8_t
{
    Own = 0, //!< The host of this window's sessions: this process, or the daemon it is attached to.
    Other,   //!< Another process, or another daemon.
};

/// How far a host's refusal reaches.
enum class RefusalScope : uint8_t
{
    Unrouted = 0, //!< Only the drops of its own host's ids that it has no route for.
    Every,        //!< Every drop, whoever's ids it names.
};

/// A row of the refusal table: what a host refuses, and for what.
struct HostRefusalRow
{
    HostMoves host;      ///< The host.
    MoveRefusal refusal; ///< What it refuses a drop for.
    RefusalScope scope;  ///< Which drops.
};

/// The refusal table, in HostMoves order.
inline constexpr auto HostRefusals = std::array {
    HostRefusalRow {
        .host = HostMoves::Nodes, .refusal = MoveRefusal::None, .scope = RefusalScope::Unrouted },
    HostRefusalRow {
        .host = HostMoves::Tabs, .refusal = MoveRefusal::OldDaemon, .scope = RefusalScope::Unrouted },
    HostRefusalRow { .host = HostMoves::Nothing, .refusal = MoveRefusal::Tmux, .scope = RefusalScope::Every },
};

static_assert(HostRefusals.size() == static_cast<size_t>(HostMoves::Nothing) + 1);

/// Why a drop cannot be made: the host's refusal where it refuses every drop, OtherProcess for a drag of
/// another host's ids, the host's refusal where it has no route for the drop, and None otherwise.
/// @param host  What the window's host can move.
/// @param from  Whose ids the drag names.
/// @param route How the host would carry the drop out (moveRouteOf()).
/// @return The refusal; None when nothing stands in the way.
[[nodiscard]] constexpr MoveRefusal moveRefusalOf(HostMoves host, DragHost from, MoveRoute route) noexcept
{
    auto const& row = HostRefusals[static_cast<size_t>(host)];
    if (row.scope == RefusalScope::Every)
        return row.refusal;
    if (from == DragHost::Other)
        return MoveRefusal::OtherProcess;
    return route == MoveRoute::Refused ? row.refusal : MoveRefusal::None;
}

/// Where a moved pane or tab landed, as a screen reader is told it.
enum class MoveLanding : uint8_t
{
    Beside = 0,          //!< Beside a pane of the window it came from.
    BesideInOtherWindow, //!< Beside a pane of another window.
    Swapped,             //!< Onto a pane, trading places with it.
    NewTab,              //!< Into a tab strip, as a tab of its own.
    NewWindow,           //!< Into a window of its own.
};

/// A row of the announcement table: a landing and what is said for a pane and for a tab.
struct MoveAnnouncementRow
{
    MoveLanding landing;       ///< The landing this row describes.
    std::string_view paneText; ///< What is said when a pane landed there (`%1`: the window's name).
    std::string_view tabText;  ///< What is said when a tab landed there.
};

/// The announcement table, in enumerator order.
inline constexpr auto MoveAnnouncementRows = std::array {
    MoveAnnouncementRow { .landing = MoveLanding::Beside, .paneText = "Pane moved", .tabText = "Tab moved" },
    MoveAnnouncementRow { .landing = MoveLanding::BesideInOtherWindow,
                          .paneText = "Pane moved to window %1",
                          .tabText = "Tab moved" },
    MoveAnnouncementRow {
        .landing = MoveLanding::Swapped, .paneText = "Panes swapped", .tabText = "Panes swapped" },
    MoveAnnouncementRow {
        .landing = MoveLanding::NewTab, .paneText = "Pane moved to new tab", .tabText = "Tab moved" },
    MoveAnnouncementRow {
        .landing = MoveLanding::NewWindow, .paneText = "Pane moved to new window", .tabText = "Tab moved" },
};

static_assert(MoveAnnouncementRows.size() == static_cast<size_t>(MoveLanding::NewWindow) + 1);

/// What a screen reader is told after @p payload landed at @p landing.
/// @param payload What moved: a Pane, or a Tab of several panes.
/// @param landing Where it landed.
/// @return The announcement (a pane moved into another window carries a literal `%1` for that window's name);
///         empty for a payload that is no pane or tab.
[[nodiscard]] constexpr std::string_view moveAnnouncementOf(DropPayload payload, MoveLanding landing) noexcept
{
    auto const& row = MoveAnnouncementRows[static_cast<size_t>(landing)];
    switch (payload)
    {
        case DropPayload::Pane: return row.paneText;
        case DropPayload::Tab: return row.tabText;
        case DropPayload::None:
        case DropPayload::Files:
        case DropPayload::Text: break;
    }
    return {};
}

/// What a drop over a pane does.
enum class DropZone : uint8_t
{
    None = 0,    //!< Not a drop target: nothing accepts it.
    InsertText,  //!< Files or text: typed into the pane's session as a paste.
    PassThrough, //!< A tab the pane leaves alone: not accepted by the pane, so the window takes it as a
                 //!< no-op.
    SplitLeft,   //!< A tab becomes the half to the left of the pane.
    SplitRight,  //!< A tab becomes the half to the right of the pane.
    SplitTop,    //!< A tab becomes the half above the pane.
    SplitBottom, //!< A tab becomes the half below the pane.
    Swap,        //!< A pane trades places with the one it hovers.
    NoOp,        //!< Accepted, changes nothing (a pane dropped onto itself).
    Refused,     //!< Accepted, changes nothing, and shows the refusal.
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

/// The width of the ring around a pane that takes the drag, in logical pixels: the design's 2 px.
inline constexpr double DropRingWidth = 2.0;

/// The width of the edge of a drop ghost and of a tab outlined by a drag, in logical pixels: the design's 1.5
/// px.
inline constexpr double DropEdgeWidth = 1.5;

/// Everything that decides where a drop lands.
struct DropZoneInputs
{
    DropPayload payload = DropPayload::None;  ///< What is dragged.
    DropExtent pane {};                       ///< The pane's extent.
    DropPoint at {};                          ///< The pointer.
    DropOrigin origin = DropOrigin::OtherTab; ///< Where a dragged tab or pane comes from.
    MoveRefusal refusal = MoveRefusal::None;  ///< Why a pane or tab cannot move here.
};

/// A row of the zone table: a zone, the side it splits at, and the ghost's labels.
struct DropZoneRow
{
    DropZone zone;                              ///< The zone this row describes.
    std::optional<vtworkspace::SplitSide> side; ///< The side a split zone grafts at; empty otherwise.
    std::string_view tabLabel;                  ///< The ghost's label for a tab; empty for none.
    std::string_view paneLabel;                 ///< The ghost's label for a pane; empty for none.
};

/// The zone table, in enumerator order.
inline constexpr auto DropZoneRows = std::array {
    DropZoneRow { .zone = DropZone::None, .side = std::nullopt, .tabLabel = {}, .paneLabel = {} },
    DropZoneRow { .zone = DropZone::InsertText, .side = std::nullopt, .tabLabel = {}, .paneLabel = {} },
    DropZoneRow { .zone = DropZone::PassThrough, .side = std::nullopt, .tabLabel = {}, .paneLabel = {} },
    DropZoneRow { .zone = DropZone::SplitLeft,
                  .side = vtworkspace::SplitSide::Left,
                  .tabLabel = "Tab → split left",
                  .paneLabel = "Pane → split left" },
    DropZoneRow { .zone = DropZone::SplitRight,
                  .side = vtworkspace::SplitSide::Right,
                  .tabLabel = "Tab → split right",
                  .paneLabel = "Pane → split right" },
    DropZoneRow { .zone = DropZone::SplitTop,
                  .side = vtworkspace::SplitSide::Top,
                  .tabLabel = "Tab → split up",
                  .paneLabel = "Pane → split up" },
    DropZoneRow { .zone = DropZone::SplitBottom,
                  .side = vtworkspace::SplitSide::Bottom,
                  .tabLabel = "Tab → split down",
                  .paneLabel = "Pane → split down" },
    DropZoneRow { .zone = DropZone::Swap, .side = std::nullopt, .tabLabel = {}, .paneLabel = "Swap with %1" },
    DropZoneRow { .zone = DropZone::NoOp, .side = std::nullopt, .tabLabel = {}, .paneLabel = {} },
    DropZoneRow { .zone = DropZone::Refused, .side = std::nullopt, .tabLabel = {}, .paneLabel = {} },
};

/// Number of @c DropZone enumerators, derived from the last one.
inline constexpr size_t DropZoneCount = static_cast<size_t>(DropZone::Refused) + 1;

static_assert(DropZoneRows.size() == DropZoneCount);

/// Returns the table row of @p zone.
/// @param zone The zone to look up.
[[nodiscard]] constexpr DropZoneRow const& dropZoneRowOf(DropZone zone) noexcept
{
    return DropZoneRows[static_cast<size_t>(zone)];
}

/// The ghost's label for @p zone when @p payload is dropped on it.
/// @param zone The zone.
/// @param payload What is dragged.
/// @return The label (a Swap label carries a literal `%1` for the pane's name); empty for none.
[[nodiscard]] constexpr std::string_view dropGhostLabelOf(DropZone zone, DropPayload payload) noexcept
{
    switch (payload)
    {
        case DropPayload::Tab: return dropZoneRowOf(zone).tabLabel;
        case DropPayload::Pane: return dropZoneRowOf(zone).paneLabel;
        case DropPayload::None:
        case DropPayload::Files:
        case DropPayload::Text: break;
    }
    return {};
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
/// Files and text insert anywhere over the pane and never split. A pane or tab that cannot move here is
/// refused; a pane over itself is a no-op. A pane splits in an edge band and swaps in the middle. A tab
/// splits only when the window can and it comes from another tab, and only in an edge band; elsewhere
/// the pane leaves it alone. In a corner the edge the pointer is proportionally nearer to wins, and a tie
/// goes to left/right.
/// @param in The payload, the pane, the pointer and the window's capabilities.
/// @return The zone; @c DropZone::None for no payload, an empty pane or a pointer outside it.
[[nodiscard]] constexpr DropZone resolveDropZone(DropZoneInputs const& in) noexcept
{
    auto const [width, height] = in.pane;
    auto const [x, y] = in.at;
    if (in.payload == DropPayload::None || width <= 0.0 || height <= 0.0 || x < 0.0 || y < 0.0 || x >= width
        || y >= height)
        return DropZone::None;

    if (in.payload == DropPayload::Files || in.payload == DropPayload::Text)
        return DropZone::InsertText;

    if (in.refusal != MoveRefusal::None)
        return DropZone::Refused;

    if (in.origin == DropOrigin::OwnPane)
        return DropZone::NoOp;

    auto const isPane = in.payload == DropPayload::Pane;
    if (!isPane && in.origin == DropOrigin::OwnTab)
        return DropZone::PassThrough;

    auto const left = detail::bandDepth(x, width);
    auto const right = detail::bandDepth(width - x, width);
    auto const top = detail::bandDepth(y, height);
    auto const bottom = detail::bandDepth(height - y, height);

    // The pointer is in a band when its depth is below one; the right and bottom bands start at the
    // far side, so their distance is measured from the far edge (which the pointer never reaches).
    auto zone = isPane ? DropZone::Swap : DropZone::PassThrough;
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
        case DropZone::Swap:
            return DropRect { .x = inset, .y = inset, .width = innerWidth, .height = innerHeight };
        case DropZone::None:
        case DropZone::InsertText:
        case DropZone::PassThrough:
        case DropZone::NoOp:
        case DropZone::Refused: break;
    }
    return std::nullopt;
}

} // namespace contour::window
