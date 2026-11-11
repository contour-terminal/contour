// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <expected>
#include <filesystem>

#include <vthost/handover/LiveUpgrade.hpp>

namespace vthost::keeperlink
{

class KeeperLink;

/// The PTY keeper behind a daemon's link, as the daemon's live upgrade sees it: it admits only a target that
/// can run behind a keeper, and hears which image comes next before the exec, with everything queued for it.
class UpgradeKeeperLink final: public handover::UpgradeKeeper
{
  public:
    /// @param link The daemon's link to its keeper (not owned; outlives this).
    explicit UpgradeKeeperLink(KeeperLink& link) noexcept: _link { link } {}

    /// @return Nothing when @p target can run behind a keeper (handover::checkServesKeeper());
    ///         TargetCannotServeKeeper otherwise.
    [[nodiscard]] std::expected<void, handover::UpgradeRefusal> admits(
        handover::ReadinessReport const& target) const override;

    /// Posts Exec, sends everything queued and waits for the keeper's note (KeeperLink::announceExec()).
    /// @return Nothing once it went; KeeperUnreachable otherwise, logged.
    [[nodiscard]] std::expected<void, handover::UpgradeRefusal> announceExec(
        std::filesystem::path const& executable) override;

    /// Posts ExecAborted (KeeperLink::abortExec()).
    void abortExec() override;

  private:
    KeeperLink& _link;
};

} // namespace vthost::keeperlink
