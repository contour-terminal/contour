// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What a session is handed when a program asks to switch its window (GuardedRole::SwitchWindow).

#include <contour/session/Permission.hpp>
#include <contour/session/WindowSwitching.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace contour::session
{

/// Whether a switch's prompt offers to remember its answer.
enum class SwitchRemembering : uint8_t
{
    Offered = 0, ///< For the rest of this process, for the switches between the same two daemons.
    Withheld,    ///< Never: an ad-hoc host, which no answer may stand for (@see isAdHocHost()).
};

/// @param target Where a switch goes.
/// @return Whether it names a host by URL (`ssh://…`) rather than a saved host: an allowed switch must not
///         become a standing licence to point the window at any host, which would run ssh with the user's
///         credentials.
[[nodiscard]] inline bool isAdHocHost(SwitchTarget const& target) noexcept
{
    return target.kind == vthost::proto::SwitchDaemonKind::Host && target.daemon.contains("://");
}

/// Told the user's answer to a switch's prompt, once, on the GUI thread. A memory other than Once asks that
/// the answer stand: kept by whoever asked, not by the session, which a switch tears down.
using SwitchAnswered = std::function<void(PermissionDecision decision, PermissionMemory memory)>;

/// A program's request to switch the window, as the session's prompt shows it.
struct SwitchAsk
{
    SwitchTarget target;      ///< Where to, as the program named it.
    std::string sourceDaemon; ///< The daemon the window is on, as the user knows it; empty for one unnamed.
    SwitchRemembering remembering = SwitchRemembering::Offered; ///< Whether the box is offered.
    SwitchAnswered answered; ///< Told the answer; Deny for this once when the question goes unanswered.
};

} // namespace contour::session
