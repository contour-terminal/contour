// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one drag payload a pane card and a tab both carry (`application/x-contour-node`): what is dragged,
/// whose model it names, and where it came from. It rides in an OS-level drag, so a drop reads it back from
/// whatever process started the drag; parseNodeDrag() trusts nothing of it.

#include <contour/window/DropZonePolicy.hpp>

#include <QtCore/QString>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::window
{

/// The mime type of a node drag.
inline constexpr auto NodeDragMime = "application/x-contour-node";

/// The most sessions a node drag names: more panes than any tab holds, so a payload naming more is forged,
/// and refused before its list is read.
inline constexpr auto MaxDragSessions = std::size_t { 1024 };

/// Who hosts the sessions a drag names.
enum class HostKind : uint8_t
{
    Process = 0, //!< This process, or another one: the sessions run where its windows are.
    Daemon,      //!< A Contour daemon, which any process attached to it can ask to move them.
};

/// The host of the sessions a drag names: one Contour process, or one run of a daemon.
///
/// It guards against confusion, not against an adversary: it tells two Contour instances' ids apart so one
/// never moves what another's ids happen to name. It authenticates nothing -- any application the drag
/// hovers can read it from the drag's data -- so nothing a drag asks for may need more than the user's own
/// pointer could do.
struct HostIdentity
{
    HostKind kind = HostKind::Process; ///< Process or daemon.
    /// The process's random token (@see processToken()), or the daemon instance in decimal.
    QString id;

    bool operator==(HostIdentity const&) const = default;
};

/// A node drag: a pane, or a tab of two or more panes.
struct NodeDrag
{
    DropPayload payload = DropPayload::None; ///< Pane or Tab; nothing else is a node.
    HostIdentity host;                       ///< Whose ids @c sessions are.
    vtworkspace::WindowId window;            ///< The window the drag started in, in the dragging process.
    int tabIndex = 0;                        ///< The row of the tab the drag started in.
    /// The host's ids of what is dragged: a pane's one session; a tab's every session, the first naming the
    /// tab. Local SessionId values in-process, daemon session ids when daemon-hosted.
    std::vector<uint64_t> sessions;

    bool operator==(NodeDrag const&) const = default;
};

/// Writes @p drag as the JSON of an `application/x-contour-node` drag.
/// @param drag A pane or tab drag.
/// @return The JSON (`kind`, `host`, `windowId`, `tabIndex`, `sessions`).
[[nodiscard]] QString writeNodeDrag(NodeDrag const& drag);

/// Reads the JSON of an `application/x-contour-node` drag. It is data from whichever process started the
/// drag, so nothing is assumed: a field missing, unknown, of the wrong type, non-integral or out of range, an
/// unknown kind, a session count the kind cannot have (a pane names one, a tab two to MaxDragSessions), or a
/// session named twice reads as no drag.
/// @param json The payload.
/// @return The drag, or std::nullopt.
[[nodiscard]] std::optional<NodeDrag> parseNodeDrag(QString const& json);

} // namespace contour::window
