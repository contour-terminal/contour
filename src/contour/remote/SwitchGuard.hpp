// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The guard in front of the switches programs ask for through the control API (`workspace.switch`).

#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/session/SwitchPermission.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <string>

#include <vthost/SwitchClient.hpp>
#include <vthost/proto/Pdu.hpp>
#include <vtworkspace/Primitives.hpp>

namespace contour::remote
{

/// A switch a program asked a window for (proto::SwitchRequest), as the guard sees it.
struct GuardedSwitch
{
    vtworkspace::WorkspaceId window; ///< The window to switch.
    DaemonAddress source;            ///< The daemon it is on.
    session::SwitchTarget target;    ///< Where to, as the program named it.
    /// How long the daemon waits for the reply, from the request's arrival (proto::SwitchRequest).
    std::chrono::milliseconds answerWithin { vthost::SwitchAnswerTimeout };
};

/// Told once, on the GUI thread, what came of a guarded switch, and a sentence for the user (empty on
/// success): what the SwitchReply carries.
using GuardedSwitchDone = std::function<void(vthost::proto::SwitchOutcome outcome, std::string detail)>;

/// Takes a switch question down, unanswered, if it is still up.
using SwitchWithdraw = std::function<void()>;

/// Asks the user in a window about a switch (TerminalSession::askToSwitchWindow()).
/// @return How to take the question down again once asked; why it was not, when the window has no session to
///         ask in.
using SwitchAsker = std::function<std::expected<SwitchWithdraw, std::string>(vtworkspace::WorkspaceId window,
                                                                             session::SwitchAsk ask)>;

/// The longest a switch question is put up for, whatever the daemon says it waits: a daemon on another host
/// states its wait, and a question that stays up for days is a question nobody is asked any more.
inline constexpr auto MaxSwitchAnswerWindow = std::chrono::milliseconds { std::chrono::minutes { 5 } };

/// @param answerWithinMs How long the daemon says it waits for the reply (proto::SwitchRequest), 0 for a
///        daemon that states nothing.
/// @return How long it waits: what it states, at most MaxSwitchAnswerWindow, else what a daemon that states
///         nothing waits (vthost::SwitchAnswerTimeout).
[[nodiscard]] constexpr std::chrono::milliseconds switchAnswerWindowOf(uint32_t answerWithinMs) noexcept
{
    if (answerWithinMs == 0)
        return std::chrono::milliseconds { vthost::SwitchAnswerTimeout };
    return std::min(std::chrono::milliseconds { answerWithinMs }, MaxSwitchAnswerWindow);
}

/// The detail of a switch the user refused.
inline constexpr std::string_view SwitchRefusedDetail = "the user declined the switch";

/// The detail of a switch whose question went unanswered until the program stopped waiting.
inline constexpr std::string_view SwitchUnansweredDetail = "the user did not answer in time";

/// The detail of a switch the daemon would stop waiting for before an allowed one could land.
inline constexpr std::string_view SwitchTooLateDetail =
    "too little time is left to switch before the daemon stops waiting";

/// What the reply's trip back to the daemon is allowed, beside the switch's own landing.
inline constexpr auto SwitchReplyMargin = std::chrono::milliseconds { std::chrono::seconds { 2 } };

/// How long a switch question stays up: the daemon's wait for the answer less what an allowed switch may take
/// to land or give up (@p landing) and SwitchReplyMargin. An Allow later than that would switch a window
/// after `contour switch` gave up, so the question is taken down then and the switch refused.
/// @param answerWithin How long the daemon waits for the reply (GuardedSwitch::answerWithin).
/// @param landing How long an allowed switch waits for its target to show (SwitchDeadline::timeout).
/// @return How long to ask; zero or less when an Allow could no longer land in time, even at once.
[[nodiscard]] constexpr std::chrono::milliseconds switchQuestionExpiry(
    std::chrono::milliseconds answerWithin, std::chrono::milliseconds landing) noexcept
{
    return answerWithin - landing - SwitchReplyMargin;
}
static_assert(switchQuestionExpiry(std::chrono::milliseconds { vthost::SwitchAnswerTimeout },
                                   DefaultSwitchTimeout)
              > std::chrono::milliseconds::zero());

/// @param arrival When the request arrived.
/// @param answerWithin How long the daemon waits for the reply (GuardedSwitch::answerWithin).
/// @return When an allowed switch must have landed by: before the daemon gives up waiting, with the reply's
///         trip back (SwitchReplyMargin) to spare.
[[nodiscard]] constexpr core::platform::SteadyTimePoint switchLandingTime(
    core::platform::SteadyTimePoint arrival, std::chrono::milliseconds answerWithin) noexcept
{
    return arrival + answerWithin - SwitchReplyMargin;
}

/// Runs @p request behind GuardedRole::SwitchWindow. A request with no time left for a switch to land
/// (switchQuestionExpiry()) is Refused at once. An answer @p grants keeps for its pair of daemons stands
/// without asking: Deny answers Refused, Allow switches. Otherwise the user is asked in the window, and an
/// answer they asked to stand is kept in @p grants (never for an ad-hoc host). An allowed switch is
/// @p switching's SwitchMode::Switch request, whose outcome @p done is told; a refused one is Refused, and
/// so is one left unanswered for switchQuestionExpiry(), whose question is taken down then -- an answer
/// after that changes nothing. An allowed switch must land by switchLandingTime() from the request's arrival,
/// whatever else it waits for first (the target daemon's workspaces are listed before anything is followed):
/// @p switching refuses to start it later, and gives up on it then. The switches the user drives from the GUI
/// itself do not come here.
/// @param request The switch.
/// @param grants The answers that stand. Borrowed; it must outlive the question.
/// @param ask How the user is asked.
/// @param switching What performs the switch. Borrowed; it must outlive the question.
/// @param deadline What expires the question, what the allowed switch waits to land at most -- the one
///        @p switching is built with -- and the clock the request's arrival is read from.
/// @param done Told what came of it, once.
void runGuardedSwitch(GuardedSwitch const& request,
                      SwitchGrants& grants,
                      SwitchAsker const& ask,
                      session::WindowSwitching& switching,
                      SwitchDeadline const& deadline,
                      GuardedSwitchDone done);

} // namespace contour::remote
