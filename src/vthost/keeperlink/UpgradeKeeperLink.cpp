// SPDX-License-Identifier: Apache-2.0
#include <vthost/keeperlink/UpgradeKeeperLink.hpp>

#include <vthost/Logging.hpp>
#include <vthost/handover/Readiness.hpp>
#include <vthost/keeperlink/KeeperLink.hpp>

namespace vthost::keeperlink
{

std::expected<void, handover::UpgradeRefusal> UpgradeKeeperLink::admits(
    handover::ReadinessReport const& target) const
{
    return handover::checkServesKeeper(target);
}

std::expected<void, handover::UpgradeRefusal> UpgradeKeeperLink::announceExec(
    std::filesystem::path const& executable)
{
    return _link.announceExec(executable).transform_error([](ChannelError error) {
        errorLog()("live upgrade: the keeper did not take what the daemon queued for it: {}",
                   error == ChannelError::TimedOut ? "it did not take note in time" : "it is gone");
        return handover::UpgradeRefusal::KeeperUnreachable;
    });
}

void UpgradeKeeperLink::abortExec()
{
    _link.abortExec();
}

} // namespace vthost::keeperlink
