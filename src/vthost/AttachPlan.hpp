// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What an attach intent means: which of the daemon's workspaces a connection selects, and whether attaching
/// creates something. A pure decision over the intent and a summary of the daemon, so every row of the
/// table is testable without a socket, a session or a terminal.

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// One daemon workspace, as the plan needs to see it.
struct WorkspaceSummary
{
    uint64_t id = 0;  ///< The workspace's id.
    int tabCount = 0; ///< How many tabs it holds; zero for a workspace nothing has been created in yet.
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
    /// The workspace a first session is created in when the daemon has none to show.
    uint64_t primaryWorkspace = 0;
    /// Every workspace, ascending by id: the order a window lists them in.
    std::vector<WorkspaceSummary> workspaces = {};
    /// Whether the hello names a working directory; a launch that would create nothing then opens a tab in
    /// it.
    WorkingDirectoryStated workingDirectory = WorkingDirectoryStated::No;
    /// Which run of the daemon this is (SessionHost::instance()); 0 when none was stated. An intent whose ids
    /// are of another run names none of this one's workspaces (proto::AttachIntent::daemonInstance).
    uint64_t instance = 0;
    /// The workspace some connection selected last (SessionHost::lastSelectedWorkspace()), if any.
    std::optional<uint64_t> lastSelected = std::nullopt;
};

/// What attaching creates.
enum class AttachCreation : uint8_t
{
    None = 0,              ///< Nothing.
    FirstTabInPrimary = 1, ///< The first tab (and session) of the primary workspace.
    NewWorkspace = 2,      ///< A new workspace of its own, which the connection selects once it exists.
    /// A tab of the selected workspace, beside the sessions it holds: what a launch that states a working
    /// directory creates when the daemon already has workspaces to show, so the terminal asked for in that
    /// directory is not dropped.
    TabInSelected = 3,
};

/// The decision: what to select, and what to create.
struct AttachPlan
{
    /// The existing workspace the connection selects; none when it selects the one it creates
    /// (AttachCreation::NewWorkspace).
    std::optional<uint64_t> select = std::nullopt;
    AttachCreation creation = AttachCreation::None;
    bool operator==(AttachPlan const&) const = default;
};

/// Decides what an attach intent means for the daemon as it is.
/// @param intent What the connection asked for.
/// @param context The daemon, as the connection finds it.
/// @return The plan.
[[nodiscard]] AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context);

} // namespace vthost
