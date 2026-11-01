// SPDX-License-Identifier: Apache-2.0
#include <vthost/NodeMove.hpp>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <utility>

#include <vthost/TabMerge.hpp>
#include <vtworkspace/Tab.hpp>

namespace vthost
{

namespace
{
    using Failure = std::unexpected<NodeMoveFailure>;

    [[nodiscard]] Failure refuse(NodeMoveError error)
    {
        return std::unexpected { NodeMoveFailure { .error = error } };
    }

    /// @return The source the request names, or why it names none.
    [[nodiscard]] std::expected<vtworkspace::MoveSource, NodeMoveFailure> sourceOf(
        vtworkspace::SessionModel& model, proto::MoveNode const& move)
    {
        auto const kind = move.sourceKind;
        if (kind != std::to_underlying(vtworkspace::MoveSourceKind::Pane)
            && kind != std::to_underlying(vtworkspace::MoveSourceKind::Tab))
            return refuse(NodeMoveError::UnknownKind);
        if (move.sourceSessions.empty())
            return refuse(NodeMoveError::UnknownSession);
        auto const [window, tab, leaf] =
            model.findSessionLeaf(vtworkspace::SessionId { move.sourceSessions.front() });
        if (window == nullptr || tab == nullptr || leaf == nullptr)
            return refuse(NodeMoveError::UnknownSession);
        if (kind == std::to_underlying(vtworkspace::MoveSourceKind::Pane))
        {
            if (move.sourceSessions.size() != 1)
                return refuse(NodeMoveError::UnknownKind); // malformed, not stale
            return vtworkspace::MoveSource { vtworkspace::PaneSource { leaf->session() } };
        }
        // The request names the whole tab it saw: moving what other clients added meanwhile would drag
        // panes nobody dragged.
        if (!namesWholeTab(*tab, move.sourceSessions))
            return refuse(NodeMoveError::StaleTab);
        return vtworkspace::MoveSource { vtworkspace::TabSource { window->id(), tab->id() } };
    }

    /// @return The target the request names, or why it names none.
    [[nodiscard]] std::expected<vtworkspace::MoveTarget, NodeMoveFailure> targetOf(
        vtworkspace::SessionModel& model, proto::MoveNode const& move)
    {
        using vtworkspace::MoveTargetKind;
        auto const session = vtworkspace::SessionId { move.targetSession };
        auto const [window, tab, leaf] = model.findSessionLeaf(session);
        switch (move.targetKind)
        {
            case std::to_underlying(MoveTargetKind::Edge): {
                auto const side = splitSideFrom(move.side);
                if (!side)
                    return refuse(NodeMoveError::UnknownSide);
                if (leaf == nullptr)
                    return refuse(NodeMoveError::UnknownSession);
                return vtworkspace::MoveTarget { vtworkspace::EdgeTarget { .pane = session, .side = *side } };
            }
            case std::to_underlying(MoveTargetKind::Swap):
                if (leaf == nullptr)
                    return refuse(NodeMoveError::UnknownSession);
                return vtworkspace::MoveTarget { vtworkspace::SwapTarget { .pane = session } };
            case std::to_underlying(MoveTargetKind::TabWell): {
                if (window == nullptr)
                    return refuse(NodeMoveError::UnknownSession);
                constexpr auto Largest = static_cast<uint64_t>(std::numeric_limits<int>::max());
                return vtworkspace::MoveTarget { vtworkspace::TabWellTarget {
                    .window = window->id(), .index = static_cast<int>(std::min(move.index, Largest)) } };
            }
            case std::to_underlying(MoveTargetKind::NewWindow):
                return vtworkspace::MoveTarget { vtworkspace::NewWindowTarget {} };
            default: return refuse(NodeMoveError::UnknownKind);
        }
    }
} // namespace

std::expected<vtworkspace::MoveResult, NodeMoveFailure> applyMoveNode(vtworkspace::SessionModel& model,
                                                                      proto::MoveNode const& move,
                                                                      MoveWindowHooks const& windows)
{
    // Both ends are read before the model is touched, so a refusal changes nothing.
    auto const source = sourceOf(model, move);
    if (!source)
        return std::unexpected { source.error() };
    auto target = targetOf(model, move);
    if (!target)
        return std::unexpected { target.error() };
    // sourceOf() found the session's pane, so it has a window.
    auto const sourceWindow =
        std::get<0>(model.findSessionLeaf(vtworkspace::SessionId { move.sourceSessions.front() }))->id();
    // The model would refuse a new window for what fills its window, but only once the window was made:
    // refused before, nothing is made, and no client sees a window come and go.
    if (std::holds_alternative<vtworkspace::NewWindowTarget>(*target) && model.fillsItsWindow(*source))
        return std::unexpected { NodeMoveFailure { .error = NodeMoveError::Model,
                                                   .model = vtworkspace::MoveError::NoOp } };
    // A new window is made by the host's own path, which names it and records who asked, and is then the
    // well of a move; a refused move takes the still-empty window away again.
    auto const made = std::holds_alternative<vtworkspace::NewWindowTarget>(*target)
                          ? std::optional { windows.create() }
                          : std::nullopt;
    auto const landing =
        made ? vtworkspace::MoveTarget { vtworkspace::TabWellTarget { .window = *made, .index = 0 } }
             : *target;
    auto moved = model.moveNode(*source, landing).transform_error([](vtworkspace::MoveError error) {
        return NodeMoveFailure { .error = NodeMoveError::Model, .model = error };
    });
    if (!moved)
    {
        if (made)
            windows.discard(*made);
        return moved;
    }
    // A window the move took its last tab out of is not left behind: its observers were told it holds none,
    // so a client closes its own window for it, and it goes.
    if (auto const* const emptied = model.window(sourceWindow);
        emptied != nullptr && emptied->empty() && sourceWindow != moved->window)
        windows.discard(sourceWindow);
    return moved;
}

} // namespace vthost
