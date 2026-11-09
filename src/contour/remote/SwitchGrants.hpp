// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What the user said should stand for the switches programs ask for (GuardedRole::SwitchWindow).

#include <contour/remote/SwitchPlan.hpp>
#include <contour/session/SwitchPermission.hpp>

#include <map>
#include <optional>
#include <string>
#include <tuple>

#include <vthost/proto/Pdu.hpp>

namespace contour::remote
{

/// Whether the prompt for a switch to @p target may offer to remember its answer: never for an ad-hoc host.
/// @param target Where the switch goes.
/// @return Offered, or Withheld for an ad-hoc host (session::isAdHocHost()).
[[nodiscard]] inline session::SwitchRemembering rememberingFor(session::SwitchTarget const& target) noexcept
{
    return session::isAdHocHost(target) ? session::SwitchRemembering::Withheld
                                        : session::SwitchRemembering::Offered;
}

/// The process-lifetime memory of "don't ask again" answers to GuardedRole::SwitchWindow, keyed by
/// (source daemon, target daemon) and never persisted (spec §Safety). The session's own memory would go with
/// the session the switch tears down, so the application keeps this one, for every window. A target's
/// workspace is not part of the key: what the user allows is pointing the window from one daemon at
/// another. Same-daemon switches are keyed (daemon, daemon). GUI thread only.
class SwitchGrants
{
  public:
    /// @param source The daemon the window is on.
    /// @param target Where the switch goes.
    /// @return The answer that stands for switches from @p source to @p target's daemon; none when the user
    ///         is to be asked -- always for an ad-hoc host.
    [[nodiscard]] std::optional<session::PermissionDecision> remembered(
        DaemonAddress const& source, session::SwitchTarget const& target) const
    {
        if (rememberingFor(target) == session::SwitchRemembering::Withheld)
            return std::nullopt;
        auto const found = _answers.find(keyOf(source, target));
        if (found == _answers.end())
            return std::nullopt;
        return found->second;
    }

    /// Has @p decision stand for switches from @p source to @p target's daemon from now on. Ignored for an
    /// ad-hoc host, which no answer may stand for.
    /// @param source The daemon the window is on.
    /// @param target Where the switch goes.
    /// @param decision The answer.
    void remember(DaemonAddress const& source,
                  session::SwitchTarget const& target,
                  session::PermissionDecision decision)
    {
        if (rememberingFor(target) == session::SwitchRemembering::Withheld)
            return;
        _answers.insert_or_assign(keyOf(source, target), decision);
    }

  private:
    /// (source kind, source name, target kind, target name).
    using Key = std::
        tuple<vthost::proto::SwitchDaemonKind, std::string, vthost::proto::SwitchDaemonKind, std::string>;

    /// @return The key of a switch from @p source to @p target: a target on the window's own daemon (Same)
    ///         is keyed as that daemon.
    [[nodiscard]] static Key keyOf(DaemonAddress const& source, session::SwitchTarget const& target)
    {
        if (target.kind == vthost::proto::SwitchDaemonKind::Same)
            return Key { source.kind, source.name, source.kind, source.name };
        return Key { source.kind, source.name, target.kind, target.daemon };
    }

    std::map<Key, session::PermissionDecision> _answers;
};

} // namespace contour::remote
