// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <chrono>

#include <vthost/keeper/Messages.hpp>

namespace vthost
{

/// How often a daemon behind a keeper tells it, from its loop thread, that it is alive.
inline constexpr std::chrono::milliseconds KeeperHeartbeatInterval { 1'000 };

/// What a daemon tells its PTY keeper about keeping it (keeper::Serving): the keeper reads no configuration.
struct KeeperSettings
{
    /// Whether the keeper restarts a daemon that stops answering heartbeats (`daemon.keeper.restart_hung`).
    keeper::HungRestart hungRestart = keeper::HungRestart::On;
    /// How long the heartbeats may stop before the daemon counts as hung.
    std::chrono::milliseconds heartbeatTimeout { 30'000 };
};

} // namespace vthost
