// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// Which workspace each attached window selects, and which was focused last: what the loop publishes and an
/// extension's lane reads -- where a dashboard opens, and where an agent's pane opens when its dashboard is
/// gone.

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include <vthost/ConnectionId.hpp>
#include <vtworkspace/Primitives.hpp>

namespace vthost
{

/// Whether a window's client can open a web link on its own machine (proto::Capability::UrlOpen).
enum class LinkOpening : uint8_t
{
    Unable = 0, ///< It cannot: it did not offer the capability.
    Able,       ///< It opens what OpenUrl names, with its own launcher.
};

/// Which workspace each attached window selects, read from any thread.
class IConnectionWorkspaces
{
  public:
    IConnectionWorkspaces() = default;
    IConnectionWorkspaces(IConnectionWorkspaces const&) = delete;
    IConnectionWorkspaces(IConnectionWorkspaces&&) = delete;
    IConnectionWorkspaces& operator=(IConnectionWorkspaces const&) = delete;
    IConnectionWorkspaces& operator=(IConnectionWorkspaces&&) = delete;
    virtual ~IConnectionWorkspaces() = default;

    /// @param connection An attached connection.
    /// @return The workspace its window selects; nullopt for none, or a connection not attached.
    [[nodiscard]] virtual std::optional<vtworkspace::WorkspaceId> selectedBy(
        ConnectionId const& connection) const = 0;

    /// @return The attached connection whose window was focused last, of those that select a workspace;
    ///         nullopt when none does.
    [[nodiscard]] virtual std::optional<ConnectionId> mostRecentlyFocused() const = 0;

    /// @param connection An attached connection.
    /// @return Whether its client can open a web link; Unable for a connection that selects no workspace.
    [[nodiscard]] virtual LinkOpening linkOpeningOf(ConnectionId const& connection) const = 0;
};

/// The production IConnectionWorkspaces: a mutex-guarded mirror the loop publishes into.
class ConnectionWorkspaces final: public IConnectionWorkspaces
{
  public:
    /// Records which workspace @p connection's window selects. Loop thread.
    /// @param connection An attached connection.
    /// @param selected Its selection; nullopt once it selects none, or detached.
    /// @param links Whether its client can open a web link.
    void publish(ConnectionId const& connection,
                 std::optional<vtworkspace::WorkspaceId> selected,
                 LinkOpening links);

    /// Records that @p connection's window was focused now. Loop thread.
    void noteFocused(ConnectionId const& connection);

    [[nodiscard]] std::optional<vtworkspace::WorkspaceId> selectedBy(
        ConnectionId const& connection) const override;
    [[nodiscard]] std::optional<ConnectionId> mostRecentlyFocused() const override;
    [[nodiscard]] LinkOpening linkOpeningOf(ConnectionId const& connection) const override;

  private:
    /// One attached connection that selects a workspace.
    struct Selection
    {
        ConnectionId connection;
        vtworkspace::WorkspaceId workspace;
        LinkOpening links = LinkOpening::Unable;
    };

    mutable std::mutex _mutex; ///< Guards everything below.
    std::vector<Selection> _selections;
    std::vector<std::pair<ConnectionId, uint64_t>> _focus; ///< Focus stamps, kept across a deselection.
    uint64_t _focusClock = 0;
};

} // namespace vthost
