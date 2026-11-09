// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// The guard in front of the switches programs ask for through the control API (`workspace.switch`).

#include <contour/remote/DaemonLinkHost.hpp>
#include <contour/remote/SwitchGrants.hpp>
#include <contour/remote/SwitchPlan.hpp>
#include <contour/session/SwitchPermission.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <chrono>
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

/// The detail of a switch the user refused.
inline constexpr std::string_view SwitchRefusedDetail = "the user declined the switch";

/// The detail of a switch whose question went unanswered until the program stopped waiting.
inline constexpr std::string_view SwitchUnansweredDetail = "the user did not answer in time";

/// How long a switch question stays up: a little less than the daemon waits for the answer
/// (vthost::SwitchAnswerTimeout), so that an Allow never switches a window after `contour switch` gave up.
inline constexpr auto SwitchQuestionExpiry = std::chrono::milliseconds { vthost::SwitchAnswerTimeout }
                                             - std::chrono::milliseconds { std::chrono::seconds { 2 } };
static_assert(SwitchQuestionExpiry > std::chrono::milliseconds::zero());

/// Runs @p request behind GuardedRole::SwitchWindow. An answer @p grants keeps for its pair of daemons
/// stands without asking: Deny answers Refused, Allow switches. Otherwise the user is asked in the window,
/// and an answer they asked to stand is kept in @p grants (never for an ad-hoc host). An allowed switch is
/// @p switching's SwitchMode::Switch request, whose outcome @p done is told; a refused one is Refused, and
/// so is one left unanswered for SwitchQuestionExpiry, whose question is taken down then. The switches the
/// user drives from the GUI itself do not come here.
/// @param request The switch.
/// @param grants The answers that stand. Borrowed; it must outlive the question.
/// @param ask How the user is asked.
/// @param switching What performs the switch. Borrowed; it must outlive the question.
/// @param timer What expires the question.
/// @param done Told what came of it, once.
void runGuardedSwitch(GuardedSwitch const& request,
                      SwitchGrants& grants,
                      SwitchAsker const& ask,
                      session::WindowSwitching& switching,
                      SwitchTimer const& timer,
                      GuardedSwitchDone done);

} // namespace contour::remote
