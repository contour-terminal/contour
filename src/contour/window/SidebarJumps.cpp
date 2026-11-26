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
                           std::function<void()> changed,
                           Landed landed,
                           Unreached unreached):
    _switching { switching },
    _announce { std::move(announce) },
    _changed { std::move(changed) },
    _landed { std::move(landed) },
    _unreached { std::move(unreached) },
    _alive { std::make_shared<SidebarJumps*>(this) }
{
}

void SidebarJumps::jumpTo(vtworkspace::WorkspaceId window,
                          SidebarTarget const& target,
                          JumpMode mode,
                          std::expected<std::array<uint8_t, 16>, JumpRefusal> const& controlId,
                          std::optional<SidebarTarget> from,
                          HistoryCause cause,
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
    auto done = start(target, std::move(from), switchMode, cause, std::move(then));
    _switching.request(window, *std::move(switchTarget), switchMode, session::UserSwitch, std::move(done));
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
    auto done = start(target, std::move(from), session::SwitchMode::Switch, HistoryCause::Jump, {});
    _switching.createWorkspace(
        window, *std::move(daemon), std::string {}, session::SwitchMode::Switch, std::move(done));
}

session::SwitchCompletion SidebarJumps::start(SidebarTarget const& target,
                                              std::optional<SidebarTarget> from,
                                              session::SwitchMode mode,
                                              HistoryCause cause,
                                              JumpThen then)
{
    // Pending before the request: the switch of its mode it replaces is told it ended as this one starts,
    // and says nothing, as its serial is no longer the one under way; and one that ends at once finds this
    // one.
    auto const serial = _nextSerial++;
    slotOf(mode) = PendingJump { .serial = serial,
                                 .target = target,
                                 .from = std::move(from),
                                 .cause = cause,
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

void SidebarJumps::release()
{
    // Out of its slot: the switching service tells it ended once the newer request replaced it, and that
    // completion finds no jump to speak for.
    if (std::exchange(slotOf(session::SwitchMode::Switch), std::nullopt))
        _changed();
}

void SidebarJumps::supersede(vtworkspace::WorkspaceId window)
{
    // Out of its slot first: the completion the cancel tells, at once or later, then finds no jump to speak
    // for.
    if (!std::exchange(slotOf(session::SwitchMode::Switch), std::nullopt))
        return;
    _switching.cancel(window, session::SwitchMode::Switch);
    _changed();
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
    auto const landedHere = slot == _pending.begin() + std::to_underlying(session::SwitchMode::Switch);
    auto const ended = *std::exchange(*slot, std::nullopt);
    auto const event = jumpEventOf(outcome, ended.cancel);
    if (event)
        _announce(*event, ended.target, outcome ? std::string {} : outcome.error());
    _changed();
    // Recorded only where this window moved: not where another window that shows the target was raised, nor
    // a window opened beside it. Before the then, which may move it further.
    if (landedHere && outcome && *outcome == vthost::proto::SwitchOutcome::Switched && _landed)
        _landed(ended.from, ended.target, ended.cause);
    // A jump back that got nowhere leaves its place to go back to.
    if (ended.cause == HistoryCause::Back && (event == JumpEvent::Failed || event == JumpEvent::Cancelled)
        && _unreached)
        _unreached(ended.target);
    // Only once it landed: here, or in the window that shows the target already.
    if (event == JumpEvent::Landed && ended.then)
        ended.then(*outcome);
}

} // namespace contour::window
