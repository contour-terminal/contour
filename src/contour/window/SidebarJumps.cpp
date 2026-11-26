// SPDX-License-Identifier: Apache-2.0
#include <contour/session/SidebarJump.hpp>
#include <contour/window/SidebarJumps.hpp>

#include <algorithm>
#include <cstddef>
#include <utility>

namespace contour::window
{

namespace
{
    /// @return The switch a jump of @p mode makes.
    [[nodiscard]] constexpr session::SwitchMode switchModeOf(JumpMode mode) noexcept
    {
        return mode == JumpMode::NewWindow ? session::SwitchMode::Open : session::SwitchMode::Switch;
    }
} // namespace

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
                          std::optional<SidebarTarget> from,
                          JumpThen then)
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
    auto const switchMode = switchModeOf(mode);
    auto done = start(target, std::move(from), switchMode, std::move(then));
    _switching.request(
        window, *std::move(switchTarget), switchMode, session::NoLandingDeadline, std::move(done));
}

void SidebarJumps::createAndJump(vtworkspace::WorkspaceId window,
                                 SidebarTarget const& daemonTarget,
                                 std::optional<SidebarTarget> from)
{
    auto daemon = session::switchTargetOf(
        daemonTarget.daemon, session::AnyWorkspace {}, session::LandingFocus {}, daemonTarget.instance);
    if (!daemon)
        return;
    // Pending on the daemon's row: the workspace it lands on does not exist yet.
    auto const target = SidebarTarget { .daemon = daemonTarget.daemon,
                                        .instance = daemonTarget.instance,
                                        .workspace = 0,
                                        .tab = {},
                                        .pane = {} };
    auto done = start(target, std::move(from), session::SwitchMode::Switch, {});
    _switching.createWorkspace(
        window, *std::move(daemon), std::string {}, session::SwitchMode::Switch, std::move(done));
}

session::SwitchCompletion SidebarJumps::start(SidebarTarget const& target,
                                              std::optional<SidebarTarget> from,
                                              session::SwitchMode mode,
                                              JumpThen then)
{
    // Pending before the request: the switch of its mode it replaces is told it ended as this one starts,
    // and says nothing, as its serial is no longer the one under way; and one that ends at once finds this
    // one.
    auto const serial = _nextSerial++;
    slotOf(mode) = PendingJump { .serial = serial,
                                 .target = target,
                                 .from = std::move(from),
                                 .then = std::move(then),
                                 .cancel = JumpCancel::None };
    _announce(JumpEvent::Connecting, target, {});
    _changed();
    return [alive = std::weak_ptr { _alive },
            serial](std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome) {
        if (auto const self = alive.lock())
            (*self)->finish(serial, outcome);
    };
}

PendingJumps SidebarJumps::pending() const
{
    auto const targetOf = [this](session::SwitchMode mode) {
        auto const& jump = slotOf(mode);
        return jump ? std::optional { jump->target } : std::nullopt;
    };
    return PendingJumps { .here = targetOf(session::SwitchMode::Switch),
                          .newWindow = targetOf(session::SwitchMode::Open) };
}

void SidebarJumps::cancel(vtworkspace::WorkspaceId window, JumpMode mode)
{
    auto const switchMode = switchModeOf(mode);
    auto& jump = slotOf(switchMode);
    if (!jump)
        return;
    jump->cancel = JumpCancel::Requested;
    _switching.cancel(window, switchMode);
}

void SidebarJumps::finish(uint64_t serial,
                          std::expected<vthost::proto::SwitchOutcome, std::string> const& outcome)
{
    // A jump a newer one of its mode replaced says nothing: the newer one speaks for the window.
    auto const slot = std::ranges::find_if(_pending, [serial](std::optional<PendingJump> const& jump) {
        return jump && jump->serial == serial;
    });
    if (slot == _pending.end())
        return;
    auto const ended = *std::exchange(*slot, std::nullopt);
    auto const event = jumpEventOf(outcome, ended.cancel);
    if (event)
        _announce(*event, ended.target, outcome ? std::string {} : outcome.error());
    _changed();
    // Only once it landed: here, or in the window that shows the target already.
    if (event == JumpEvent::Landed && ended.then)
        ended.then(*outcome);
}

} // namespace contour::window
