// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SidebarJump.hpp>
#include <contour/window/SidebarJumps.hpp>

#include <utility>

namespace contour::window
{

SidebarJumps::SidebarJumps(session::WindowSwitching& switching,
                           Announce announce,
                           std::function<void()> changed):
    _switching { switching },
    _announce { std::move(announce) },
    _changed { std::move(changed) },
    _alive { std::make_shared<SidebarJumps*>(this) }
{
}

void SidebarJumps::jumpTo(vtworkspace::WorkspaceId window,
                          SidebarTarget const& target,
                          JumpMode mode,
                          std::expected<std::array<uint8_t, 16>, JumpRefusal> const& controlId,
                          std::optional<SidebarTarget> from)
{
    // A row of a run that has ended names nothing any more: said, and nothing is asked of anyone.
    if (!controlId)
    {
        _announce(JumpEvent::Failed, target, std::string { JumpStaleRunReason });
        return;
    }
    // The workspace by its control id, as a switch names one; a workspace whose daemon stated none (or a
    // daemon row, which names no workspace) leaves the choice to the daemon.
    auto workspace = *controlId == std::array<uint8_t, 16> {}
                         ? session::WorkspaceTarget { session::AnyWorkspace {} }
                         : session::WorkspaceTarget { vtcontrol::ObjectRef {
                               .kind = vtcontrol::ObjectKind::Workspace, .uuid = { .bytes = *controlId } } };
    auto switchTarget =
        session::switchTargetOf(target.daemon,
                                std::move(workspace),
                                session::LandingFocus { .tab = target.tab, .pane = target.pane },
                                target.instance);
    if (!switchTarget)
        return;
    // Pending before the request: the switch it replaces is told it ended as this one starts, and says
    // nothing, as its serial is no longer the one under way; and one that ends at once finds this one.
    auto const serial = _nextSerial++;
    auto const switchMode =
        mode == JumpMode::NewWindow ? session::SwitchMode::Open : session::SwitchMode::Switch;
    _pending = PendingJump { .serial = serial,
                             .target = target,
                             .from = std::move(from),
                             .mode = switchMode,
                             .then = {},
                             .cancel = JumpCancel::None };
    _announce(JumpEvent::Connecting, target, {});
    _changed();
    _switching.request(window,
                       *std::move(switchTarget),
                       switchMode,
                       session::NoLandingDeadline,
                       [alive = std::weak_ptr { _alive },
                        serial](std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome) {
                           if (auto const self = alive.lock())
                               (*self)->finish(serial, outcome);
                       });
}

std::optional<SidebarTarget> SidebarJumps::pending() const
{
    return _pending ? std::optional { _pending->target } : std::nullopt;
}

void SidebarJumps::cancel(vtworkspace::WorkspaceId window)
{
    if (!_pending)
        return;
    _pending->cancel = JumpCancel::Requested;
    _switching.cancel(window, _pending->mode);
}

void SidebarJumps::finish(uint64_t serial,
                          std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome)
{
    // A jump a newer one replaced says nothing: the newer one speaks for the window.
    if (!_pending || _pending->serial != serial)
        return;
    auto const ended = *std::exchange(_pending, std::nullopt);
    if (auto const event = jumpEventOf(outcome, ended.cancel))
        _announce(*event, ended.target, outcome ? std::string {} : outcome.error());
    if (outcome && ended.then)
        ended.then();
    _changed();
}

} // namespace contour::window
