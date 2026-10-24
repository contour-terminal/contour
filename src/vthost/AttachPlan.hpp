// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// What an attach intent means: which of the daemon's windows a connection follows, and whether attaching
/// creates something. A pure decision over the intent and a summary of the daemon, so every row of the
/// table is testable without a socket, a session or a terminal.

#include <cstddef>
#include <cstdint>
#include <vector>

#include <vthost/proto/Pdu.hpp>

namespace vthost
{

/// One daemon window, as the plan needs to see it.
struct WindowSummary
{
    uint64_t id = 0;  ///< The window's id.
    int tabCount = 0; ///< How many tabs it holds; zero for a window nothing has been created in yet.
};

/// What the daemon looks like to a connection that is attaching.
struct AttachContext
{
    /// How many other handshaken native `ConnectionRole::Attach` connections there are. A `Control`
    /// connection (status probes, a control API) never counts; an absent role is `Attach`.
    std::size_t otherClients = 0;
    /// The window a first session is created in when the daemon has none to show.
    uint64_t primaryWindow = 0;
    /// Every window, ascending by id.
    std::vector<WindowSummary> windows = {};
};

/// Which windows a connection follows.
enum class FollowScope : uint8_t
{
    Listed = 0, ///< The plan's `follow` plus the windows this connection creates.
    Every = 1,  ///< Every window, including those created later by other clients (the legacy behaviour).
};

/// What attaching creates.
enum class AttachCreation : uint8_t
{
    None = 0,              ///< Nothing.
    FirstTabInPrimary = 1, ///< The first tab (and session) of the primary window.
    NewWindow = 2,         ///< A new window of its own, which joins `follow` once it exists.
};

/// The decision: what to follow, and what to create.
struct AttachPlan
{
    FollowScope scope = FollowScope::Listed;
    std::vector<uint64_t> follow = {}; ///< The existing windows to follow, ascending.
    AttachCreation creation = AttachCreation::None;
    bool operator==(AttachPlan const&) const = default;
};

/// Decides what an attach intent means for the daemon as it is.
/// @param intent What the connection asked for.
/// @param context The daemon, as the connection finds it.
/// @return The plan.
[[nodiscard]] AttachPlan planAttach(proto::AttachIntent const& intent, AttachContext const& context);

} // namespace vthost
