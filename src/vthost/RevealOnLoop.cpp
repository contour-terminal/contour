// SPDX-License-Identifier: Apache-2.0
#include <vthost/RevealOnLoop.hpp>

#include <algorithm>
#include <array>
#include <cstdint>

#include <vthost/ServedSessions.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

namespace
{
    /// What a reveal does once the window that was asked to switch answered.
    enum class SwitchedReveal : uint8_t
    {
        Nothing = 0,  ///< The window did not go there: the user was asked, refused, or it failed.
        ToOrigin,     ///< The asked window shows the tab's workspace now: it is told.
        ToShowingNow, ///< Another window already showed it and was focused: that one is told.
    };

    /// One row of what a reveal does, by the asked window's answer.
    struct SwitchedRevealRow
    {
        proto::SwitchOutcome outcome;
        SwitchedReveal reveal;
    };

    inline constexpr auto SwitchedReveals = std::array {
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Switched, .reveal = SwitchedReveal::ToOrigin },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::FocusedExisting,
                            .reveal = SwitchedReveal::ToShowingNow },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::PickerShown, .reveal = SwitchedReveal::Nothing },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Refused, .reveal = SwitchedReveal::Nothing },
        SwitchedRevealRow { .outcome = proto::SwitchOutcome::Failed, .reveal = SwitchedReveal::Nothing },
    };

    /// @return What a reveal does once the asked window answered @p outcome.
    [[nodiscard]] constexpr SwitchedReveal switchedRevealOf(proto::SwitchOutcome outcome) noexcept
    {
        auto const row = std::ranges::find(SwitchedReveals, outcome, &SwitchedRevealRow::outcome);
        return row != SwitchedReveals.end() ? row->reveal : SwitchedReveal::Nothing;
    }
} // namespace

void revealSession(ServedTabHost& host,
                   vtworkspace::SessionId session,
                   ConnectionId const& connection,
                   vtworkspace::WorkspaceId origin)
{
    auto const where = host.workspaceOf(session);
    if (!where)
        return;
    if (*where == origin)
    {
        host.activate(session);
        host.revealTo(connection, session);
        return;
    }
    // In another workspace: the origin's window is asked to go there first, as the workspace it leaves.
    host.askToSwitch(connection,
                     origin,
                     proto::SwitchTargetWire { .kind = proto::SwitchDaemonKind::Same,
                                               .daemon = {},
                                               .workspace = host.workspaceRef(*where) },
                     [&host, session, connection, target = *where](proto::SwitchReply const& reply) {
                         auto const reveal = switchedRevealOf(reply.outcome);
                         if (reveal == SwitchedReveal::Nothing || host.workspaceOf(session) != target)
                             return;
                         host.activate(session);
                         switch (reveal)
                         {
                             case SwitchedReveal::ToOrigin: host.revealTo(connection, session); break;
                             case SwitchedReveal::ToShowingNow:
                                 if (auto const shows = host.showing(target); !shows.empty())
                                     host.revealTo(shows.front(), session);
                                 break;
                             case SwitchedReveal::Nothing: break;
                         }
                     });
}

} // namespace vthost
