// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What an attach intent means: which of the daemon's workspaces a connection follows, and whether attaching
/// creates something. A pure decision over the intent and a summary of the daemon, so every row of the
/// table is testable without a socket, a session or a terminal.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vthost/WorkspaceVisibility.hpp>
#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// One daemon workspace, as the plan needs to see it.
struct WorkspaceSummary
{
    uint64_t id = 0;  ///< The workspace's id.
    int tabCount = 0; ///< How many tabs it holds; zero for a workspace nothing has been created in yet.
    /// Whether a client shows it, or put it away. A launch brings back only the Left ones.
    WorkspaceVisibility visibility = WorkspaceVisibility::Left;
};

/// Whether the attaching connection's hello names the directory the session it creates starts in
/// (proto::ClientHello::workingDirectory).
enum class WorkingDirectoryStated : uint8_t
{
    No = 0,  ///< It names none: whatever the attach creates starts where its profile says.
    Yes = 1, ///< It names one, which a launch that would create nothing opens a tab in.
};

/// What the daemon looks like to a connection that is attaching.
struct AttachContext
{
    /// How many other handshaken native `ConnectionRole::Attach` connections there are. A `Control`
    /// connection (status probes, a control API) never counts; an absent role is `Attach`. A launch does not
    /// consult it: what other clients show is told by each workspace's visibility.
    std::size_t otherClients = 0;
    /// The workspace a first session is created in when the daemon has none to show.
    uint64_t primaryWorkspace = 0;
    /// Every workspace, ascending by id.
    std::vector<WorkspaceSummary> workspaces = {};
    /// Whether the hello names a working directory; a launch that would create nothing then opens a tab in
    /// it.
    WorkingDirectoryStated workingDirectory = WorkingDirectoryStated::No;
    /// Which run of the daemon this is (SessionHost::instance()); 0 when none was stated. An intent whose ids
    /// are of another run names none of this one's workspaces (proto::AttachIntent::daemonInstance).
    uint64_t instance = 0;
};

/// What attaching creates.
enum class AttachCreation : uint8_t
{
    None = 0,              ///< Nothing.
    FirstTabInPrimary = 1, ///< The first tab (and session) of the primary workspace.
    NewWorkspace = 2,      ///< A new workspace of its own, which joins `follow` once it exists.
    /// A tab of the first workspace in `follow`, beside the sessions it brings back: what a launch that
    /// states a working directory creates when the daemon already has workspaces to show, so the terminal
    /// asked for in that directory is not dropped.
    TabInFirstFollowed = 3,
};

/// The decision: what to follow, and what to create. The connection follows `follow` and the workspaces it
/// gains later (its own creations, the unclaimed ones for the primary viewer, FollowWorkspace).
struct AttachPlan
{
    std::vector<uint64_t> follow = {}; ///< The existing workspaces to follow, ascending.
    AttachCreation creation = AttachCreation::None;
    bool operator==(AttachPlan const&) const = default;
};

/// Decides what an attach intent means for the daemon as it is.
/// @param intent What the connection asked for.
/// @param context The daemon, as the connection finds it.
/// @return The plan.
[[nodiscard]] AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context);

} // namespace vthost
