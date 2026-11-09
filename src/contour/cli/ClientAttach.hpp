// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What `contour client` attaches with, whichever daemon it reaches -- the local one, a saved host, a TLS
/// endpoint. Pure, so that each rule is tested without a daemon.

#include <contour/cli/ContourApp.hpp>

#include <array>
#include <cstdint>
#include <optional>

#include <vthost/proto/Pdu.hpp>

namespace contour::cli
{

/// The `contour client` options that are silently meaningless beside `--host`, whose entry says where and how
/// to connect; given together, the client refuses. `--workspace` is not among them: it names what to show
/// there, as it does locally.
inline constexpr auto HostExclusions = std::array {
    OptionPresence { .name = "connect-tcp", .givenAs = GivenAs::Text },
    OptionPresence { .name = "socket", .givenAs = GivenAs::Text },
    OptionPresence { .name = "tmux", .givenAs = GivenAs::Flag },
    OptionPresence { .name = "token", .givenAs = GivenAs::Text },
    OptionPresence { .name = "token-file", .givenAs = GivenAs::Text },
    OptionPresence { .name = "tls-ca", .givenAs = GivenAs::Text },
};

/// What a launch's first connection follows, on any daemon: exactly the workspace `--workspace` names, else
/// the launch rule -- the workspaces left by the windows that closed, each in a window of its own, or one new
/// workspace when none is left. Never every workspace: what another client shows, or a window switched away
/// from, is not pulled in.
/// @param workspace `--workspace`: the daemon workspace to show; none for a plain launch.
/// @return The attach intent.
[[nodiscard]] inline vthost::proto::AttachIntent launchIntent(std::optional<uint64_t> workspace)
{
    if (workspace)
        return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Workspaces,
                                             .workspaces = { *workspace } };
    return vthost::proto::AttachIntent { .kind = vthost::proto::AttachIntentKind::Launch, .workspaces = {} };
}

} // namespace contour::cli
