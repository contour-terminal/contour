// SPDX-License-Identifier: Apache-2.0
#pragma once

/// @file
/// `DaemonUpgradeHooks` -- the daemon's side of a live upgrade that is its clients and its workspace: it
/// tells every connection the daemon is upgrading, writes the workspace snapshot and closes every connection.

#include <algorithm>
#include <functional>
#include <string_view>
#include <vector>

#include <vthost/ConnectionRegistry.hpp>
#include <vthost/DaemonControl.hpp>
#include <vthost/handover/LiveUpgrade.hpp>

namespace vthost
{

/// The UpgradeHooks of a running daemon. Loop thread only.
///
/// Its connections come in two kinds. Those that speak the native protocol end in an orderly way of their own
/// -- they are UpgradeObservers, told to announce and then to close, so that DaemonUpgrading is flushed
/// before the socket goes. Every other connection (tmux control mode, imsg, the compat endpoint) is a socket
/// of the ConnectionRegistry, closed outright.
class DaemonUpgradeHooks final: public handover::UpgradeHooks
{
  public:
    /// @param connections The daemon's non-native connections (not owned; outlives this).
    /// @param writeSnapshot Writes the workspace snapshot now and blocks until it is on disk, without sealing
    ///        the writer (WorkspacePersistence::writeNow); empty while the daemon keeps no workspace.
    DaemonUpgradeHooks(ConnectionRegistry const& connections, std::function<void()> writeSnapshot):
        _connections(connections), _writeSnapshot(std::move(writeSnapshot))
    {
    }

    /// Has @p observer announced to and closed with the upgrade, until unsubscribe().
    /// @param observer Not owned; must stay alive until unsubscribed.
    void subscribe(UpgradeObserver& observer) { _observers.push_back(&observer); }

    /// Stops telling @p observer; harmless when it is not subscribed.
    /// @param observer A subscriber, or not.
    void unsubscribe(UpgradeObserver& observer) { std::erase(_observers, &observer); }

    /// Makes @p onStarted the requester's answer: called by the next announceUpgrade() ahead of every
    /// client's announcement, once.
    /// @param onStarted What answers the requester; may be empty.
    void setRequester(UpgradeStarted onStarted) { _requester = std::move(onStarted); }

    /// Forgets the requester.
    void clearRequester() { _requester = nullptr; }

    /// Answers the requester, then tells every observer.
    /// @param toRelease The release the daemon becomes.
    void announceUpgrade(std::string_view toRelease) override;

    /// Writes the workspace snapshot through the injected writer; seals nothing.
    void writeWorkspaceSnapshot() override;

    /// Ends every observer's connection in an orderly way, and closes every other connection.
    void closeClients() override;

    /// Closes every observer's connection at once, and every other connection.
    void abortClients() override;

  private:
    ConnectionRegistry const& _connections;
    std::function<void()> _writeSnapshot;
    UpgradeStarted _requester;
    std::vector<UpgradeObserver*> _observers;
};

} // namespace vthost
