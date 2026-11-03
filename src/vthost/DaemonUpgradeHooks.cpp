// SPDX-License-Identifier: Apache-2.0
#include <vthost/DaemonUpgradeHooks.hpp>

#include <utility>

namespace vthost
{

void DaemonUpgradeHooks::announceUpgrade(std::string_view toRelease)
{
    // The requester first: its answer must reach it before DaemonUpgrading and before its connection goes.
    if (auto const answer = std::exchange(_requester, nullptr))
        answer();
    // A copy: an observer's reaction never unsubscribes it inline, but this should not depend on that.
    for (auto* const observer: std::vector<UpgradeObserver*>(_observers))
        observer->announceUpgrade(toRelease);
}

void DaemonUpgradeHooks::writeWorkspaceSnapshot()
{
    if (_writeSnapshot)
        _writeSnapshot();
}

void DaemonUpgradeHooks::closeClients()
{
    for (auto* const observer: std::vector<UpgradeObserver*>(_observers))
        observer->closeForUpgrade();
    _connections.closeAll();
}

void DaemonUpgradeHooks::abortClients()
{
    for (auto* const observer: std::vector<UpgradeObserver*>(_observers))
        observer->abortForUpgrade();
    _connections.closeAll();
}

} // namespace vthost
