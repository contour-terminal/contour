// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The one drag of a pane or tab this process runs. `QDrag::exec` is process-wide -- at most one OS drag is
/// in flight, whichever window started it -- so the session manager owns it: what it drags, resolved once
/// against the model, the window whose delegate carries it, the moves its drops ask for meanwhile, and what
/// the drag-end probe tracked of it.

#include <contour/window/DragEndProbe.hpp>
#include <contour/window/DropZonePolicy.hpp>
#include <contour/window/NodeDrag.hpp>

#include <QtCore/QObject>
#include <QtCore/QPointer>
#include <QtCore/QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include <vtworkspace/Primitives.hpp>

namespace contour::session
{

class TerminalSession;

/// How a drop of a drag at one place is carried out, and why it cannot be.
struct PlaceVerdict
{
    window::MoveRoute route = window::MoveRoute::Refused;    ///< How (window::moveRouteOf()).
    window::MoveRefusal refusal = window::MoveRefusal::None; ///< Why not (window::moveRefusalOf()).
};

/// A node drag resolved against this process's model: parsed and looked up once per drag, so that a hover
/// over it runs geometry only (@see TerminalSessionManager::resolveNodeDrag).
struct ResolvedDrag
{
    window::NodeDrag drag; ///< What the payload names.
    /// The local tab the drag is, or came from: the one tab that shows every session it names and, for a
    /// tab, no other. None for a drag of another host's, or of what is gone since.
    std::optional<vtworkspace::TabId> tab;
    vtworkspace::WindowId window {}; ///< The window of @c tab; meaningless without it.
    /// What it moves: a whole tab for a tab, or for the only pane of one.
    window::MoveSubject subject = window::MoveSubject::Pane;
    /// How it is dropped at each place, by MovePlace.
    std::array<PlaceVerdict, window::MovePlaceCount> places {};
    uint64_t layout = 0; ///< The layout generation it was resolved against.

    /// @return How a drop at @p place is carried out, and why it cannot be.
    [[nodiscard]] PlaceVerdict const& at(window::MovePlace place) const noexcept
    {
        return places[static_cast<std::size_t>(place)];
    }

    /// @return Why the drag cannot be dropped at @p place.
    [[nodiscard]] window::MoveRefusal refusalAt(window::MovePlace place) const noexcept
    {
        return at(place).refusal;
    }

    /// @return The place beside a pane of window @p target is: its own window's, or another's.
    [[nodiscard]] window::MovePlace edgePlaceIn(vtworkspace::WindowId target) const noexcept
    {
        return tab && window == target ? window::MovePlace::Edge : window::MovePlace::EdgeOtherWindow;
    }

    /// @return Whether it moves a whole tab.
    [[nodiscard]] bool movesWholeTab() const noexcept { return subject == window::MoveSubject::WholeTab; }

    /// @return What a screen reader is told moved, once it landed at @p landing: a tab of one pane taken
    ///         into a tab strip is the tab moved, not a pane made a tab.
    [[nodiscard]] window::DropPayload announcedAs(window::MoveLanding landing) const noexcept
    {
        return landing == window::MoveLanding::NewTab && movesWholeTab() ? window::DropPayload::Tab
                                                                         : drag.payload;
    }
};

/// The node drag this process runs, from the pick-up to its end: one object, so that ending it -- dropped,
/// cancelled, its window closing, or the next drag starting -- forgets everything about it at once.
class NodeDragSession
{
  public:
    /// @param owner   The window controller whose delegate carries the drag.
    /// @param payload The JSON the drag carries.
    /// @param drag    What it drags, resolved.
    /// @param carried The pane whose header carries it; null for a tab.
    /// @param probe   What tracks where the drag ends; told to forget it as this session ends.
    NodeDragSession(QObject const& owner,
                    QString payload,
                    ResolvedDrag drag,
                    TerminalSession* carried,
                    window::DragEndProbe& probe);

    /// The probe forgets the drag: the next one is tracked afresh.
    ~NodeDragSession();

    NodeDragSession(NodeDragSession const&) = delete;
    NodeDragSession& operator=(NodeDragSession const&) = delete;
    NodeDragSession(NodeDragSession&&) = delete;
    NodeDragSession& operator=(NodeDragSession&&) = delete;

    /// @return Whether @p host is the window controller that carries the drag.
    [[nodiscard]] bool isOwnedBy(QObject const* host) const noexcept
    {
        return host != nullptr && _owner.data() == host;
    }
    /// @return The JSON the drag carries.
    [[nodiscard]] QString const& payload() const noexcept { return _payload; }
    /// @return What it drags, as last resolved.
    [[nodiscard]] ResolvedDrag const& drag() const noexcept { return _drag; }
    /// Replaces what the drag was resolved as, once the layout it was resolved against changed.
    void reresolve(ResolvedDrag drag) { _drag = std::move(drag); }
    /// @return The pane whose header carries the drag; null for a tab, or a pane closed since.
    [[nodiscard]] TerminalSession* carried() const noexcept;
    /// @return Where the drag ends, as the probe tracked it.
    [[nodiscard]] window::DragEndProbe const& probe() const noexcept { return _probe.get(); }

    /// Holds @p move back until the drag has ended (@see TerminalSessionManager::afterNodeDrag).
    void hold(std::function<void()> move) { _held.push_back(std::move(move)); }
    /// @return The moves held back, which the session no longer holds.
    [[nodiscard]] std::vector<std::function<void()>> takeHeld() noexcept { return std::exchange(_held, {}); }

  private:
    QPointer<QObject const> _owner;
    QString _payload;
    ResolvedDrag _drag;
    QPointer<QObject> _carried; ///< A TerminalSession; held weakly, as its pane may close mid-drag.
    std::reference_wrapper<window::DragEndProbe> _probe;
    std::vector<std::function<void()>> _held;
};

} // namespace contour::session
