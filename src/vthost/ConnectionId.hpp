// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// How one accepted connection identifies itself in the daemon's diagnostics.

#include <algorithm>
#include <array>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

namespace vthost
{

/// Identifies one accepted connection: which endpoint accepted it, its per-endpoint sequence
/// number, and the peer where one is meaningful.
///
/// Exists so the accept line, the handshake line, ten thousand trace lines and the disconnect
/// line all carry the same `native#3` and can be read as one story. The daemon serves up to
/// five listeners at once, so a line that does not say WHICH is barely a diagnostic.
struct ConnectionId
{
    /// The listener that accepted: "control", "native", "imsg", "tmux-compat", "native-tcp".
    ///
    /// A std::string and NOT a string_view into the acceptor's name, deliberately: in
    /// runDaemon the acceptors are declared AFTER the event loop and so are destroyed BEFORE
    /// the loop reaps its in-flight connection flows. A view would dangle exactly at shutdown,
    /// which is when the logs matter most.
    std::string endpoint {};

    /// This connection's number on that endpoint, counting from 1.
    std::uint64_t index = 0;

    /// The peer address, where the transport knows one. Empty for AF_UNIX, whose peers have
    /// no address — the filesystem permissions are the identity there.
    std::string peer {};

    bool operator==(ConnectionId const&) const = default;
};

/// The endpoints whose listener is a local (Unix-socket) one: a connection accepted there came from this
/// machine, and its socket's permissions are its identity.
inline constexpr std::array<std::string_view, 2> LocalEndpoints { "control", "native" };

/// @param id A connection.
/// @return Whether it was accepted on a local (Unix-socket) listener, as opposed to a network one.
[[nodiscard]] inline bool isLocal(ConnectionId const& id) noexcept
{
    return std::ranges::contains(LocalEndpoints, std::string_view { id.endpoint });
}

} // namespace vthost

template <>
struct std::formatter<vthost::ConnectionId>
{
    static constexpr auto parse(auto& ctx) { return ctx.begin(); }

    // Straight to the output iterator: this prefixes essentially every daemon log line,
    // including the gated per-PDU trace lines, so a throwaway std::string per format would be
    // one extra allocation per traced PDU.
    auto format(vthost::ConnectionId const& value, auto& ctx) const
    {
        if (value.peer.empty())
            return std::format_to(ctx.out(), "{}#{}", value.endpoint, value.index);
        return std::format_to(ctx.out(), "{}#{}@{}", value.endpoint, value.index, value.peer);
    }
};
