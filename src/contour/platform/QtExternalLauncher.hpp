// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <contour/platform/ExternalLauncher.hpp>

#include <vtpty/ProcessPlacement.hpp>

#include <memory>

namespace contour::platform
{

/// The production ExternalLauncher: opens URLs through Qt's QDesktopServices::openUrl, and starts
/// programs detached -- on POSIX through vtpty::startDetached(), placed as a session's shell is; on
/// Windows through QProcess::startDetached.
///
/// Used where there is no xdg-desktop-portal to speak to -- every platform but Linux. On Linux,
/// PortalExternalLauncher is used instead, because Qt's openUrl() reaches the portal with a BLOCKING
/// call. @see makeExternalLauncher(), and PortalExternalLauncher for why that is not a sandbox-only
/// concern.
///
/// Its runDetached()/runDetachedWithInput() are also what PortalExternalLauncher delegates to:
/// starting a child process has nothing to do with the portal, and is identical on both paths.
///
/// It keeps no children: every program it starts without waiting is detached, so nothing it started
/// is ended when it goes.
class QtExternalLauncher final: public ExternalLauncher
{
  public:
    /// @param placement Places each program it starts before the program runs -- the placement the
    ///                  sessions use, so that nothing a session starts lives in Contour's own systemd
    ///                  unit. Required. Windows, which has none to offer, starts programs unplaced.
    explicit QtExternalLauncher(std::shared_ptr<vtpty::ProcessPlacement> placement);

    [[nodiscard]] std::expected<void, LaunchError> openUrl(QUrl const& url) override;
    [[nodiscard]] std::expected<void, SpawnError> runDetached(QString const& program,
                                                              QStringList const& arguments) override;
    [[nodiscard]] std::expected<void, SpawnError> runDetachedWithInput(
        std::string const& commandLine, std::filesystem::path const& inputFile) override;

  private:
    std::shared_ptr<vtpty::ProcessPlacement> _placement;
};

} // namespace contour::platform
